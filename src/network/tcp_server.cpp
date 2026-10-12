#include <telemetryhub/network/tcp_server.hpp>

#include "client_session.hpp"

#include <telemetryhub/network/tcp.hpp>
#include <telemetryhub/network/winsock_runtime.hpp>
#include <telemetryhub/network/winsock_error.hpp>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace telemetryhub::network {

class TcpServer::Impl
{
public:
    explicit Impl(const TcpServerConfig config)
        : config_{config}
    {
        if (config_.max_connections == 0U)
        {
            throw std::invalid_argument{"TcpServer max_connections must be positive"};
        }
        if (config_.max_connections > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            throw std::invalid_argument{"TcpServer max_connections exceeds the Winsock listen backlog range"};
        }
        if (config_.max_outbound_buffer_size_bytes == 0U)
        {
            throw std::invalid_argument{"TcpServer max outbound buffer size must be positive"};
        }
        if (config_.outbound_queue_capacity == 0U)
        {
            throw std::invalid_argument{"TcpServer outbound queue capacity must be positive"};
        }
        if (config_.terminal_flush_timeout.count() <= 0)
        {
            throw std::invalid_argument{"TcpServer terminal flush timeout must be positive"};
        }
    }

    ~Impl() noexcept
    {
        shutdown();
    }

    void start()
    {
        std::unique_lock lifecycle_lock{lifecycle_mutex_};
        lifecycle_changed_.wait(lifecycle_lock, [this] { return !stopping_; });
        if (started_)
        {
            throw std::logic_error{"TcpServer can only be started while stopped"};
        }

        Socket listener = create_ipv4_tcp_socket();
        if (const std::error_code error = listener.set_nonblocking(true))
        {
            throw std::system_error{error, "Failed to set TCP listener nonblocking"};
        }
        bind_loopback(listener, config_.port);
        listen(listener, static_cast<int>(config_.max_connections));
        bound_port_ = local_port(listener);
        listener_ = std::move(listener);

        try
        {
            running_.store(true);
            accept_thread_ = std::jthread{[this](const std::stop_token token) {
                try
                {
                    accept_loop(token);
                }
                catch (...)
                {
                    running_.store(false);
                }
            }};
            {
                std::lock_guard enqueue_lock{enqueue_mutex_};
                accepting_enqueues_ = true;
            }
            started_ = true;
        }
        catch (...)
        {
            running_.store(false);
            static_cast<void>(listener_.close());
            throw;
        }
    }

    void shutdown() noexcept
    {
        std::unique_lock lifecycle_lock{lifecycle_mutex_};
        if (stopping_)
        {
            lifecycle_changed_.wait(lifecycle_lock, [this] { return !stopping_; });
            return;
        }
        if (!started_)
        {
            return;
        }

        // Close admission before publishing the stopping state. No enqueue can
        // be admitted after this transition.
        {
            std::lock_guard enqueue_lock{enqueue_mutex_};
            accepting_enqueues_ = false;
        }
        stopping_ = true;
        running_.store(false);
        lifecycle_lock.unlock();

        if (accept_thread_.joinable())
        {
            accept_thread_.request_stop();
            accept_thread_.join();
        }

        static_cast<void>(listener_.close());

        std::map<std::uint64_t, std::shared_ptr<ClientSession>> sessions;
        {
            std::lock_guard registry_lock{registry_mutex_};
            sessions.swap(sessions_);
        }

        for (const auto& [connection_id, session] : sessions)
        {
            static_cast<void>(connection_id);
            session->request_stop();
        }
        for (const auto& [connection_id, session] : sessions)
        {
            static_cast<void>(connection_id);
            session->join();
            record_shutdown_error(session);
        }

        // Closing session queues wakes admitted enqueue calls blocked on a full
        // queue. Keep the local session owners until all such calls have left.
        {
            std::unique_lock enqueue_lock{enqueue_mutex_};
            enqueue_changed_.wait(enqueue_lock, [this] { return in_flight_enqueues_ == 0U; });
        }
        sessions.clear();

        {
            std::lock_guard state_lock{lifecycle_mutex_};
            started_ = false;
            stopping_ = false;
        }
        lifecycle_changed_.notify_all();
    }

