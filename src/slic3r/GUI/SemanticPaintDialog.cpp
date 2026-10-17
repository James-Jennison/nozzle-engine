#include "SemanticPaintDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SemanticPaint.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/Utils/SemanticPaintClient.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace SP = Slic3r::SemanticPaint;

namespace {

std::vector<SP::FilamentInfo> current_filaments()
{
    std::vector<SP::FilamentInfo> out;
    const DynamicPrintConfig &cfg    = wxGetApp().preset_bundle->project_config;
    const auto *colours = cfg.option<ConfigOptionStrings>("filament_colour");
    const auto *types   = cfg.option<ConfigOptionStrings>("filament_type");
    const size_t n = colours ? colours->values.size() : 0;
    for (size_t i = 0; i < n; ++i) {
        SP::FilamentInfo f;
        f.index     = int(i) + 1;
        f.color_hex = colours->values[i];
        f.type      = types && i < types->values.size() ? types->values[i] : std::string();
        out.push_back(f);
    }
    return out;
}

wxImage to_wx(const SP::Image &img, int display)
{
    wxImage w(img.width, img.height);
    std::copy(img.rgb.begin(), img.rgb.end(), w.GetData());
    if (display != img.width)
        w.Rescale(display, display, wxIMAGE_QUALITY_HIGH);
    return w;
}

} // namespace

class SemanticPaintDialog : public DPIDialog
{
public:
    SemanticPaintDialog(wxWindow *parent, const std::string &object_name, SP::Segmentation seg, std::vector<SP::Image> images,
                        std::vector<std::string> view_names, std::vector<SP::FilamentInfo> filaments)
        : DPIDialog(parent, wxID_ANY, _L("AI auto-paint"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_object_name(object_name)
        , m_seg(std::move(seg))
        , m_view_names(std::move(view_names))
        , m_filaments(std::move(filaments))
        , m_alive(std::make_shared<std::atomic<bool>>(true))
    {
        for (const SP::Image &img : images)
            m_png.push_back(SP::encode_png(img));

        const int em = wxGetApp().em_unit();
        auto *root   = new wxBoxSizer(wxVERTICAL);

        root->Add(new wxStaticText(this, wxID_ANY,
                                   _L("These are the exact images that will be sent, together with a text description of each segment "
                                      "and your filament list. Nothing is sent until you press the button below.")),
                  0, wxALL, em);

        auto *images_sizer = new wxGridSizer(3, 4, 4);
        for (size_t i = 0; i < images.size(); ++i) {
            auto *box = new wxBoxSizer(wxVERTICAL);
            box->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(to_wx(images[i], 8 * em * 2 / 2 + 8 * em))), 0, wxALIGN_CENTER);
            box->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8(m_view_names[i].c_str())), 0, wxALIGN_CENTER);
            images_sizer->Add(box, 0, wxALIGN_CENTER);
        }
        root->Add(images_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, em);

        // Provider settings.
        auto *form = new wxFlexGridSizer(2, 4, 8);
        form->AddGrowableCol(1, 1);
        auto row = [&](const wxString &label, wxWindow *w) {
            form->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
            form->Add(w, 1, wxEXPAND);
        };
        m_provider = new wxChoice(this, wxID_ANY);
        m_provider->Append(_L("Command (default: Claude Code, uses your Claude subscription)"));
        m_provider->Append(_L("OpenAI-compatible API (OpenAI, Ollama, LM Studio...)"));
        m_command  = new wxTextCtrl(this, wxID_ANY);
        m_endpoint = new wxTextCtrl(this, wxID_ANY);
        m_key      = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_remember = new wxCheckBox(this, wxID_ANY, _L("Remember the API key in the system keychain"));
        m_model    = new wxTextCtrl(this, wxID_ANY);
        m_hint     = new wxTextCtrl(this, wxID_ANY);
        m_hint->SetHint(_L("Optional, e.g. \"make it a festive Christmas tree\""));
        row(_L("Provider"), m_provider);
        row(_L("Command"), m_command);
        row(_L("Server address"), m_endpoint);
        row(_L("API key"), m_key);
        form->Add(new wxStaticText(this, wxID_ANY, ""));
        form->Add(m_remember);
        row(_L("Model (blank = default)"), m_model);
        row(_L("Hint"), m_hint);
        root->Add(form, 0, wxALL | wxEXPAND, em);

        m_status = new wxStaticText(this, wxID_ANY, "");
        root->Add(m_status, 0, wxLEFT | wxRIGHT | wxEXPAND, em);

        m_send = new wxButton(this, wxID_ANY, "");
        root->Add(m_send, 0, wxALL, em);

        // Result: per-segment filament choice.
        m_result_win = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 14 * em));
        m_result_win->SetScrollRate(0, em);
        m_result_sizer = new wxFlexGridSizer(3, 6, 12);
        m_result_win->SetSizer(m_result_sizer);
        root->Add(m_result_win, 1, wxLEFT | wxRIGHT | wxEXPAND, em);
        m_result_win->Hide();

        m_notes = new wxStaticText(this, wxID_ANY, "");
        root->Add(m_notes, 0, wxALL | wxEXPAND, em);

        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_apply = new wxButton(this, wxID_OK, _L("Apply as paint"));
        m_apply->Disable();
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxRIGHT, em);
        buttons->Add(m_apply, 0);
        root->Add(buttons, 0, wxALL | wxEXPAND, em);

        SetSizerAndFit(root);
        load_settings();
        update_provider_fields();
        m_provider->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { update_provider_fields(); });

        m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_send(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) {
            m_cancel = true;
            e.Skip();
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            if (e.GetId() == wxID_CANCEL)
                m_cancel = true;
            e.Skip();
        }, wxID_CANCEL);
    }

    ~SemanticPaintDialog() override
    {
        m_alive->store(false);
        m_cancel = true;
        if (m_worker.joinable())
            m_worker.join();
    }

    // Segment -> filament slot (0 = leave unpainted) chosen by the user.
    std::vector<int> assignments() const
    {
        std::vector<int> out(m_seg.segments.size(), 0);
        for (size_t i = 0; i < m_choices.size() && i < out.size(); ++i)
            out[i] = m_choices[i]->GetSelection();
        return out;
    }

