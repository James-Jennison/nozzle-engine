#include "NativePrinterPanel.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "CreatePresetsDialog.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/DeviceWorkspaceState.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/U1PrintTask.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/clrpicker.h>
#include <wx/combobox.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

#include <algorithm>
#include <ctime>

namespace Slic3r::GUI {

namespace PP = PrinterPanel;

// ---------------------------------------------------------------------------
// Printer address

std::string resolve_printer_lan_host()
{
    auto *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};

    std::string address;
    if (const DynamicPrintConfig *physical = bundle->physical_printers.get_selected_printer_config())
        address = physical->opt_string("print_host");
    const DynamicPrintConfig &printer = bundle->printers.get_edited_preset().config;
    if (address.empty())
        address = printer.opt_string("print_host");

    // Accept "http://host[:port]/..." or a bare host; LAN addresses only.
    std::string host = address;
    if (const auto scheme = host.find("://"); scheme != std::string::npos)
        host = host.substr(scheme + 3);
    if (const auto slash = host.find('/'); slash != std::string::npos)
        host = host.substr(0, slash);
    const std::string bare = DeviceWorkspaceState::lan_host_from_address(host);
    return bare.empty() ? std::string() : host;
}

// ---------------------------------------------------------------------------
// Visual helpers

struct NativePrinterPanel::Palette
{
    wxColour page, card, card_border, tile, text, muted, accent, danger, success, warning;
};

NativePrinterPanel::Palette NativePrinterPanel::palette() const
{
    if (wxGetApp().dark_mode())
        return {wxColour(0x16, 0x18, 0x1B), wxColour(0x1F, 0x22, 0x26), wxColour(0x34, 0x38, 0x3E), wxColour(0x2A, 0x2E, 0x33),
                wxColour(0xE8, 0xEA, 0xED), wxColour(0x9A, 0xA0, 0xA6), wxColour(0x6C, 0xA6, 0xFF), wxColour(0xFF, 0x6B, 0x6B),
                wxColour(0x4A, 0xD6, 0x8C), wxColour(0xFF, 0xC1, 0x4D)};
    return {wxColour(0xF3, 0xF4, 0xF6), wxColour(0xFF, 0xFF, 0xFF), wxColour(0xDD, 0xE0, 0xE4), wxColour(0xF1, 0xF3, 0xF5),
            wxColour(0x1F, 0x23, 0x28), wxColour(0x5F, 0x66, 0x6D), wxColour(0x25, 0x63, 0xEB), wxColour(0xC6, 0x28, 0x28),
            wxColour(0x16, 0x7D, 0x4A), wxColour(0x9A, 0x5B, 0x00)};
}

namespace {

// A rounded card with a title row; children go in body().
class Card : public wxPanel
{
public:
    Card(wxWindow *parent, const wxString &title, const wxString &note = {}) : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        auto *outer = new wxBoxSizer(wxVERTICAL);
        auto *head  = new wxBoxSizer(wxHORIZONTAL);
        m_title     = new wxStaticText(this, wxID_ANY, title);
        m_title->SetFont(m_title->GetFont().Bold());
        m_note = new wxStaticText(this, wxID_ANY, note);
        head->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
        head->AddStretchSpacer();
        head->Add(m_note, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(head, 0, wxEXPAND | wxALL, FromDIP(10));
        m_body = new wxBoxSizer(wxVERTICAL);
        outer->Add(m_body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        SetSizer(outer);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.SetPen(wxPen(m_border, FromDIP(1)));
            dc.DrawRoundedRectangle(wxRect(GetClientSize()).Deflate(1), FromDIP(10));
        });
    }
    wxBoxSizer   *body() const { return m_body; }
    wxStaticText *note() const { return m_note; }
    wxStaticText *title() const { return m_title; }
    void          set_border(const wxColour &c) { m_border = c; Refresh(); }

private:
    wxStaticText *m_title = nullptr;
    wxStaticText *m_note  = nullptr;
    wxBoxSizer   *m_body  = nullptr;
    wxColour      m_border;
};

wxStaticText *label(wxWindow *parent, const wxString &text, double scale = 1.0, bool bold = false)
{
    auto  *t    = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = t->GetFont();
    if (scale != 1.0)
        font.SetFractionalPointSize(font.GetFractionalPointSize() * scale);
    if (bold)
        font.MakeBold();
    t->SetFont(font);
    return t;
}

wxString temp_text(const PP::Heater &h)
{
    if (h.target > 0.)
        return wxString::Format("%.0f° / %.0f°", h.temperature, h.target);
    return wxString::Format("%.0f° / off", h.temperature);
}

// wxStaticText pins its minimum size to the text extent at creation; fonts
// applied later (bold/larger labels, the app's dark-mode/DPI pass) would then
// be clipped. Let sizers use the current best size instead.
void unpin_text_sizes(wxWindow *root)
{
    for (wxWindow *child : root->GetChildren()) {
        if (dynamic_cast<wxStaticText *>(child) != nullptr) {
            child->SetMinSize(wxDefaultSize);
            child->InvalidateBestSize();
        }
        unpin_text_sizes(child);
    }
}

long now_ms() { return static_cast<long>(wxGetLocalTimeMillis().GetValue()); }