    [[nodiscard]] bool is_running() const noexcept
    {
        return running_.load();
    }

    [[nodiscard]] std::uint16_t bound_port() const
    {
        std::lock_guard lifecycle_lock{lifecycle_mutex_};
        if (bound_port_ == 0U)
        {
            throw std::logic_error{"TcpServer has not been started"};
        }
        return bound_port_;
    }

    [[nodiscard]] std::size_t active_connection_count() const noexcept
    {
        std::lock_guard registry_lock{registry_mutex_};
        return sessions_.size();
    }

    [[nodiscard]] std::vector<ConnectionId> connection_ids() const
    {
        std::vector<ConnectionId> result;
        std::lock_guard registry_lock{registry_mutex_};
        result.reserve(sessions_.size());
        for (const auto& [connection_id, session] : sessions_)
        {
            static_cast<void>(session);
            result.emplace_back(connection_id);
        }
        return result;
    }

    [[nodiscard]] EnqueueStatus enqueue(
        const ConnectionId connection_id,
        std::vector<std::byte> bytes,
        const std::stop_token stop_token)
    {
        if (!begin_enqueue())
        {
            return EnqueueStatus::closed;
        }
        EnqueueGuard operation{*this};

        std::shared_ptr<ClientSession> session;
        {
            std::lock_guard registry_lock{registry_mutex_};
            const auto found = sessions_.find(connection_id.value());
            if (found == sessions_.end())
            {
                return EnqueueStatus::unknown_connection;
            }
            session = found->second;
        }

        return session->enqueue(std::move(bytes), stop_token);
    }

    [[nodiscard]] std::error_code shutdown_error() const noexcept
    {
        const int error = shutdown_error_code_.load();
        return error == 0 ? std::error_code{} : make_winsock_error_code(error);
    }

private:
    class EnqueueGuard
    {
    public:
        explicit EnqueueGuard(Impl& owner) noexcept : owner_{owner} {}
        ~EnqueueGuard() noexcept { owner_.finish_enqueue(); }

        EnqueueGuard(const EnqueueGuard&) = delete;
        EnqueueGuard& operator=(const EnqueueGuard&) = delete;

    private:
        Impl& owner_;
    };

    [[nodiscard]] bool begin_enqueue() noexcept
    {
        std::lock_guard enqueue_lock{enqueue_mutex_};
        if (!accepting_enqueues_)
        {
            return false;
        }
        ++in_flight_enqueues_;
        return true;
    }

    void finish_enqueue() noexcept
    {
        bool became_idle = false;
        {
            std::lock_guard enqueue_lock{enqueue_mutex_};
            --in_flight_enqueues_;
            became_idle = in_flight_enqueues_ == 0U;
        }
        if (became_idle)
        {
            enqueue_changed_.notify_all();
        }
    }

    void record_shutdown_error(const std::shared_ptr<ClientSession>& session) noexcept
    {
        const std::error_code error = session->shutdown_error();
        if (error)
        {
            int expected = 0;
            static_cast<void>(shutdown_error_code_.compare_exchange_strong(expected, error.value()));
        }
    }