protected:
    void on_dpi_changed(const wxRect &) override { Fit(); }

private:
    bool is_openai() const { return m_provider->GetSelection() == 1; }

    void load_settings()
    {
        AppConfig *cfg = wxGetApp().app_config;
        m_provider->SetSelection(cfg->get("semantic_paint", "provider") == "openai" ? 1 : 0);
        const std::string command = cfg->get("semantic_paint", "command");
        m_command->SetValue(wxString::FromUTF8((command.empty() ? std::string(k_semantic_paint_default_command) : command).c_str()));
        const std::string endpoint = cfg->get("semantic_paint", "endpoint");
        m_endpoint->SetValue(wxString::FromUTF8((endpoint.empty() ? SemanticPaintSettings().endpoint : endpoint).c_str()));
        m_model->SetValue(wxString::FromUTF8(cfg->get("semantic_paint", "model").c_str()));
        const std::string key = semantic_paint_load_api_key();
        m_key->SetValue(wxString::FromUTF8(key.c_str()));
        m_remember->SetValue(!key.empty());
    }

    void save_settings(const SemanticPaintSettings &s)
    {
        AppConfig *cfg = wxGetApp().app_config;
        cfg->set("semantic_paint", "provider", s.provider);
        cfg->set("semantic_paint", "command", s.command);
        cfg->set("semantic_paint", "endpoint", s.endpoint);
        cfg->set("semantic_paint", "model", s.model);
        if (s.provider == "openai")
            semantic_paint_save_api_key(m_remember->GetValue() ? s.api_key : std::string());
    }

    void update_provider_fields()
    {
        const bool openai = is_openai();
        m_command->Enable(!openai);
        m_endpoint->Enable(openai);
        m_key->Enable(openai);
        m_remember->Enable(openai);
        const wxString dest = openai ? _L("the server") : _L("the command");
        m_send->SetLabel(wxString::Format(_L("Send these %d images to %s"), int(m_png.size()), dest));
        Layout();
    }

    SemanticPaintSettings gather() const
    {
        SemanticPaintSettings s;
        s.provider = is_openai() ? "openai" : "command";
        s.command  = std::string(m_command->GetValue().utf8_str());
        s.endpoint = std::string(m_endpoint->GetValue().utf8_str());
        s.api_key  = std::string(m_key->GetValue().utf8_str());
        s.model    = std::string(m_model->GetValue().utf8_str());
        return s;
    }

    void on_send()
    {
        if (m_busy)
            return;
        const SemanticPaintSettings s = gather();
        save_settings(s);
        const std::string prompt = SP::build_prompt(m_seg, m_filaments, m_object_name, m_view_names, std::string(m_hint->GetValue().utf8_str()));

        m_busy   = true;
        m_cancel = false;
        m_send->Disable();
        m_apply->Disable();
        m_status->SetLabel(_L("Waiting for the answer... this can take up to a minute."));
        if (m_worker.joinable())
            m_worker.join();
        auto alive = m_alive;
        m_worker   = std::thread([this, alive, s, prompt] {
            SemanticPaintReply r = semantic_paint_query(s, prompt, m_png, &m_cancel);
            wxTheApp->CallAfter([this, alive, r = std::move(r)] {
                if (alive->load())
                    on_reply(r);
            });
        });
    }

    void on_reply(const SemanticPaintReply &r)
    {
        m_busy = false;
        m_send->Enable();
        if (!r.ok) {
            m_status->SetLabel(_L("Failed: ") + wxString::FromUTF8(r.error.c_str()));
            Layout();
            return;
        }
        const auto parsed = SP::parse_assignments(r.text, m_seg.segments.size(), m_filaments.size());
        if (!parsed) {
            m_status->SetLabel(_L("The answer could not be understood. Try again or pick another model."));
            Layout();
            return;
        }
        m_status->SetLabel(_L("Review the suggestion, change anything you like, then apply it."));
        populate(*parsed);
        m_apply->Enable();
        Layout();
        Fit();
    }

    void populate(const std::vector<int> &assignment)
    {
        m_result_sizer->Clear(true);
        m_choices.clear();
        for (const SP::Segment &s : m_seg.segments) {
            m_result_sizer->Add(new wxStaticText(m_result_win, wxID_ANY, wxString::Format("#%d", s.id)), 0, wxALIGN_CENTER_VERTICAL);
            m_result_sizer->Add(new wxStaticText(m_result_win, wxID_ANY, wxString::Format(_L("%.1f%% of the surface"), s.area_fraction * 100.)), 0,
                                wxALIGN_CENTER_VERTICAL);
            auto *choice = new wxChoice(m_result_win, wxID_ANY);
            choice->Append(_L("Unpainted"));
            for (const SP::FilamentInfo &f : m_filaments)
                choice->Append(wxString::Format("%d  %s  %s", f.index, wxString::FromUTF8(f.color_hex.c_str()),
                                                wxString::FromUTF8(SP::color_name(f.color_hex).c_str())));
            choice->SetSelection(s.id < int(assignment.size()) ? assignment[s.id] : 0);
            m_choices.push_back(choice);
            m_result_sizer->Add(choice, 0, wxEXPAND);
        }
        m_result_win->Show();
        m_result_win->FitInside();
    }

    std::string                       m_object_name;
    SP::Segmentation                  m_seg;
    std::vector<std::string>          m_png;
    std::vector<std::string>          m_view_names;
    std::vector<SP::FilamentInfo>     m_filaments;
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::atomic<bool>                 m_cancel{false};
    bool                              m_busy{false};
    std::thread                       m_worker;

    wxChoice     *m_provider{nullptr};
    wxTextCtrl   *m_model{nullptr}, *m_command{nullptr}, *m_endpoint{nullptr}, *m_key{nullptr}, *m_hint{nullptr};
    wxCheckBox   *m_remember{nullptr};
    wxStaticText *m_status{nullptr}, *m_notes{nullptr};
    wxButton     *m_send{nullptr}, *m_apply{nullptr};
    wxScrolledWindow *m_result_win{nullptr};
    wxFlexGridSizer  *m_result_sizer{nullptr};
    std::vector<wxChoice *> m_choices;
};

