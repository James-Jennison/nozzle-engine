#ifndef slic3r_NativePrinterPanel_hpp_
#define slic3r_NativePrinterPanel_hpp_

#include "libslic3r/PrinterSnapshot.hpp"
#include "slic3r/Utils/MoonrakerLive.hpp"

#include <wx/panel.h>
#include <wx/timer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxBoxSizer;
class wxButton;
class wxFlexGridSizer;
class wxGauge;
class wxSlider;
class wxStaticText;
class wxToggleButton;

namespace Slic3r::GUI {

/// The printer's LAN address for the selected printer preset: the
/// "print_host" typed into the printer settings (physical printer first).
/// Empty when unknown or not a private LAN address. Returns a host, not a URL.
std::string resolve_printer_lan_host();

/// Native "monitor first, intervene second" printer page: live camera and the
/// current job on the left, telemetry, quick controls and materials on the
/// right. Sections are driven by what the printer reports, so it adapts to
/// any Moonraker printer; narrow windows stack into one column.
class NativePrinterPanel final : public wxPanel
{
public:
    /// Called with (true, lan_host, webcam) when the camera should run and
    /// (false, ...) when it should stop; the owner supplies the camera view.
    using CameraFn = std::function<void(bool active, const std::string &lan_host, const PrinterPanel::Webcam &webcam)>;

    NativePrinterPanel(wxWindow *parent, CameraFn camera_fn);
    ~NativePrinterPanel() override;

    /// Parent for the camera view the owner creates.
    wxWindow *camera_host() const { return m_camera_host; }

    /// Connects while active (the page is visible), disconnects when not.
    void set_active(bool active);
    void on_sys_color_changed();

private:
    struct Palette;
    Palette palette() const;

    void build();
    void connect();
    void disconnect();
    void set_compact(bool compact);

    void on_link(MoonrakerLiveClient::Link link, const std::string &detail);
    void on_capabilities(const PrinterPanel::Capabilities &caps);
    void on_snapshot(const PrinterPanel::Snapshot &snapshot);
    void rebuild_controls();
    void rebuild_materials();
    void show_adjust_dialog();
    void show_toolhead_dialog(size_t index);
    void confirm_stop();
    void report_error(const std::string &what, const std::string &error);
    void apply_colors();

    // Defers a callback from the client thread to the GUI thread, dropped if
    // this panel or its client is gone by then.
    template<class F> void post(F &&fn);

    CameraFn                             m_camera_fn;
    std::unique_ptr<MoonrakerLiveClient> m_client;
    std::shared_ptr<std::atomic<NativePrinterPanel *>> m_lifetime;
    std::atomic<std::uint64_t>           m_client_generation{0};
    bool                                 m_active            = false;
    bool                                 m_compact           = false;
    bool                                 m_camera_running    = false;
    std::string                          m_host;

    PrinterPanel::Capabilities m_caps;
    PrinterPanel::Snapshot     m_snapshot;

    // Layout
    wxBoxSizer *m_columns     = nullptr;
    wxBoxSizer *m_left        = nullptr;
    wxBoxSizer *m_right       = nullptr;
    wxWindow   *m_camera_card = nullptr;
    wxWindow   *m_job_card    = nullptr;
    wxWindow   *m_camera_host = nullptr;

    // Header
    wxStaticText *m_printer_name = nullptr;
    wxStaticText *m_link_label   = nullptr;
    wxStaticText *m_health_label = nullptr;

    // Job
    wxStaticText *m_job_name      = nullptr;
    wxStaticText *m_job_detail    = nullptr;
    wxStaticText *m_job_percent   = nullptr;
    wxGauge      *m_job_progress  = nullptr;
    wxStaticText *m_job_remaining = nullptr;
    wxStaticText *m_job_finishes  = nullptr;
    wxStaticText *m_job_layer     = nullptr;
    wxButton     *m_pause_button  = nullptr;
    wxButton     *m_adjust_button = nullptr;
    wxButton     *m_stop_button   = nullptr;

    // Telemetry
    wxStaticText *m_nozzle_value  = nullptr;
    wxStaticText *m_nozzle_label  = nullptr;
    wxStaticText *m_bed_value     = nullptr;
    wxStaticText *m_chamber_value = nullptr;
    wxStaticText *m_speed_value   = nullptr;
    wxStaticText *m_flow_value    = nullptr;

    // Controls and materials, rebuilt when capabilities change
    wxWindow        *m_controls_card = nullptr;
    wxFlexGridSizer *m_controls_grid = nullptr;
    wxWindow        *m_material_card = nullptr;
    wxBoxSizer      *m_material_row  = nullptr;
    wxStaticText    *m_material_note = nullptr;

    struct FanRow
    {
        PrinterPanel::Control control;
        bool                  part_fan = false;
        wxSlider             *slider   = nullptr;
        wxStaticText         *value    = nullptr;
        long                  hold_until_ms = 0; ///< ignore echoes right after a user change
    };
    std::vector<FanRow> m_fan_rows;
    struct LightRow
    {
        PrinterPanel::Control control;
        wxToggleButton       *toggle = nullptr;
    };
    std::vector<LightRow> m_light_rows;
    struct MaterialTile
    {
        wxWindow     *panel = nullptr;
        wxWindow     *swatch = nullptr;
        wxStaticText *name  = nullptr;
        wxStaticText *detail = nullptr;
    };
    std::vector<MaterialTile> m_material_tiles;
};

} // namespace Slic3r::GUI

#endif // slic3r_NativePrinterPanel_hpp_
