#ifndef slic3r_U1LanPrintHost_hpp_
#define slic3r_U1LanPrintHost_hpp_

#include "PrintHost.hpp"
#include "libslic3r/U1PrintTask.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Slic3r {

/// Sends a job straight to a Snapmaker U1's Moonraker over the LAN: plain
/// HTTP upload, then server.files.start_local_print (websocket JSON-RPC) with the filament ->
/// toolhead map and print options. No Flutter, MQTT or cloud involved, and no
/// overall time limit on the upload, so large multi-color jobs are fine.
class U1LanPrintHost : public PrintHost
{
public:
    /// *base_url* is the Moonraker root, e.g. "http://192.168.1.113".
    U1LanPrintHost(std::string base_url, U1PrintTask::Options options);
    ~U1LanPrintHost() override = default;

    /// Reads print_task_config from the printer. Tries *host* as given (port 80
    /// through the printer's web proxy), then Moonraker's own port 7125. On
    /// success returns the parsed state and sets *base_url* to the root that
    /// answered; on failure returns nullopt and sets *error*.
    static std::optional<U1PrintTask::PrinterState> query_printer(const std::string &host, std::string &base_url, wxString &error);

    /// Reads the loaded filaments and nozzles for the slicer's Sync Filament
    /// flow (see U1PrintTask::build_filament_report). *host* as in query_printer.
    static std::optional<nlohmann::json> query_filament_report(const std::string &host, wxString &error);

    const char *get_name() const override { return "Snapmaker U1 (LAN)"; }
    bool        test(wxString &curl_msg) const override;
    wxString    get_test_ok_msg() const override;
    wxString    get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override
    {
        return PrintHostPostUploadAction::StartPrint;
    }
    std::string get_host() const override { return m_base_url; }

private:
    bool start_print(const std::string &path, ErrorFn error_fn) const;

    std::string          m_base_url;
    U1PrintTask::Options m_options;
};

} // namespace Slic3r

#endif // slic3r_U1LanPrintHost_hpp_