bool can_run_semantic_paint()
{
    return wxGetApp().obj_list()->get_selected_obj_idx() >= 0;
}

void run_semantic_paint()
{
    Plater *plater = wxGetApp().plater();
    const int obj_idx = wxGetApp().obj_list()->get_selected_obj_idx();
    Model &model = plater->model();
    if (obj_idx < 0 || obj_idx >= int(model.objects.size())) {
        show_info(plater, _L("Select an object first."), _L("AI auto-paint"));
        return;
    }
    ModelObject *object = model.objects[obj_idx];
    std::vector<ModelVolume *> parts;
    for (ModelVolume *v : object->volumes)
        if (v->is_model_part())
            parts.push_back(v);
    if (parts.empty())
        return;

    const std::vector<SP::FilamentInfo> filaments = current_filaments();
    if (filaments.size() < 2) {
        show_info(plater, _L("Add at least two filaments first; there is nothing to choose between with one."), _L("AI auto-paint"));
        return;
    }
    const bool has_paint = std::any_of(parts.begin(), parts.end(), [](const ModelVolume *v) { return !v->mmu_segmentation_facets.empty(); });
    if (has_paint &&
        MessageDialog(plater, _L("This object already has colour painting. AI auto-paint will replace it. Continue?"), _L("AI auto-paint"),
                      wxYES_NO | wxICON_QUESTION).ShowModal() != wxID_YES)
        return;

    // All parts go into one mesh (volume transforms applied, instance ignored). Parts share no vertices, so a segment never
    // spans two parts, and each part owns a contiguous range of faces that is used to split the answer back up.
    indexed_triangle_set       combined;
    std::vector<size_t>        first_face;
    for (ModelVolume *v : parts) {
        TriangleMesh mesh = v->mesh();
        mesh.transform(v->get_matrix());
        const int offset = int(combined.vertices.size());
        first_face.push_back(combined.indices.size());
        combined.vertices.insert(combined.vertices.end(), mesh.its.vertices.begin(), mesh.its.vertices.end());
        for (const stl_triangle_vertex_indices &t : mesh.its.indices)
            combined.indices.emplace_back(t(0) + offset, t(1) + offset, t(2) + offset);
    }
    first_face.push_back(combined.indices.size());

    const SP::Segmentation seg = SP::segment_mesh(combined);
    std::vector<SP::Image>   images;
    std::vector<std::string> names;
    for (const SP::View &v : SP::default_views()) {
        images.push_back(SP::render_view(combined, seg, v));
        names.push_back(v.name);
    }

    SemanticPaintDialog dlg(plater, object->name, seg, std::move(images), std::move(names), filaments);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const std::vector<int> faces = SP::face_filaments(seg, dlg.assignments());
    {
        Plater::TakeSnapshot snapshot(plater, _u8L("AI auto-paint"));
        for (size_t p = 0; p < parts.size(); ++p) {
            TriangleSelector selector(parts[p]->mesh());
            for (size_t f = first_face[p]; f < first_face[p + 1]; ++f)
                if (faces[f] > 0 && faces[f] <= int(EnforcerBlockerType::ExtruderMax))
                    selector.set_facet(int(f - first_face[p]), EnforcerBlockerType(faces[f]));
            parts[p]->mmu_segmentation_facets.set(selector);
        }
    }
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    plater->changed_object(obj_idx);
}

} // namespace GUI
} // namespace Slic3r
