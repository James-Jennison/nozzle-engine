#ifndef slic3r_DeviceWorkspaceState_hpp_
#define slic3r_DeviceWorkspaceState_hpp_

#include <array>
#include <cstddef>
#include <string>

namespace Slic3r {

// NativePrinter is the native Moonraker panel; it manages its own connection
// and is not a direct (endpoint-bound) page.
enum class DeviceWorkspacePage : unsigned char { SnapmakerControl = 0, LanCamera, NativePrinter, Count };

struct DeviceWorkspaceEndpoint
{
    std::string printer_id;
    std::string display_name;
    std::string host;
    bool        connected{false};
};

struct DeviceWorkspaceUpdate
{
    bool identity_changed{false};
    bool endpoint_changed{false};
    bool availability_changed{false};
    bool direct_pages_invalidated{false};
};

class DeviceWorkspaceState
{
public:
    DeviceWorkspaceUpdate update_authoritative(DeviceWorkspaceEndpoint endpoint);
    void                  select_page(DeviceWorkspacePage page);
    void                  set_outer_active(bool active);
    void                  suspend_direct_pages();

    DeviceWorkspacePage            selected_page() const { return m_selected_page; }
    const DeviceWorkspaceEndpoint& endpoint() const { return m_endpoint; }
    std::size_t                    generation() const { return m_generation; }
    bool                           outer_active() const { return m_outer_active; }
    bool                           endpoint_available() const;
    bool                           should_run(DeviceWorkspacePage page) const;

    static bool        is_direct_page(DeviceWorkspacePage page);
    static bool        is_safe_lan_host(const std::string& host);
    static std::string lan_host_from_address(const std::string& address);
    static std::string http_origin(const std::string& host);

private:
    DeviceWorkspaceEndpoint                                                m_endpoint;
    DeviceWorkspacePage                                                    m_selected_page{DeviceWorkspacePage::SnapmakerControl};
    std::array<bool, static_cast<std::size_t>(DeviceWorkspacePage::Count)> m_page_running{{true, false, false}};
    std::size_t                                                            m_generation{0};
    bool                                                                   m_outer_active{false};
};

} // namespace Slic3r

#endif
