#include "NativeDevicePage.hpp"

#include "DirectLanPage.hpp"
#include "NativePrinterPanel.hpp"

#include <wx/sizer.h>

namespace Slic3r::GUI {

NativeDevicePage::NativeDevicePage(wxWindow* parent) : wxPanel(parent)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_panel = new NativePrinterPanel(this, [this](bool active, const std::string& lan_host, const PrinterPanel::Webcam& webcam) {
        if (m_camera == nullptr)
            return;
        if (!active || lan_host.empty()) {
            m_camera->deactivate();
            return;
        }
        // An MJPEG webcam without stream_url streams at the camera service's default path.
        std::string mjpeg = webcam.webrtc ? std::string() : webcam.stream_url;
        if (!webcam.webrtc && mjpeg.empty())
            mjpeg = "/webcam/stream.mjpg";
        m_camera->set_mjpeg_only(mjpeg);
        m_camera->activate(DeviceWorkspaceEndpoint{"native-printer", std::string(), lan_host, true}, ++m_camera_generation);
    });
    m_camera = new DirectLanPage(m_panel->camera_host());
    m_camera->set_embedded(true);
    m_panel->camera_host()->GetSizer()->Add(m_camera, 1, wxEXPAND);

    sizer->Add(m_panel, 1, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {
        set_active(evt.IsShown());
        evt.Skip();
    });
}

NativeDevicePage::~NativeDevicePage() = default;

void NativeDevicePage::set_active(bool active)
{
    m_active = active;
    m_panel->set_active(active);
}

void NativeDevicePage::reload()
{
    // Drop the connection to the previous printer and reconnect to the new one.
    m_panel->set_active(false);
    m_panel->set_active(m_active);
}

void NativeDevicePage::on_sys_color_changed() { m_panel->on_sys_color_changed(); }

void NativeDevicePage::msw_rescale() { m_panel->on_sys_color_changed(); }

} // namespace Slic3r::GUI
