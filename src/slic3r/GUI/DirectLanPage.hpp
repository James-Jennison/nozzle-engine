#ifndef slic3r_GUI_DirectLanPage_hpp_
#define slic3r_GUI_DirectLanPage_hpp_

// Header-only embedded web view that shows a printer's camera stream straight from
// its LAN address (WebRTC page with automatic MJPEG fallback). Extracted from the
// Nozzle Advanced Workspace DeviceWorkspace; used by NativeDevicePage.

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/DeviceWorkspaceState.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include <wx/button.h>
#if defined(__linux__)
#include <webkit2/webkit2.h>
#include <wx/nativewin.h>
#endif
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r::GUI {

namespace direct_lan_detail {

constexpr int ID_DEVICE_CONTROL = wxID_HIGHEST + 1800;
constexpr int ID_DEVICE_CAMERA  = wxID_HIGHEST + 1801;
constexpr int ID_DEVICE_THEME   = wxID_HIGHEST + 1802;
constexpr int ID_DEVICE_PRINTER = wxID_HIGHEST + 1803;

wxString html_escape(const wxString& value)
{
    wxString escaped = value;
    escaped.Replace("&", "&amp;");
    escaped.Replace("\"", "&quot;");
    escaped.Replace("'", "&#39;");
    escaped.Replace("<", "&lt;");
    escaped.Replace(">", "&gt;");
    return escaped;
}

wxPanel* make_status_panel(wxWindow* parent, wxStaticText*& status)
{
    auto* panel = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    status = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    status->Wrap(panel->FromDIP(620));
    sizer->Add(status, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, panel->FromDIP(24));
    sizer->AddStretchSpacer();
    panel->SetSizer(sizer);
    return panel;
}

} // namespace direct_lan_detail
using direct_lan_detail::html_escape;
using direct_lan_detail::make_status_panel;

class DirectLanPage final : public wxPanel
{
public:
    explicit DirectLanPage(wxWindow* parent) : wxPanel(parent)
    {
        auto* outer    = new wxBoxSizer(wxVERTICAL);
        m_book         = new wxSimplebook(this);
        m_status_panel = make_status_panel(m_book, m_status);
        m_book->AddPage(m_status_panel, wxEmptyString, true);
        outer->Add(m_book, 1, wxEXPAND);
        SetSizer(outer);
        show_unavailable();
    }

    ~DirectLanPage() override { teardown(); }

    void activate(const DeviceWorkspaceEndpoint& endpoint, std::size_t generation)
    {
        const std::string origin = DeviceWorkspaceState::http_origin(endpoint.host);
        if (!endpoint.connected || origin.empty()) {
            teardown();
            show_unavailable();
            return;
        }

        if (m_browser != nullptr && m_generation == generation && m_origin == origin) {
            m_book->SetSelection(1);
            return;
        }

        teardown();
        m_generation    = generation;
        m_origin        = origin;
        m_showing_error = false;
        m_browser       = create_ephemeral_browser();
        if (m_browser == nullptr) {
            show_error();
            return;
        }
        m_book->AddPage(m_browser, wxEmptyString, true);

        load_camera();
    }

    void deactivate()
    {
        teardown();
        show_suspended();
    }

    // Printers without WebRTC (e.g. mjpeg-streamer): show this stream path
    // directly instead of the U1's WebRTC page. Empty restores the U1 page.
    void set_mjpeg_only(std::string path) { m_mjpeg_only = std::move(path); }
    // Embedded in a card: hide the page's own mode/fallback bar so the video
    // fills the view. Automatic MJPEG fallback and errors still work.
    void set_embedded(bool embedded) { m_embedded = embedded; }

    void reload(const DeviceWorkspaceEndpoint& endpoint, std::size_t generation)
    {
        teardown();
        activate(endpoint, generation);
    }

private:
    void load_camera()
    {
        if (!m_mjpeg_only.empty()) {
            load_mjpeg_only();
            return;
        }
        const wxString origin = html_escape(wxString::FromUTF8(m_origin.c_str()));
        const wxString html   = wxString::Format(R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; frame-src %s; img-src %s; style-src 'unsafe-inline'; script-src 'unsafe-inline'; form-action 'none'; base-uri 'none'">
<meta name="referrer" content="no-referrer">
<style>html,body{height:100%%;margin:0;background:#17191a;color:#f5f5f5;font:14px sans-serif}main{height:100%%;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px}iframe,img{width:100%%;height:calc(100%% - 52px);border:0;object-fit:contain;background:#090a0a}.actions{display:flex;align-items:center;gap:12px}button{padding:8px 14px}#error{color:#ffb4ab}%s</style>
</head><body><main><div class="actions"><span id="mode">WebRTC camera</span><button id="fallback" type="button">Use MJPEG fallback</button><span id="error" role="status"></span></div><iframe id="rtc" credentialless sandbox="allow-scripts allow-same-origin" referrerpolicy="no-referrer" allow="autoplay" src="about:blank"></iframe><img id="mjpeg" alt="LAN camera" hidden></main>
<script>(()=>{'use strict';const rtc=document.getElementById('rtc'),img=document.getElementById('mjpeg'),mode=document.getElementById('mode'),error=document.getElementById('error');let fallback=false;function useMjpeg(){if(fallback)return;fallback=true;rtc.src='about:blank';rtc.remove();mode.textContent='MJPEG compatibility stream';img.hidden=false;img.src='%s/webcam/stream.mjpg';}document.getElementById('fallback').addEventListener('click',useMjpeg);img.addEventListener('error',()=>{img.removeAttribute('src');error.textContent='LAN camera is unavailable. Snapmaker Control remains connected.';});if(typeof RTCPeerConnection==='undefined')useMjpeg();else{rtc.addEventListener('error',useMjpeg);rtc.src='%s/webcam/webrtc?timeout_s=300';}addEventListener('pagehide',()=>{rtc.src='about:blank';img.removeAttribute('src');},{once:true});})();</script></body></html>)HTML",
                                                 origin, origin,
                                                 m_embedded ? wxString(".actions{position:absolute;top:8px;left:8px}#mode,#fallback{display:none}iframe,img{height:100%}") : wxString(),
                                                 origin, origin);
        // The browser's per-view ephemeral data manager is the isolation
        // boundary. Keep the camera wrapper on an opaque local document origin
        // and make the WebRTC frame credentialless as defense in depth. The
        // MJPEG image deliberately uses the browser's default no-credentials
        // cross-origin request mode because U1 streams do not advertise CORS.
        load_html(html);
    }

    // Moonraker's /webcam/ path often redirects to mjpeg-streamer on another
    // port of the same printer (the Elegoo CC uses :8080), so the image policy
    // allows any port on this printer's host and nothing else.
    void load_mjpeg_only()
    {
        const wxString origin = html_escape(wxString::FromUTF8(m_origin.c_str()));
        const wxString path   = html_escape(wxString::FromUTF8(m_mjpeg_only.c_str()));
        const wxString html   = wxString::Format(R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src %s %s:*; style-src 'unsafe-inline'; script-src 'unsafe-inline'; form-action 'none'; base-uri 'none'">
<meta name="referrer" content="no-referrer">
<style>html,body{height:100%%;margin:0;background:#090a0a;color:#f5f5f5;font:14px sans-serif}img{width:100%%;height:100%%;object-fit:contain}#error{position:absolute;top:45%%;width:100%%;text-align:center;color:#ffb4ab}</style>
</head><body><img id="mjpeg" alt="Printer camera" src="%s%s"><div id="error" role="status"></div>
<script>(()=>{'use strict';const img=document.getElementById('mjpeg');img.addEventListener('error',()=>{img.removeAttribute('src');document.getElementById('error').textContent='The printer camera is unavailable.';});addEventListener('pagehide',()=>img.removeAttribute('src'),{once:true});})();</script></body></html>)HTML",
                                                 origin, origin, origin, path);
        load_html(html);
    }

    wxWindow* create_ephemeral_browser()
    {
#if defined(__linux__)
        m_native_browser = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "is-ephemeral", TRUE, nullptr));
        if (m_native_browser == nullptr || !webkit_web_view_is_ephemeral(m_native_browser)) {
            if (m_native_browser != nullptr)
                g_object_unref(m_native_browser);
            m_native_browser = nullptr;
            return nullptr;
        }

        WebKitSettings* settings = webkit_web_view_get_settings(m_native_browser);
        webkit_settings_set_enable_developer_extras(settings, FALSE);
        webkit_settings_set_enable_webrtc(settings, TRUE);
        g_signal_connect(m_native_browser, "decide-policy", G_CALLBACK(on_native_policy), this);
        g_signal_connect(m_native_browser, "load-failed", G_CALLBACK(on_native_load_failed), this);
        g_signal_connect(m_native_browser, "context-menu", G_CALLBACK(on_native_context_menu), this);

        auto* wrapper = new wxNativeWindow(m_book, wxID_ANY, GTK_WIDGET(m_native_browser));
        if (wrapper->GetHandle() == nullptr) {
            wrapper->Destroy();
            g_object_unref(m_native_browser);
            m_native_browser = nullptr;
            return nullptr;
        }
        // Transfer the creation reference to wxNativeWindow so its destruction
        // releases the native WebKit view exactly once.
        wrapper->Disown();
        return wrapper;
#else
        // wxWidgets 3.1 does not expose an ephemeral data-store constructor on
        // the WebView2 or WKWebView backends. Fail closed instead of silently
        // sharing the application's persistent browser profile.
        return nullptr;
#endif
    }

    void load_html(const wxString& html)
    {
#if defined(__linux__)
        const wxScopedCharBuffer utf8 = html.ToUTF8();
        webkit_web_view_load_html(m_native_browser, utf8.data(), "about:blank");
#else
        (void) html;
#endif
    }

    void teardown()
    {
        if (m_browser == nullptr)
            return;
#if defined(__linux__)
        g_signal_handlers_disconnect_by_data(m_native_browser, this);
        webkit_web_view_stop_loading(m_native_browser);
        webkit_web_view_load_uri(m_native_browser, "about:blank");
#endif
        const int page = m_book->FindPage(m_browser);
        if (page != wxNOT_FOUND)
            m_book->RemovePage(static_cast<size_t>(page));
        m_browser->Destroy();
        m_browser = nullptr;
#if defined(__linux__)
        m_native_browser = nullptr;
#endif
        m_origin.clear();
        m_generation = 0;
    }

    bool allows(const wxString& candidate) const
    {
        if (candidate == "about:blank")
            return true;
        const wxString origin = wxString::FromUTF8(m_origin.c_str());
        if (!candidate.StartsWith(origin))
            return false;
        if (candidate.length() == origin.length())
            return true;
        const wxChar boundary = candidate[origin.length()];
        return boundary == '/' || boundary == '?' || boundary == '#';
    }

