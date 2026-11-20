#include <telemetryhub/server/device_registration_registry.hpp>

namespace telemetryhub::server {

RegistrationResult DeviceRegistrationRegistry::register_device(
    const network::ConnectionId connection_id,
    const std::string_view device_id)
{
    return register_device_with_count(connection_id, device_id).result;
}

RegistrationSnapshot DeviceRegistrationRegistry::register_device_with_count(
    const network::ConnectionId connection_id,
    const std::string_view device_id)
{
    std::lock_guard lock{mutex_};
    bool connection_has_other_device = false;
    for (const auto& [registered_device, owner] : owners_)
    {
        if (owner == connection_id)
        {
            if (registered_device == device_id)
            {
                return {RegistrationResult::already_registered, owners_.size()};
            }
            connection_has_other_device = true;
        }
    }
    if (connection_has_other_device)
    {
        return {RegistrationResult::connection_already_registered, owners_.size()};
    }

    const auto found = owners_.find(device_id);
    if (found != owners_.end())
    {
        return {found->second == connection_id
                    ? RegistrationResult::already_registered
                    : RegistrationResult::device_in_use,
                owners_.size()};
    }

    owners_.emplace(std::string{device_id}, connection_id);
    return {RegistrationResult::registered, owners_.size()};
}

void DeviceRegistrationRegistry::erase_connection(const network::ConnectionId connection_id) noexcept
{
    static_cast<void>(erase_connection_with_count(connection_id));
}

DisconnectSnapshot DeviceRegistrationRegistry::erase_connection_with_count(
    const network::ConnectionId connection_id) noexcept
{
    std::lock_guard lock{mutex_};
    std::optional<std::string> removed_device;
    for (auto item = owners_.begin(); item != owners_.end();)
    {
        if (item->second == connection_id)
        {
            auto removed = owners_.extract(item++);
            removed_device.emplace(std::move(removed.key()));
        }
        else
        {
            ++item;
        }
    }
    return {std::move(removed_device), owners_.size()};
}

bool DeviceRegistrationRegistry::is_registered(const network::ConnectionId connection_id) const noexcept
{
    std::lock_guard lock{mutex_};
    for (const auto& [device_id, owner] : owners_)
    {
        static_cast<void>(device_id);
        if (owner == connection_id)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> DeviceRegistrationRegistry::device_for_connection(
    const network::ConnectionId connection_id) const
{
    std::lock_guard lock{mutex_};
    for (const auto& [device_id, owner] : owners_)
    {
        if (owner == connection_id)
        {
            return device_id;
        }
    }
    return std::nullopt;
}
std::size_t DeviceRegistrationRegistry::size() const noexcept
{
    std::lock_guard lock{mutex_};
    return owners_.size();
}

} // namespace telemetryhub::server
