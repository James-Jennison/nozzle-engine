#include "U1SendDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcmemory.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <set>

namespace Slic3r {
namespace GUI {

namespace {

wxBitmap swatch(const std::string &color, int size)
{
    wxBitmap   bmp(size, size);
    wxMemoryDC dc(bmp);
    wxColour   fill(color.empty() ? wxString("#808080") : wxString(color));
    dc.SetBackground(wxBrush(fill));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(128, 128, 128)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, size, size);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxString describe(const U1PrintTask::Filament &f)
{
    if (!f.present)
        return _L("empty");
    wxString text = f.type.empty() ? _L("unknown") : wxString::FromUTF8(f.type);
    if (!f.color.empty())
        text += " " + wxString::FromUTF8(f.color);
    return text;
}

} // namespace

U1SendDialog::U1SendDialog(wxWindow                                 *parent,
                           const boost::filesystem::path            &default_name,
                           const std::vector<U1PrintTask::Filament> &plate_filaments,
                           const U1PrintTask::PrinterState          &printer)
    : DPIDialog(parent, wxID_ANY, _L("Send to Snapmaker U1"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_plate_filaments(plate_filaments)
    , m_toolheads(printer.toolheads)
{
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);

    top->Add(new wxStaticText(this, wxID_ANY, _L("File name on the printer:")), 0, wxLEFT | wxRIGHT | wxTOP, em);
    m_filename = new wxTextCtrl(this, wxID_ANY, from_path(default_name.filename()));
    top->Add(m_filename, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);

    // Filament -> toolhead mapping
    top->Add(new wxStaticText(this, wxID_ANY, _L("Toolhead for each filament:")), 0, wxLEFT | wxRIGHT | wxTOP, em);
    auto *grid = new wxFlexGridSizer(3, em / 2, em);
    grid->AddGrowableCol(2);

    wxArrayString toolhead_labels;
    for (size_t t = 0; t < printer.toolheads.size(); ++t)
        toolhead_labels.Add(wxString::Format(_L("Toolhead %d"), int(t + 1)) + " (" + describe(printer.toolheads[t]) + ")");

    const std::vector<int> suggested = U1PrintTask::suggest_mapping(plate_filaments, printer.toolheads);
    for (size_t i = 0; i < plate_filaments.size(); ++i) {
        const U1PrintTask::Filament &f = plate_filaments[i];
        if (!f.present)
            continue;
        grid->Add(new wxStaticBitmap(this, wxID_ANY, swatch(f.color, em * 2)), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(this, wxID_ANY, wxString::Format(_L("Filament %d"), int(i + 1)) + ": " + describe(f)), 0,
                  wxALIGN_CENTER_VERTICAL);
        auto *choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, toolhead_labels);
        choice->SetSelection(suggested[i] >= 0 ? suggested[i] : 0);
        grid->Add(choice, 1, wxEXPAND);
        m_rows.push_back({i, choice});
    }
    top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // Print options, defaulting to what the printer last used.
    top->Add(new wxStaticText(this, wxID_ANY, _L("Before printing:")), 0, wxLEFT | wxRIGHT | wxTOP, em);
    auto add_check = [&](const wxString &label, bool value) {
        auto *check = new wxCheckBox(this, wxID_ANY, label);
        check->SetValue(value);
        top->Add(check, 0, wxLEFT | wxRIGHT | wxTOP, em / 2);
        return check;
    };
    m_bed_level        = add_check(_L("Bed leveling"), printer.bed_level);
    m_flow_calibrate   = add_check(_L("Flow calibration (toolheads used by this print)"), printer.flow_calibrate);
    m_shaper_calibrate = add_check(_L("Input shaper calibration"), printer.shaper_calibrate);
    m_time_lapse       = add_check(_L("Timelapse"), printer.time_lapse_camera);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto *upload  = new wxButton(this, wxID_ANY, _L("Upload only"));
    auto *print   = new wxButton(this, wxID_OK, _L("Print"));
    buttons->AddStretchSpacer();
    buttons->Add(cancel, 0, wxRIGHT, em / 2);
    buttons->Add(upload, 0, wxRIGHT, em / 2);
    buttons->Add(print);
    top->Add(buttons, 0, wxEXPAND | wxALL, em);

    upload->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_confirm(false); });
    print->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_confirm(true); });
    print->SetDefault();

    SetSizerAndFit(top);
    SetMinSize(wxSize(60 * em, -1));
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

boost::filesystem::path U1SendDialog::filename() const
{
    return boost::filesystem::path(into_u8(m_filename->GetValue()));
}

U1PrintTask::Options U1SendDialog::options() const
{
    U1PrintTask::Options options;
    options.filament_to_toolhead.assign(m_plate_filaments.size(), U1PrintTask::UNMAPPED);
    std::set<int> used_toolheads;
    for (const Row &row : m_rows) {
        const int toolhead                         = row.toolhead->GetSelection();
        options.filament_to_toolhead[row.filament] = toolhead;
        used_toolheads.insert(toolhead);
    }
    options.bed_level         = m_bed_level->GetValue();
    options.flow_calibrate    = m_flow_calibrate->GetValue();
    options.shaper_calibrate  = m_shaper_calibrate->GetValue();
    options.time_lapse_camera = m_time_lapse->GetValue();
    if (*options.flow_calibrate)
        options.flow_calibrate_toolheads.assign(used_toolheads.begin(), used_toolheads.end());
    return options;
}

void U1SendDialog::on_confirm(bool start_print)
{
    static const wxString forbidden = "#*;\\/:\"<>|";
    const wxString name = m_filename->GetValue().Strip(wxString::both);
    if (name.IsEmpty() || name.find_first_of(forbidden) != wxString::npos) {
        MessageDialog(this, _L("Please enter a file name without the characters #, *, ;, \\, /, :, \", <, >, or |."),
                      _L("Send to Snapmaker U1"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    if (!name.Lower().EndsWith(".gcode"))
        m_filename->SetValue(name + ".gcode");

    std::string error;
    if (!U1PrintTask::validate(options(), error)) {
        MessageDialog(this, wxString::FromUTF8(error), _L("Send to Snapmaker U1"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    // Mapping onto a different material is allowed (it can be deliberate),
    // but the G-code keeps the sliced filament's temperatures and speeds.
    wxString mismatches;
    for (const Row &row : m_rows) {
        const auto  &want     = m_plate_filaments[row.filament];
        const int    toolhead = row.toolhead->GetSelection();
        if (toolhead < 0 || size_t(toolhead) >= m_toolheads.size())
            continue;
        const auto &have = m_toolheads[toolhead];
        if (have.present && !want.type.empty() && !have.type.empty() && wxString::FromUTF8(want.type).Upper() != wxString::FromUTF8(have.type))
            mismatches += "\n" + wxString::Format(_L("Filament %d is %s, but toolhead %d has %s loaded."), int(row.filament + 1),
                                                   wxString::FromUTF8(want.type), toolhead + 1, wxString::FromUTF8(have.type));
    }
    if (!mismatches.IsEmpty() &&
        MessageDialog(this, _L("The filament types do not match:") + "\n" + mismatches + "\n\n" +
                                _L("The print will use the temperatures and speeds sliced for the original filament. Continue anyway?"),
                      _L("Send to Snapmaker U1"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING)
                .ShowModal() != wxID_YES)
        return;

    m_start_print = start_print;
    EndModal(wxID_OK);
}

void U1SendDialog::on_dpi_changed(const wxRect & /*suggested_rect*/)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