#if defined(__linux__)
    static gboolean on_native_policy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer user_data)
    {
        auto* self = static_cast<DirectLanPage*>(user_data);
        if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
            return FALSE;

        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        WebKitURIRequest* request = webkit_navigation_action_get_request(action);
        const gchar*      uri     = webkit_uri_request_get_uri(request);
        if (uri == nullptr || !self->allows(wxString::FromUTF8(uri))) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
        return FALSE;
    }

    static gboolean on_native_load_failed(WebKitWebView* failed_view, WebKitLoadEvent, const gchar* failing_uri, GError*, gpointer user_data)
    {
        auto* self = static_cast<DirectLanPage*>(user_data);
        if (self->m_showing_error || failing_uri == nullptr || std::string(failing_uri) == "about:blank")
            return FALSE;

        self->show_error();
        wxWindow* const failed_wrapper = self->m_browser;
        self->CallAfter([self, failed_wrapper, failed_view] {
            if (self->m_browser == failed_wrapper && self->m_native_browser == failed_view)
                self->teardown();
        });
        return TRUE;
    }

    static gboolean on_native_context_menu(WebKitWebView*, WebKitContextMenu*, GdkEvent*, WebKitHitTestResult*, gpointer) { return TRUE; }
#endif

    void show_unavailable()
    {
        m_status->SetLabel(_L("LAN tools are unavailable until the selected Snapmaker U1 has an active LAN endpoint."));
        m_book->SetSelection(0);
    }

    void show_suspended()
    {
        m_status->SetLabel(_L("This LAN page is suspended while hidden."));
        m_book->SetSelection(0);
    }

    void show_error()
    {
        m_showing_error = true;
        m_status->SetLabel(_L("The selected printer's LAN page is unavailable. Snapmaker Control remains connected."));
        m_book->SetSelection(0);
    }

    wxSimplebook* m_book{nullptr};
    wxPanel*      m_status_panel{nullptr};
    wxStaticText* m_status{nullptr};
    wxWindow*     m_browser{nullptr};
#if defined(__linux__)
    WebKitWebView* m_native_browser{nullptr};
#endif
    std::string m_origin;
    std::string m_mjpeg_only;
    bool        m_embedded{false};
    std::size_t m_generation{0};
    bool        m_showing_error{false};
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_DirectLanPage_hpp_
