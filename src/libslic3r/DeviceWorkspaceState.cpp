#include "DeviceWorkspaceState.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <vector>

namespace Slic3r {

namespace {

bool parse_ipv4(const std::string& host, std::array<unsigned int, 4>& octets)
{
    std::istringstream input(host);
    std::string        part;
    std::size_t        index = 0;
    while (std::getline(input, part, '.')) {
        if (index >= octets.size() || part.empty() || part.size() > 3)
            return false;
        if (!std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return false;
        const unsigned long value = std::stoul(part);
        if (value > 255 || (part.size() > 1 && part.front() == '0'))
            return false;
        octets[index++] = static_cast<unsigned int>(value);
    }
    return index == octets.size() && host.back() != '.';
}

bool is_private_ipv4(const std::array<unsigned int, 4>& octets)
{
    return octets[0] == 10 || octets[0] == 127 || (octets[0] == 169 && octets[1] == 254) ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) || (octets[0] == 192 && octets[1] == 168);
}

bool is_valid_port(const std::string& port)
{
    if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;
    const unsigned long value = std::stoul(port);
    return value > 0 && value <= 65535;
}

bool is_private_ipv6(std::string host)
{
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (host.empty() || host.size() > 39 || std::count(host.begin(), host.end(), ':') < 2 ||
        !std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isxdigit(c) != 0 || c == ':'; }))
        return false;
    const std::size_t compression = host.find("::");
    if (compression != std::string::npos && host.find("::", compression + 2) != std::string::npos)
        return false;
    std::istringstream groups(host);
    std::string        group;
    while (std::getline(groups, group, ':')) {
        if (group.size() > 4)
            return false;
    }
    if (host == "::1")
        return true;
    return host.rfind("fc", 0) == 0 || host.rfind("fd", 0) == 0 || host.rfind("fe8", 0) == 0 || host.rfind("fe9", 0) == 0 ||
           host.rfind("fea", 0) == 0 || host.rfind("feb", 0) == 0;
}

} // namespace

DeviceWorkspaceUpdate DeviceWorkspaceState::update_authoritative(DeviceWorkspaceEndpoint endpoint)
{
    if (!is_safe_lan_host(endpoint.host))
        endpoint.host.clear();
    if (endpoint.printer_id.empty()) {
        endpoint.host.clear();
        endpoint.connected = false;
    }

    DeviceWorkspaceUpdate update;
    update.identity_changed         = endpoint.printer_id != m_endpoint.printer_id;
    update.endpoint_changed         = endpoint.host != m_endpoint.host;
    update.availability_changed     = endpoint.connected != m_endpoint.connected;
    update.direct_pages_invalidated = update.identity_changed || update.endpoint_changed || update.availability_changed;

    if (update.direct_pages_invalidated) {
        suspend_direct_pages();
        ++m_generation;
    }

    m_endpoint = std::move(endpoint);
    if (m_outer_active && endpoint_available() && is_direct_page(m_selected_page))
        m_page_running[static_cast<std::size_t>(m_selected_page)] = true;
    return update;
}

void DeviceWorkspaceState::select_page(DeviceWorkspacePage page)
{
    if (page >= DeviceWorkspacePage::Count)
        page = DeviceWorkspacePage::SnapmakerControl;
    suspend_direct_pages();
    m_selected_page = page;
    if (m_outer_active && endpoint_available() && is_direct_page(page))
        m_page_running[static_cast<std::size_t>(page)] = true;
}

void DeviceWorkspaceState::set_outer_active(bool active)
{
    m_outer_active = active;
    if (!active) {
        suspend_direct_pages();
        return;
    }
    if (endpoint_available() && is_direct_page(m_selected_page))
        m_page_running[static_cast<std::size_t>(m_selected_page)] = true;
}

void DeviceWorkspaceState::suspend_direct_pages()
{
    m_page_running[static_cast<std::size_t>(DeviceWorkspacePage::LanCamera)] = false;
}

bool DeviceWorkspaceState::endpoint_available() const
{
    return m_endpoint.connected && !m_endpoint.printer_id.empty() && is_safe_lan_host(m_endpoint.host);
}

bool DeviceWorkspaceState::should_run(DeviceWorkspacePage page) const
{
    const auto index = static_cast<std::size_t>(page);
    return index < m_page_running.size() && m_page_running[index];
}

bool DeviceWorkspaceState::is_direct_page(DeviceWorkspacePage page)
{
    return page == DeviceWorkspacePage::LanCamera;
}

bool DeviceWorkspaceState::is_safe_lan_host(const std::string& host)
{
    if (host.empty() || host.size() > 253 || host.find_first_of("/@?#%\\\t\r\n ") != std::string::npos)
        return false;

    std::array<unsigned int, 4> octets{};
    if (parse_ipv4(host, octets))
        return is_private_ipv4(octets);
    if (host.find(':') != std::string::npos)
        return is_private_ipv6(host);

    return false;
}

std::string DeviceWorkspaceState::lan_host_from_address(const std::string& address)
{
    if (is_safe_lan_host(address))
        return address;

    if (address.size() > 2 && address.front() == '[') {
        const std::size_t closing = address.find(']');
        if (closing == std::string::npos)
            return {};
        const std::string host = address.substr(1, closing - 1);
        const std::string tail = address.substr(closing + 1);
        if (is_safe_lan_host(host) && (tail.empty() || (tail.front() == ':' && is_valid_port(tail.substr(1)))))
            return host;
        return {};
    }

    if (std::count(address.begin(), address.end(), ':') == 1) {
        const std::size_t separator = address.find(':');
        const std::string host      = address.substr(0, separator);
        if (is_safe_lan_host(host) && is_valid_port(address.substr(separator + 1)))
            return host;
    }
    return {};
}

std::string DeviceWorkspaceState::http_origin(const std::string& host)
{
    if (!is_safe_lan_host(host))
        return {};
    if (host.find(':') != std::string::npos && host.front() != '[')
        return "http://[" + host + "]";
    return "http://" + host;
}

} // namespace Slic3r
