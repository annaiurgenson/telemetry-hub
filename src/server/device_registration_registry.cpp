#include <telemetryhub/server/device_registration_registry.hpp>

namespace telemetryhub::server {

RegistrationResult DeviceRegistrationRegistry::register_device(
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
                return RegistrationResult::already_registered;
            }
            connection_has_other_device = true;
        }
    }
    if (connection_has_other_device)
    {
        return RegistrationResult::connection_already_registered;
    }

    const auto found = owners_.find(device_id);
    if (found != owners_.end())
    {
        return found->second == connection_id
                   ? RegistrationResult::already_registered
                   : RegistrationResult::device_in_use;
    }

    owners_.emplace(std::string{device_id}, connection_id);
    return RegistrationResult::registered;
}

void DeviceRegistrationRegistry::erase_connection(const network::ConnectionId connection_id) noexcept
{
    std::lock_guard lock{mutex_};
    for (auto item = owners_.begin(); item != owners_.end();)
    {
        if (item->second == connection_id)
        {
            item = owners_.erase(item);
        }
        else
        {
            ++item;
        }
    }
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

std::size_t DeviceRegistrationRegistry::size() const noexcept
{
    std::lock_guard lock{mutex_};
    return owners_.size();
}

} // namespace telemetryhub::server