    void accept_loop(const std::stop_token stop_token)
    {
        while (!stop_token.stop_requested())
        {
            reap_completed_sessions();
            const SocketWaitResult ready = wait_for_socket(
                listener_, SocketWaitInterest::read, stop_token);
            if (ready.status == SocketWaitStatus::cancelled)
            {
                break;
            }
            if (ready.status == SocketWaitStatus::timeout)
            {
                continue;
            }
            if (ready.status == SocketWaitStatus::error)
            {
                break;
            }

            Socket accepted;
            try
            {
                accepted = accept(listener_);
            }
            catch (const std::system_error& error)
            {
                const int code = error.code().value();
                if (code == WSAEWOULDBLOCK || code == WSAEINTR ||
                    code == WSAECONNABORTED || code == WSAECONNRESET)
                {
                    continue;
                }
                if (stop_token.stop_requested() || code == WSAENOTSOCK)
                {
                    break;
                }
                break;
            }

            if (stop_token.stop_requested())
            {
                break;
            }
            if (const std::error_code error = accepted.set_nonblocking(true))
            {
                continue;
            }

            std::uint64_t id_value = 0U;
            {
                std::lock_guard registry_lock{registry_mutex_};
                if (sessions_.size() >= config_.max_connections || ids_exhausted_)
                {
                    continue;
                }
                id_value = next_id_;
                if (next_id_ == (std::numeric_limits<std::uint64_t>::max)())
                {
                    ids_exhausted_ = true;
                }
                else
                {
                    ++next_id_;
                }
            }

            SessionCallbacks callbacks;
            try
            {
                if (config_.session_callbacks_factory)
                {
                    callbacks = config_.session_callbacks_factory(ConnectionId{id_value});
                }
            }
            catch (...)
            {
                continue;
            }

            auto session = std::make_shared<ClientSession>(
                ConnectionId{id_value}, std::move(accepted), config_.outbound_queue_capacity,
                config_.max_outbound_buffer_size_bytes, config_.terminal_flush_timeout,
                std::move(callbacks));
            session->start();
            {
                std::lock_guard registry_lock{registry_mutex_};
                sessions_.emplace(id_value, std::move(session));
            }
        }
        reap_completed_sessions();
        running_.store(false);
    }

    void reap_completed_sessions()
    {
        std::vector<std::shared_ptr<ClientSession>> completed;
        {
            std::lock_guard registry_lock{registry_mutex_};
            for (auto item = sessions_.begin(); item != sessions_.end();)
            {
                if (item->second->completed())
                {
                    completed.push_back(std::move(item->second));
                    item = sessions_.erase(item);
                }
                else
                {
                    ++item;
                }
            }
        }

        for (const auto& session : completed)
        {
            session->request_stop();
            session->join();
            record_shutdown_error(session);
        }
    }

    // Lock order: lifecycle_mutex_ may precede enqueue_mutex_ during start and
    // shutdown admission transitions. Enqueue releases enqueue_mutex_ before
    // acquiring registry_mutex_; registry_mutex_ is never nested with lifecycle
    // or enqueue locks. No registry/lifecycle lock spans a blocking operation.
    WinsockRuntime winsock_runtime_;
    TcpServerConfig config_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    mutable std::mutex registry_mutex_;
    std::mutex enqueue_mutex_;
    std::condition_variable enqueue_changed_;
    bool accepting_enqueues_{false};
    std::size_t in_flight_enqueues_{0U};
    Socket listener_;
    std::map<std::uint64_t, std::shared_ptr<ClientSession>> sessions_;
    std::jthread accept_thread_;
    std::uint64_t next_id_{1U};
    std::uint16_t bound_port_{0U};
    bool ids_exhausted_{false};
    bool started_{false};
    bool stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<int> shutdown_error_code_{0};
};

TcpServer::TcpServer(const TcpServerConfig config)
    : impl_{std::make_unique<Impl>(config)}
{
}

TcpServer::~TcpServer() noexcept = default;

void TcpServer::start()
{
    impl_->start();
}

void TcpServer::shutdown() noexcept
{
    impl_->shutdown();
}

std::error_code TcpServer::shutdown_error() const noexcept
{
    return impl_->shutdown_error();
}

bool TcpServer::is_running() const noexcept
{
    return impl_->is_running();
}

std::uint16_t TcpServer::bound_port() const
{
    return impl_->bound_port();
}

std::size_t TcpServer::active_connection_count() const noexcept
{
    return impl_->active_connection_count();
}

std::vector<ConnectionId> TcpServer::connection_ids() const
{
    return impl_->connection_ids();
}

EnqueueStatus TcpServer::enqueue(
    const ConnectionId connection_id,
    std::vector<std::byte> bytes,
    const std::stop_token stop_token)
{
    return impl_->enqueue(connection_id, std::move(bytes), stop_token);
}

} // namespace telemetryhub::network