wxString finish_clock(double remaining)
{
    const std::time_t at = std::time(nullptr) + static_cast<std::time_t>(remaining);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%I:%M %p", std::localtime(&at));
    wxString s = wxString::FromUTF8(buf);
    if (s.StartsWith("0"))
        s.Remove(0, 1);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------

template<class F> void NativePrinterPanel::post(F &&fn)
{
    const auto          lifetime   = m_lifetime;
    const std::uint64_t generation = m_client_generation;
    wxGetApp().CallAfter([lifetime, generation, fn = std::forward<F>(fn)]() mutable {
        NativePrinterPanel *self = lifetime->load();
        if (self != nullptr && self->m_client_generation == generation)
            fn(self);
    });
}

NativePrinterPanel::NativePrinterPanel(wxWindow *parent, CameraFn camera_fn)
    : wxPanel(parent), m_camera_fn(std::move(camera_fn)), m_lifetime(std::make_shared<std::atomic<NativePrinterPanel *>>(this))
{
    build();
    apply_colors();
    Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        set_compact(e.GetSize().GetWidth() < FromDIP(900));
        e.Skip();
    });
}

NativePrinterPanel::~NativePrinterPanel()
{
    m_lifetime->store(nullptr);
    disconnect();
}

void NativePrinterPanel::build()
{
    auto *outer = new wxBoxSizer(wxVERTICAL);

    // Header: identity and connection on the left, health on the right.
    auto *header   = new wxBoxSizer(wxHORIZONTAL);
    m_printer_name = label(this, _L("Printer"), 1.15, true);
    m_link_label   = label(this, _L("Not connected"));
    m_health_label = label(this, wxEmptyString);
    header->Add(m_printer_name, 0, wxALIGN_CENTER_VERTICAL);
    header->Add(m_link_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->AddStretchSpacer();
    header->Add(m_health_label, 0, wxALIGN_CENTER_VERTICAL);
    outer->Add(header, 0, wxEXPAND | wxALL, FromDIP(12));

    m_columns = new wxBoxSizer(wxHORIZONTAL);
    m_left    = new wxBoxSizer(wxVERTICAL);
    m_right   = new wxBoxSizer(wxVERTICAL);

    // Camera
    auto *camera  = new Card(this, _L("Live view"));
    m_camera_card = camera;
    m_camera_host = new wxPanel(camera);
    m_camera_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_camera_host->SetMinSize(FromDIP(wxSize(360, 240)));
    camera->body()->Add(m_camera_host, 1, wxEXPAND);
    m_left->Add(camera, 1, wxEXPAND | wxBOTTOM, FromDIP(12));

    // Job
    auto *job  = new Card(this, wxEmptyString);
    m_job_card = job;
    auto *top  = new wxBoxSizer(wxHORIZONTAL);
    auto *name = new wxBoxSizer(wxVERTICAL);
    m_job_name    = label(job, _L("No active print"), 1.1, true);
    m_job_detail  = label(job, wxEmptyString);
    m_job_percent = label(job, wxEmptyString, 2.0);
    name->Add(m_job_name, 0, wxEXPAND);
    name->Add(m_job_detail, 0, wxTOP, FromDIP(2));
    top->Add(name, 1, wxALIGN_CENTER_VERTICAL);
    top->Add(m_job_percent, 0, wxALIGN_CENTER_VERTICAL);
    job->body()->Add(top, 0, wxEXPAND);
    m_job_progress = new wxGauge(job, wxID_ANY, 1000, wxDefaultPosition, wxSize(-1, FromDIP(8)));
    job->body()->Add(m_job_progress, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));

    auto *facts = new wxFlexGridSizer(2, 3, FromDIP(2), FromDIP(24));
    for (int i = 0; i < 3; ++i)
        facts->AddGrowableCol(i);
    facts->Add(label(job, _L("Remaining")));
    facts->Add(label(job, _L("Finishes")));
    facts->Add(label(job, _L("Current layer")));
    m_job_remaining = label(job, "-", 1.05, true);
    m_job_finishes  = label(job, "-", 1.05, true);
    m_job_layer     = label(job, "-", 1.05, true);
    facts->Add(m_job_remaining);
    facts->Add(m_job_finishes);
    facts->Add(m_job_layer);
    job->body()->Add(facts, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    // Pause is the prominent action; Stop sits apart on the right.
    auto *actions   = new wxBoxSizer(wxHORIZONTAL);
    m_pause_button  = new wxButton(job, wxID_ANY, _L("Pause"));
    m_adjust_button = new wxButton(job, wxID_ANY, _L("Adjust"));
    m_stop_button   = new wxButton(job, wxID_ANY, _L("Stop print"));
    actions->Add(m_pause_button, 0, wxRIGHT, FromDIP(8));
    actions->Add(m_adjust_button, 0);
    actions->AddStretchSpacer();
    actions->Add(m_stop_button, 0);
    job->body()->Add(actions, 0, wxEXPAND);
    m_left->Add(job, 0, wxEXPAND);

    m_pause_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (!m_client)
            return;
        auto on_error = [this](const std::string &e) { post([e](NativePrinterPanel *self) { self->report_error("pause/resume", e); }); };
        if (m_snapshot.job_state == PP::JobState::Paused)
            m_client->resume(on_error);
        else
            m_client->pause(on_error);
    });
    m_adjust_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_adjust_dialog(); });
    m_stop_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { confirm_stop(); });

    // Live essentials
    auto *essentials = new Card(this, _L("Live essentials"), _L("Current / target"));
    auto *grid       = new wxFlexGridSizer(2, FromDIP(10), FromDIP(16));
    grid->AddGrowableCol(0);
    grid->AddGrowableCol(1);
    auto add_stat = [&](const wxString &title, wxStaticText *&value, wxStaticText **title_out = nullptr) {
        auto *cell = new wxBoxSizer(wxVERTICAL);
        auto *t    = label(essentials, title);
        value      = label(essentials, "-", 1.35);
        cell->Add(t);
        cell->Add(value);
        grid->Add(cell, 1, wxEXPAND);
        if (title_out)
            *title_out = t;
    };
    add_stat(_L("Nozzle"), m_nozzle_value, &m_nozzle_label);
    add_stat(_L("Bed"), m_bed_value);
    add_stat(_L("Speed"), m_speed_value);
    add_stat(_L("Flow"), m_flow_value);
    add_stat(_L("Chamber"), m_chamber_value);
    essentials->body()->Add(grid, 0, wxEXPAND);
    m_right->Add(essentials, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    // Quick controls (filled from capabilities)
    auto *controls  = new Card(this, _L("Quick controls"));
    m_controls_card = controls;
    m_controls_grid = new wxFlexGridSizer(3, FromDIP(8), FromDIP(10));
    m_controls_grid->AddGrowableCol(1);
    controls->body()->Add(m_controls_grid, 0, wxEXPAND);
    m_right->Add(controls, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    // Material (multi-toolhead printers)
    auto *material  = new Card(this, _L("Material"));
    m_material_card = material;
    m_material_note = material->note();
    m_material_row  = new wxBoxSizer(wxHORIZONTAL);
    material->body()->Add(m_material_row, 0, wxEXPAND);
    m_right->Add(material, 0, wxEXPAND);

    m_columns->Add(m_left, 3, wxEXPAND | wxRIGHT, FromDIP(12));
    m_columns->Add(m_right, 2, wxEXPAND);
    outer->Add(m_columns, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizer(outer);

    on_snapshot(m_snapshot);
    rebuild_controls();
    rebuild_materials();
}

void NativePrinterPanel::set_compact(bool compact)
{
    if (compact == m_compact || m_columns == nullptr)
        return;
    m_compact = compact;
    // Narrow: job summary, camera, telemetry, controls, materials in one column.
    m_columns->SetOrientation(compact ? wxVERTICAL : wxHORIZONTAL);
    m_left->Detach(m_camera_card);
    m_left->Detach(m_job_card);
    if (compact) {
        m_left->Add(m_job_card, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        m_left->Add(m_camera_card, 1, wxEXPAND);
    } else {
        m_left->Add(m_camera_card, 1, wxEXPAND | wxBOTTOM, FromDIP(12));
        m_left->Add(m_job_card, 0, wxEXPAND);
    }
    if (auto *item = m_columns->GetItem(m_left))
        item->SetFlag(compact ? (wxEXPAND | wxBOTTOM) : (wxEXPAND | wxRIGHT));
    Layout();
}

// ---------------------------------------------------------------------------
// Connection

void NativePrinterPanel::set_active(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active)
        connect();
    else
        disconnect();
}

void NativePrinterPanel::connect()
{
    disconnect();
    m_host = resolve_printer_lan_host();

    const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const std::string model = printer.has("printer_model") ? printer.opt_string("printer_model") : std::string();
    m_printer_name->SetLabel(model.empty() ? _L("Printer") : wxString::FromUTF8(model));

    if (m_host.empty()) {
        m_link_label->SetLabel(_L("No LAN address"));
        m_health_label->SetLabel(_L("Enter the printer's IP address in the printer settings (Connection) to monitor it here."));
        m_health_label->SetForegroundColour(palette().warning);
        Layout();
        return;
    }

    ++m_client_generation;
    // "host", "host:port", "[v6]:port" or a bare IPv6 literal.
    const bool        bare_v6 = std::count(m_host.begin(), m_host.end(), ':') > 1 && m_host.front() != '[';
    const std::string base    = "http://" + (bare_v6 ? "[" + m_host + "]" : m_host);
    MoonrakerLiveClient::Callbacks callbacks;
    callbacks.link = [this](MoonrakerLiveClient::Link link, const std::string &detail) {
        post([link, detail](NativePrinterPanel *self) { self->on_link(link, detail); });
    };
    callbacks.capabilities = [this](const PP::Capabilities &caps) {
        post([caps](NativePrinterPanel *self) { self->on_capabilities(caps); });
    };
    callbacks.snapshot = [this](const PP::Snapshot &snap) {
        post([snap](NativePrinterPanel *self) { self->on_snapshot(snap); });
    };
    m_client = std::make_unique<MoonrakerLiveClient>(base, std::move(callbacks));
    m_link_label->SetLabel(GUI::format_wxstr(_L("Connecting to %s…"), m_host));
    Layout();
}

void NativePrinterPanel::disconnect()
{
    ++m_client_generation; // drop callbacks still in flight
    m_client.reset();
    if (m_camera_running && m_camera_fn)
        m_camera_fn(false, {}, {});
    m_camera_running = false;
}

void NativePrinterPanel::on_link(MoonrakerLiveClient::Link link, const std::string &detail)
{
    const Palette p = palette();
    switch (link) {
    case MoonrakerLiveClient::Link::Connecting:
        m_link_label->SetLabel(GUI::format_wxstr(_L("Connecting to %s…"), m_host));
        m_link_label->SetForegroundColour(p.muted);
        break;
    case MoonrakerLiveClient::Link::Connected:
        // Plain HTTP on the local network: say LAN, not "secure".
        m_link_label->SetLabel(GUI::format_wxstr(_L("Connected · LAN · %s"), m_host));
        m_link_label->SetForegroundColour(p.success);
        break;
    case MoonrakerLiveClient::Link::Unreachable:
        m_link_label->SetLabel(GUI::format_wxstr(_L("Can't reach %s"), m_host));
        m_link_label->SetForegroundColour(p.danger);
        m_health_label->SetLabel(detail.empty() ? _L("Retrying…") : wxString::FromUTF8(detail) + " " + _L("Retrying…"));
        m_health_label->SetForegroundColour(p.muted);
        break;
    }
    Layout();
}

void NativePrinterPanel::on_capabilities(const PP::Capabilities &caps)
{
    m_caps = caps;
    rebuild_controls();
    rebuild_materials();

    const bool has_camera = !caps.webcams.empty();
    m_camera_card->Show(has_camera);
    if (m_camera_fn) {
        if (has_camera) {
            m_camera_fn(true, DeviceWorkspaceState::lan_host_from_address(m_host), caps.webcams.front());
            m_camera_running = true;
        } else if (m_camera_running) {
            m_camera_fn(false, {}, {});
            m_camera_running = false;
        }
    }
    apply_colors();
    Layout();
}

// ---------------------------------------------------------------------------
// Rendering

void NativePrinterPanel::on_snapshot(const PP::Snapshot &s)
{
    m_snapshot    = s;
    const Palette p = palette();
    const bool running = s.job_state == PP::JobState::Printing || s.job_state == PP::JobState::Paused;

    // Health lives where "printing normally" would be; no separate panel.
    if (!s.warnings.empty()) {
        wxString text;
        for (const auto &w : s.warnings)
            text += (text.empty() ? "" : "  ·  ") + wxString::FromUTF8(w);
        m_health_label->SetLabel(text);
        m_health_label->SetForegroundColour(p.danger);
    } else if (m_client) {
        const wxString state = s.job_state == PP::JobState::Printing  ? _L("Printing normally") :
                               s.job_state == PP::JobState::Paused    ? _L("Paused") :
                               s.job_state == PP::JobState::Complete  ? _L("Print complete") :
                               s.job_state == PP::JobState::Cancelled ? _L("Print cancelled") :
                                                                        _L("Ready");
        m_health_label->SetLabel(state);
        m_health_label->SetForegroundColour(s.job_state == PP::JobState::Paused ? p.warning : p.success);
    }

    // Job
    if (!s.filename.empty()) {
        m_job_name->SetLabel(wxString::FromUTF8(s.filename));
        wxString detail;
        if (s.total_layers)
            detail = wxString::Format(_L("%d layers"), *s.total_layers);
        if (!s.job_message.empty())
            detail += (detail.empty() ? "" : " · ") + wxString::FromUTF8(s.job_message);
        m_job_detail->SetLabel(detail);
    } else {
        m_job_name->SetLabel(_L("No active print"));
        m_job_detail->SetLabel(m_client ? _L("Send a job from the slicer to start printing.") : wxString());
    }
    m_job_percent->SetLabel(s.filename.empty() ? wxString() : wxString::Format("%.0f%%", s.progress * 100.));
    m_job_progress->SetValue(static_cast<int>(s.progress * 1000.));
    m_job_remaining->SetLabel(running && s.remaining ? wxString::FromUTF8(PP::format_duration(*s.remaining)) : wxString("-"));
    m_job_finishes->SetLabel(running && s.remaining ? finish_clock(*s.remaining) : wxString("-"));
    m_job_layer->SetLabel(s.current_layer && s.total_layers ? wxString::Format("%d / %d", *s.current_layer, *s.total_layers) :
                                                              wxString("-"));
    m_pause_button->SetLabel(s.job_state == PP::JobState::Paused ? _L("Resume") : _L("Pause"));
    m_pause_button->Enable(running && m_client);
    m_adjust_button->Enable(m_client != nullptr);
    m_stop_button->Enable(running && m_client);

    // Telemetry: the active toolhead's nozzle.
    const PP::Toolhead *active = nullptr;
    for (const auto &t : s.toolheads)
        if (t.active)
            active = &t;
    if (active == nullptr && !s.toolheads.empty())
        active = &s.toolheads.front();
    if (active) {
        m_nozzle_value->SetLabel(temp_text(active->heater));
        const int index = int(active - s.toolheads.data());
        m_nozzle_label->SetLabel(s.toolheads.size() > 1 ? wxString::Format(_L("Nozzle · T%d"), index + 1) : _L("Nozzle"));
    }
    m_bed_value->SetLabel(s.bed ? temp_text(*s.bed) : wxString("-"));
    m_chamber_value->SetLabel(s.chamber ? wxString::Format("%.0f°", *s.chamber) : wxString("-"));
    m_speed_value->SetLabel(wxString::Format("%d%%", s.speed_percent));
    m_flow_value->SetLabel(wxString::Format("%d%%", s.flow_percent));

    // Controls, unless the user just moved them.
    const long now = now_ms();
    for (auto &row : m_fan_rows) {
        int value = 0;
        if (row.part_fan)
            value = s.part_fan_percent.value_or(0);
        else
            for (const auto &[fan, pct] : s.fans)
                if (fan.object == row.control.object)
                    value = pct;
        if (now >= row.hold_until_ms) {
            row.slider->SetValue(value);
            row.value->SetLabel(wxString::Format("%d%%", value));
        }
    }
    for (auto &row : m_light_rows)
        for (const auto &[light, on] : s.lights)
            if (light.object == row.control.object)
                row.toggle->SetValue(on);

    // Materials
    for (size_t i = 0; i < m_material_tiles.size() && i < s.toolheads.size(); ++i) {
        const PP::Toolhead &t    = s.toolheads[i];
        MaterialTile       &tile = m_material_tiles[i];
        tile.swatch->SetBackgroundColour(t.color.empty() ? p.muted : wxColour(wxString::FromUTF8(t.color)));
        tile.detail->SetLabel(t.material.empty() ? (m_caps.toolhead_materials ? _L("Empty") : temp_text(t.heater)) :
                                                   wxString::FromUTF8(t.material));
        tile.panel->SetBackgroundColour(t.active ? p.accent.ChangeLightness(wxGetApp().dark_mode() ? 45 : 185) : p.tile);
        tile.panel->Refresh();
    }
    if (m_material_note)
        m_material_note->SetLabel(active && s.toolheads.size() > 1 ?
                                      wxString::Format(_L("Tool %d active"), int(active - s.toolheads.data()) + 1) :
                                      wxString());
    Layout();
}

void NativePrinterPanel::rebuild_controls()
{
    m_controls_grid->Clear(true);
    m_fan_rows.clear();
    m_light_rows.clear();
    auto *card = m_controls_card;

    auto add_fan = [&](const PP::Control &control, bool part_fan, const wxString &title) {
        FanRow row;
        row.control  = control;
        row.part_fan = part_fan;
        row.slider   = new wxSlider(card, wxID_ANY, 0, 0, 100);
        row.value    = label(card, "0%");
        m_controls_grid->Add(label(card, title), 0, wxALIGN_CENTER_VERTICAL);
        m_controls_grid->Add(row.slider, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        m_controls_grid->Add(row.value, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
        const size_t index = m_fan_rows.size();
        auto send = [this, index](wxCommandEvent &) {
            if (!m_client || index >= m_fan_rows.size())
                return;
            FanRow &r  = m_fan_rows[index];
            const int v = r.slider->GetValue();
            r.value->SetLabel(wxString::Format("%d%%", v));
            r.hold_until_ms = now_ms() + 3000;
            m_client->send_gcode(r.part_fan ? PP::Commands::set_part_fan(v) : PP::Commands::set_fan(r.control, v),
                                 [this](const std::string &e) { post([e](NativePrinterPanel *self) { self->report_error("fan", e); }); });
        };
        row.slider->Bind(wxEVT_SCROLL_THUMBRELEASE, [send](wxScrollEvent &e) { wxCommandEvent c; send(c); e.Skip(); });
        row.slider->Bind(wxEVT_SCROLL_CHANGED, [send](wxScrollEvent &e) { wxCommandEvent c; send(c); e.Skip(); });
        m_fan_rows.push_back(row);
    };
    if (m_caps.part_fan)
        add_fan({"fan", "fan", _u8L("Part cooling")}, true, _L("Part cooling"));
    for (const auto &fan : m_caps.fans)
        add_fan(fan, false, wxString::FromUTF8(fan.label));

    for (const auto &light : m_caps.lights) {
        LightRow row;
        row.control = light;
        row.toggle  = new wxToggleButton(card, wxID_ANY, _L("On"));
        m_controls_grid->Add(label(card, wxString::FromUTF8(light.label)), 0, wxALIGN_CENTER_VERTICAL);
        m_controls_grid->Add(row.toggle, 0);
        m_controls_grid->AddSpacer(0);
        const size_t index = m_light_rows.size();
        row.toggle->Bind(wxEVT_TOGGLEBUTTON, [this, index](wxCommandEvent &) {
            if (!m_client || index >= m_light_rows.size())
                return;
            const LightRow &r = m_light_rows[index];
            m_client->send_gcode(PP::Commands::set_light(r.control, r.toggle->GetValue()),
                                 [this](const std::string &e) { post([e](NativePrinterPanel *self) { self->report_error("light", e); }); });
        });
        m_light_rows.push_back(row);
    }
    m_controls_card->Show(!m_fan_rows.empty() || !m_light_rows.empty());
    unpin_text_sizes(m_controls_card);
}

void NativePrinterPanel::rebuild_materials()
{
    m_material_row->Clear(true);
    m_material_tiles.clear();
    // One toolhead without material info adds nothing the telemetry lacks.
    const bool show = m_caps.extruders.size() > 1 || m_caps.toolhead_materials;
    m_material_card->Show(show);
    if (!show)
        return;
    for (size_t i = 0; i < m_caps.extruders.size(); ++i) {
        MaterialTile tile;
        tile.panel  = new wxPanel(m_material_card);
        auto *col   = new wxBoxSizer(wxVERTICAL);
        tile.swatch = new wxPanel(tile.panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(18, 18)));
        tile.name   = label(tile.panel, wxString::Format("T%d", int(i + 1)), 1.0, true);
        tile.detail = label(tile.panel, "-");
        col->Add(tile.swatch, 0, wxALIGN_CENTER | wxTOP, FromDIP(8));
        col->Add(tile.name, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
        col->Add(tile.detail, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(8));
        tile.panel->SetSizer(col);
        if (m_caps.toolhead_materials) {
            // Click a toolhead to view (tag-read) or edit (manual) its filament.
            tile.panel->SetCursor(wxCursor(wxCURSOR_HAND));
            tile.panel->SetToolTip(_L("View or edit this toolhead's filament"));
            for (wxWindow *w : {static_cast<wxWindow *>(tile.panel), tile.swatch, static_cast<wxWindow *>(tile.name),
                                static_cast<wxWindow *>(tile.detail)})
                w->Bind(wxEVT_LEFT_UP, [this, i](wxMouseEvent &) { CallAfter([this, i] { show_toolhead_dialog(i); }); });
        }
        m_material_row->Add(tile.panel, 1, wxEXPAND | (i + 1 < m_caps.extruders.size() ? wxRIGHT : 0), FromDIP(6));
        m_material_tiles.push_back(tile);
    }
    unpin_text_sizes(m_material_card);
}

void NativePrinterPanel::apply_colors()
{
    const Palette p = palette();
    SetBackgroundColour(p.page);
    SetForegroundColour(p.text);
    for (wxWindow *child : GetChildren()) {
        if (auto *card = dynamic_cast<Card *>(child)) {
            card->SetBackgroundColour(p.card);
            card->set_border(p.card_border);
            for (wxWindow *w : card->GetChildren()) {
                if (dynamic_cast<wxStaticText *>(w) != nullptr) {
                    w->SetBackgroundColour(p.card);
                    w->SetForegroundColour(p.text);
                }
            }
            card->note()->SetForegroundColour(p.muted);
        } else if (dynamic_cast<wxStaticText *>(child) != nullptr) {
            child->SetBackgroundColour(p.page);
            child->SetForegroundColour(p.text);
        }
    }
    for (auto *muted : {m_job_detail, m_nozzle_label})
        if (muted)
            muted->SetForegroundColour(p.muted);
    m_link_label->SetForegroundColour(m_client ? m_link_label->GetForegroundColour() : p.muted);
    m_stop_button->SetForegroundColour(p.danger);
    for (auto &tile : m_material_tiles) {
        tile.name->SetBackgroundColour(tile.panel->GetBackgroundColour());
        tile.detail->SetBackgroundColour(tile.panel->GetBackgroundColour());
    }
    wxGetApp().UpdateDarkUIWin(this);
    unpin_text_sizes(this);
    Layout();
    Refresh();
}

void NativePrinterPanel::on_sys_color_changed()
{
    apply_colors();
    on_snapshot(m_snapshot);
}

// ---------------------------------------------------------------------------
// Actions

void NativePrinterPanel::show_adjust_dialog()
{
    if (!m_client)
        return;
    wxDialog dlg(this, wxID_ANY, _L("Adjust print"));
    auto    *sizer = new wxFlexGridSizer(3, FromDIP(10), FromDIP(10));
    sizer->AddGrowableCol(1);
    auto add = [&](const wxString &title, int value, int lo, int hi) {
        auto *slider = new wxSlider(&dlg, wxID_ANY, value, lo, hi, wxDefaultPosition, FromDIP(wxSize(260, -1)));
        auto *out    = new wxStaticText(&dlg, wxID_ANY, wxString::Format("%d%%", value));
        slider->Bind(wxEVT_SLIDER, [slider, out](wxCommandEvent &) { out->SetLabel(wxString::Format("%d%%", slider->GetValue())); });
        sizer->Add(new wxStaticText(&dlg, wxID_ANY, title), 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(slider, 1, wxEXPAND);
        sizer->Add(out, 0, wxALIGN_CENTER_VERTICAL);
        return slider;
    };
    wxSlider *speed = add(_L("Print speed"), m_snapshot.speed_percent, 10, 300);
    wxSlider *flow  = add(_L("Flow"), m_snapshot.flow_percent, 50, 150);
    auto     *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(sizer, 1, wxEXPAND | wxALL, FromDIP(14));
    outer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
    dlg.SetSizerAndFit(outer);
    wxGetApp().UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK || !m_client)
        return;
    auto on_error = [this](const std::string &e) { post([e](NativePrinterPanel *self) { self->report_error("adjust", e); }); };
    if (speed->GetValue() != m_snapshot.speed_percent)
        m_client->send_gcode(PP::Commands::set_speed(speed->GetValue()), on_error);
    if (flow->GetValue() != m_snapshot.flow_percent)
        m_client->send_gcode(PP::Commands::set_flow(flow->GetValue()), on_error);
}

void NativePrinterPanel::confirm_stop()
{
    if (!m_client)
        return;
    MessageDialog dlg(this, GUI::format_wxstr(_L("Stop printing %s? This can't be undone."), m_snapshot.filename), _L("Stop print"),
                      wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES || !m_client)
        return;
    m_client->cancel([this](const std::string &e) { post([e](NativePrinterPanel *self) { self->report_error("stop", e); }); });
}

namespace {

// View/edit one toolhead's filament. Tag-read spools are read-only unless the
// user chooses Override (FORCE=1), matching the firmware's own rules.
class ToolheadFilamentDialog : public wxDialog
{
public:
    enum class Action { None, Save, Reread, CreatePreset };

    ToolheadFilamentDialog(wxWindow *parent, size_t index, const PP::Toolhead &t, bool busy, const wxString &matching_preset)
        : wxDialog(parent, wxID_ANY, wxString::Format(_L("Toolhead %d"), int(index + 1)))
    {
        const int em    = wxGetApp().em_unit();
        auto     *top   = new wxBoxSizer(wxVERTICAL);
        wxString  state = !t.loaded   ? _L("No filament is loaded in this toolhead.") :
                          t.from_tag  ? _L("Read from the spool's RFID tag.") :
                                        _L("Set manually on the printer.");
        top->Add(new wxStaticText(this, wxID_ANY, state), 0, wxALL, em);

        auto *grid = new wxFlexGridSizer(2, em / 2, em);
        grid->AddGrowableCol(1);
        auto add_combo = [&](const wxString &label, const std::string &value, std::vector<std::string> suggestions) {
            if (!value.empty() && std::find(suggestions.begin(), suggestions.end(), value) == suggestions.end())
                suggestions.insert(suggestions.begin(), value);
            wxArrayString items;
            for (const auto &s : suggestions)
                items.Add(wxString::FromUTF8(s));
            auto *combo = new wxComboBox(this, wxID_ANY, wxString::FromUTF8(value), wxDefaultPosition, wxSize(22 * em, -1), items);
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(combo, 1, wxEXPAND);
            return combo;
        };
        m_vendor = add_combo(_L("Brand"), t.vendor, {"Generic", "Snapmaker", "Polymaker", "Bambu Lab", "eSUN", "SUNLU", "Elegoo", "Overture"});
        m_type   = add_combo(_L("Material"), t.material, U1PrintTask::known_filament_types());
        m_sub    = add_combo(_L("Variant"), t.sub_type, U1PrintTask::known_filament_sub_types());
        m_color  = new wxColourPickerCtrl(this, wxID_ANY, t.color.empty() ? *wxWHITE : wxColour(wxString::FromUTF8(t.color)));
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Color")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_color, 0);
        top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

        // Extra tag data (weight, temperatures, drying...) when the reader provided it.
        if (t.from_tag && t.tag.is_object()) {
            auto *tag  = new wxFlexGridSizer(2, em / 4, em);
            auto  fact = [&](const wxString &label, const wxString &value) {
                tag->Add(new wxStaticText(this, wxID_ANY, label));
                tag->Add(new wxStaticText(this, wxID_ANY, value));
            };
            auto num = [&](const char *key) { return t.tag.value(key, 0); };
            if (num("HOTEND_MAX_TEMP") > 0)
                fact(_L("Nozzle temperature"), wxString::Format("%d–%d °C", num("HOTEND_MIN_TEMP"), num("HOTEND_MAX_TEMP")));
            if (num("BED_TEMP") > 0)
                fact(_L("Bed temperature"), wxString::Format("%d °C", num("BED_TEMP")));
            if (num("WEIGHT") > 0)
                fact(_L("Spool weight"), wxString::Format("%d g", num("WEIGHT")));
            if (num("DRYING_TEMP") > 0)
                fact(_L("Drying"), wxString::Format(_L("%d °C for %d h"), num("DRYING_TEMP"), num("DRYING_TIME")));
            const std::string made = t.tag.value("MF_DATE", std::string());
            if (!made.empty() && made != "19700101")
                fact(_L("Manufactured"), wxString::FromUTF8(made));
            const std::string card = t.tag.value("CARD_TYPE", std::string());
            if (!card.empty())
                fact(_L("Tag"), wxString::FromUTF8(card));
            if (!tag->IsEmpty()) {
                top->Add(new wxStaticText(this, wxID_ANY, _L("From the tag")), 0, wxLEFT | wxRIGHT | wxTOP, em);
                top->Add(tag, 0, wxLEFT | wxRIGHT | wxTOP, em / 2);
            } else
                delete tag;
        }

        // Which slicer preset Sync Filament will use for this spool.
        if (t.loaded) {
            auto *preset_row = new wxBoxSizer(wxHORIZONTAL);
            preset_row->Add(new wxStaticText(this, wxID_ANY,
                                             matching_preset.IsEmpty() ? _L("No slicer preset matches this spool; syncing uses Generic.") :
                                                                         _L("Slicer preset: ") + matching_preset),
                            1, wxALIGN_CENTER_VERTICAL);
            if (matching_preset.IsEmpty()) {
                auto *create = new wxButton(this, wxID_ANY, _L("Create preset"));
                create->SetToolTip(_L("Create a user preset for this brand, based on Generic, so future syncs use it"));
                create->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
                    if (m_type->GetValue().Strip(wxString::both).IsEmpty())
                        return;
                    m_action = Action::CreatePreset;
                    EndModal(wxID_OK);
                });
                preset_row->Add(create, 0, wxLEFT, em);
            }
            top->Add(preset_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
        }

        m_note = new wxStaticText(this, wxID_ANY, busy ? _L("Changes are available when the printer isn't printing.") : wxString());
        top->Add(m_note, 0, wxALL, em);

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        if (t.from_tag) {
            auto *reread = new wxButton(this, wxID_ANY, _L("Re-read tag"));
            m_override   = new wxButton(this, wxID_ANY, _L("Override…"));
            reread->Enable(!busy);
            m_override->Enable(!busy);
            buttons->Add(reread, 0, wxRIGHT, em / 2);
            buttons->Add(m_override, 0, wxRIGHT, em / 2);
            reread->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_action = Action::Reread; EndModal(wxID_OK); });
            m_override->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
                m_force = true;
                set_editable(true);
                m_override->Disable();
                m_note->SetLabel(_L("Saving replaces the tag's data on the printer until the tag is read again."));
                Layout();
                Fit();
            });
        }
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0, wxRIGHT, em / 2);
        m_save = new wxButton(this, wxID_OK, _L("Save"));
        buttons->Add(m_save);
        m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (m_type->GetValue().Strip(wxString::both).IsEmpty()) {
                MessageDialog(this, _L("Choose a material."), GetTitle(), wxOK | wxICON_WARNING).ShowModal();
                return;
            }
            m_action = Action::Save;
            EndModal(wxID_OK);
        });
        top->Add(buttons, 0, wxEXPAND | wxALL, em);

        set_editable(t.editable && !busy);
        m_save->Show(t.loaded && !busy);
        SetSizerAndFit(top);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    Action      action() const { return m_action; }
    bool        force() const { return m_force; }
    std::string vendor() const { return into_u8(m_vendor->GetValue()); }
    std::string type() const { return into_u8(m_type->GetValue()); }
    std::string sub_type() const { return into_u8(m_sub->GetValue()); }
    std::string color() const { return into_u8(m_color->GetColour().GetAsString(wxC2S_HTML_SYNTAX)); }

