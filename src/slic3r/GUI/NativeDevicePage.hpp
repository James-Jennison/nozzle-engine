#ifndef slic3r_GUI_NativeDevicePage_hpp_
#define slic3r_GUI_NativeDevicePage_hpp_

#include <cstddef>

#include <wx/panel.h>

namespace Slic3r::GUI {

class DirectLanPage;
class NativePrinterPanel;

/// "Printer" tab for Moonraker/Klipper printers: the native monitor-first printer
/// panel (job, telemetry, quick controls, materials) with a live camera view. It
/// connects while the tab is visible and disconnects when it is hidden.
class NativeDevicePage final : public wxPanel
{
public:
    explicit NativeDevicePage(wxWindow* parent);
    ~NativeDevicePage() override;

    /// Reconnect to the selected printer's address (call when the printer preset changes).
    void reload();
    void set_active(bool active);
    void on_sys_color_changed();
    void msw_rescale();

private:
    NativePrinterPanel* m_panel{nullptr};
    DirectLanPage*      m_camera{nullptr};
    std::size_t         m_camera_generation{0};
    bool                m_active{false};
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_NativeDevicePage_hpp_