private:
    void set_editable(bool on)
    {
        for (wxWindow *w : {static_cast<wxWindow *>(m_vendor), static_cast<wxWindow *>(m_type), static_cast<wxWindow *>(m_sub),
                            static_cast<wxWindow *>(m_color)})
            w->Enable(on);
        m_save->Enable(on);
    }

    wxComboBox         *m_vendor   = nullptr;
    wxComboBox         *m_type     = nullptr;
    wxComboBox         *m_sub      = nullptr;
    wxColourPickerCtrl *m_color    = nullptr;
    wxStaticText       *m_note     = nullptr;
    wxButton           *m_override = nullptr;
    wxButton           *m_save     = nullptr;
    Action              m_action   = Action::None;
    bool                m_force    = false;
};

} // namespace

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string spool_name(const std::string &vendor, const std::string &type, const std::string &sub_type)
{
    std::string name = (vendor.empty() ? std::string("Generic") : vendor) + " " + type;
    if (!sub_type.empty())
        name += " " + sub_type;
    return name;
}

std::string base_name(const std::string &preset_name)
{
    std::string base = preset_name.substr(0, preset_name.find('@'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    return base;
}

// A compatible system or user filament preset named exactly *name*.
const Preset *preset_named(const std::string &name)
{
    const std::string wanted = lower(name);
    for (const Preset &p : wxGetApp().preset_bundle->filaments)
        if (p.is_compatible && p.is_visible && (p.is_system || p.is_user()) && lower(base_name(p.name)) == wanted)
            return &p;
    return nullptr;
}

// Creates "<name> @<printer>" from the compatible "Generic <type>" system
// preset with the spool's brand, the same way Create Filament does.
bool create_spool_preset(const std::string &name, const std::string &vendor, const std::string &type, std::string &error)
{
    PresetBundle *bundle      = wxGetApp().preset_bundle;
    const Preset *base_preset = nullptr;
    const std::string generic = "generic " + lower(type);
    for (const Preset &p : bundle->filaments)
        if (p.is_system && p.is_compatible && lower(base_name(p.name)) == generic) {
            base_preset = &p;
            break;
        }
    if (base_preset == nullptr) {
        error = "No Generic " + type + " preset is available for this printer to base it on.";
        return false;
    }
    const Preset *printer = bundle->printers.get_preset_base(bundle->printers.get_edited_preset());
    const std::string printer_name = printer ? printer->name : bundle->printers.get_edited_preset().name;

    DynamicConfig overrides;
    overrides.set_key_value("filament_vendor", new ConfigOptionStrings({vendor.empty() ? std::string("Generic") : vendor}));
    overrides.set_key_value("compatible_printers", new ConfigOptionStrings({printer_name}));
    overrides.set_key_value("filament_type", new ConfigOptionStrings({type}));
    std::vector<std::string> failures;
    if (!bundle->filaments.clone_presets_for_filament(base_preset, failures, name, new_user_filament_id(name), overrides, printer_name)) {
        error = failures.empty() ? std::string("The preset could not be saved.") : "Already exists: " + failures.front();
        return false;
    }
    bundle->update_compatible(PresetSelectCompatibleType::Always);
    wxGetApp().mainframe->update_side_preset_ui();
    wxGetApp().plater()->sidebar().update_all_preset_comboboxes();
    return true;
}

} // namespace

void NativePrinterPanel::show_toolhead_dialog(size_t index)
{
    if (!m_client || index >= m_snapshot.toolheads.size())
        return;
    const bool busy = m_snapshot.job_state == PP::JobState::Printing || m_snapshot.job_state == PP::JobState::Paused;
    const PP::Toolhead &t = m_snapshot.toolheads[index];
    const Preset *match   = t.loaded ? preset_named(spool_name(t.vendor, t.material, t.sub_type)) : nullptr;
    ToolheadFilamentDialog dlg(this, index, t, busy, match ? wxString::FromUTF8(match->label(false)) : wxString());
    if (dlg.ShowModal() != wxID_OK || !m_client)
        return;
    if (dlg.action() == ToolheadFilamentDialog::Action::CreatePreset) {
        std::string error;
        const std::string name = spool_name(dlg.vendor(), dlg.type(), dlg.sub_type());
        if (create_spool_preset(name, dlg.vendor(), dlg.type(), error))
            MessageDialog(this, GUI::format_wxstr(_L("Created the preset \"%s\". Sync Filament will use it for this spool."), name),
                          _L("Create preset"), wxOK | wxICON_INFORMATION).ShowModal();
        else
            MessageDialog(this, wxString::FromUTF8(error), _L("Create preset"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    auto on_error = [this](const std::string &e) { post([e](NativePrinterPanel *self) { self->report_error("filament", e); }); };
    std::string script;
    if (dlg.action() == ToolheadFilamentDialog::Action::Reread)
        script = U1PrintTask::reread_tag_command(int(index));
    else if (dlg.action() == ToolheadFilamentDialog::Action::Save)
        script = U1PrintTask::set_filament_command(int(index), dlg.vendor(), dlg.type(), dlg.sub_type(), dlg.color(), dlg.force());
    if (!script.empty())
        m_client->send_gcode(script, on_error);
}

void NativePrinterPanel::report_error(const std::string &what, const std::string &error)
{
    BOOST_LOG_TRIVIAL(error) << "Printer panel: " << what << " failed: " << error;
    m_health_label->SetLabel(GUI::format_wxstr(_L("The printer rejected the last command: %s"), error));
    m_health_label->SetForegroundColour(palette().danger);
    Layout();
}

} // namespace Slic3r::GUI
