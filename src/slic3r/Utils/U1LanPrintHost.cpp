#include "U1LanPrintHost.hpp"

#include "Http.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

constexpr long QUERY_TIMEOUT_S = 5;

std::string with_scheme(std::string host)
{
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    if (!boost::istarts_with(host, "http://") && !boost::istarts_with(host, "https://"))
        host = "http://" + host;
    return host;
}

bool has_port(const std::string &url)
{
    const auto scheme_end = url.find("://");
    const auto authority  = url.substr(scheme_end == std::string::npos ? 0 : scheme_end + 3);
    return authority.find(':') != std::string::npos;
}

} // namespace

U1LanPrintHost::U1LanPrintHost(std::string base_url, U1PrintTask::Options options)
    : m_base_url(with_scheme(std::move(base_url))), m_options(std::move(options))
{}

std::optional<U1PrintTask::PrinterState> U1LanPrintHost::query_printer(const std::string &host, std::string &base_url, wxString &error)
{
    std::vector<std::string> candidates{with_scheme(host)};
    if (!has_port(candidates.front()))
        candidates.push_back(candidates.front() + ":7125");

    for (const std::string &candidate : candidates) {
        std::optional<U1PrintTask::PrinterState> state;
        Http::get(candidate + "/printer/objects/query?print_task_config")
            .timeout_connect(QUERY_TIMEOUT_S)
            .timeout_max(QUERY_TIMEOUT_S)
            .on_complete([&](std::string body, unsigned) {
                try {
                    const auto json = nlohmann::json::parse(body);
                    state = U1PrintTask::parse_printer_state(json.at("result").at("status").at("print_task_config"));
                } catch (const std::exception &e) {
                    error = GUI::format_wxstr(_L("Unexpected reply from the printer: %s"), e.what());
                }
            })
            .on_error([&](std::string body, std::string err, unsigned status) {
                error = status != 0 ? wxString::Format("HTTP %u: %s", status, wxString::FromUTF8(body)) : wxString::FromUTF8(err);
            })
            .perform_sync();
        if (state) {
            base_url = candidate;
            return state;
        }
        BOOST_LOG_TRIVIAL(warning) << "U1 LAN: print_task_config query failed at " << candidate;
    }
    return std::nullopt;
}

std::optional<nlohmann::json> U1LanPrintHost::query_filament_report(const std::string &host, wxString &error)
{
    std::string base;
    if (!query_printer(host, base, error))
        return std::nullopt;

    std::optional<nlohmann::json> report;
    Http::get(base + "/printer/objects/query?print_task_config&extruder&extruder1&extruder2&extruder3")
        .timeout_connect(QUERY_TIMEOUT_S)
        .timeout_max(QUERY_TIMEOUT_S)
        .on_complete([&](std::string body, unsigned) {
            try {
                auto built = U1PrintTask::build_filament_report(nlohmann::json::parse(body).at("result").at("status"));
                if (!built.is_null())
                    report = std::move(built);
                else
                    error = _L("The printer did not report its loaded filaments.");
            } catch (const std::exception &e) {
                error = GUI::format_wxstr(_L("Unexpected reply from the printer: %s"), e.what());
            }
        })
        .on_error([&](std::string, std::string err, unsigned) { error = wxString::FromUTF8(err); })
        .perform_sync();
    return report;
}

bool U1LanPrintHost::test(wxString &curl_msg) const
{
    std::string base;
    return query_printer(m_base_url, base, curl_msg).has_value();
}

wxString U1LanPrintHost::get_test_ok_msg() const { return _L("Connected to the Snapmaker U1 over the local network."); }

wxString U1LanPrintHost::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not reach the Snapmaker U1 on the local network"), msg);
}

bool U1LanPrintHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn /*info_fn*/) const
{
    std::string error;
    if (!U1PrintTask::validate(m_options, error)) {
        error_fn(wxString::FromUTF8(error));
        return false;
    }

    const std::string filename = upload_data.upload_path.filename().string();
    const std::string folder   = upload_data.upload_path.parent_path().generic_string();
    const std::string path     = folder.empty() ? filename : folder + "/" + filename;

    BOOST_LOG_TRIVIAL(warning) << "U1 LAN: uploading " << upload_data.source_path << " as " << path << " to " << m_base_url;

    bool res = true;
    // No timeout_max: the U1 accepts uploads at ~1.25 MB/s, so a large job
    // legitimately takes minutes. Stalls are the user's to cancel.
    Http::post(m_base_url + "/server/files/upload")
        .form_add("root", "gcodes")
        .form_add("path", folder)
        .form_add("print", "false")
        .form_add_file("file", upload_data.source_path.string(), filename)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "U1 LAN: upload complete, HTTP " << status;
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "U1 LAN: upload failed: " << err << ", HTTP " << status << ", body: " << body;
            error_fn(format_error(body, err, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel)
                res = false;
        })
        .perform_sync();

    if (!res || upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return res;
    return start_print(path, error_fn);
}

bool U1LanPrintHost::start_print(const std::string &path, ErrorFn error_fn) const
{
    namespace beast     = boost::beast;
    namespace websocket = beast::websocket;
    using tcp           = boost::asio::ip::tcp;

    // The U1 registers start_local_print for websocket/MQTT only (HTTP is
    // excluded), so call it as JSON-RPC on Moonraker's websocket.
    const int            request_id = 4242;
    const nlohmann::json request    = {{"jsonrpc", "2.0"},
                                       {"method", "server.files.start_local_print"},
                                       {"params", U1PrintTask::build_start_local_print_body(path, 1, m_options)},
                                       {"id", request_id}};
    BOOST_LOG_TRIVIAL(warning) << "U1 LAN: start_local_print " << request.dump();

    std::string host = m_base_url.substr(m_base_url.find("://") + 3);
    std::string port = "80";
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }

    try {
        boost::asio::io_context            ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(ws).connect(tcp::resolver{ioc}.resolve(host, port));
        ws.handshake(host + ":" + port, "/websocket");
        ws.write(boost::asio::buffer(request.dump()));

        // Moonraker also pushes status notifications on this socket; wait for
        // our reply. The printer scans metadata before starting, so allow time.
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(60));
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            const auto reply = nlohmann::json::parse(beast::buffers_to_string(buffer.data()), nullptr, false);
            if (reply.is_discarded() || reply.value("id", -1) != request_id)
                continue;
            beast::error_code ignored;
            ws.close(websocket::close_code::normal, ignored);

            BOOST_LOG_TRIVIAL(warning) << "U1 LAN: start_local_print reply " << reply.dump();
            if (reply.contains("error")) {
                error_fn(GUI::format_wxstr(_L("The file was uploaded, but the print could not be started: %s"),
                                           reply["error"].value("message", std::string("unknown error"))));
                return false;
            }
            const auto result = reply.value("result", nlohmann::json::object());
            if (result.is_object() && result.value("state", "") == "error") {
                error_fn(GUI::format_wxstr(_L("The printer refused to start the print: %s"),
                                           result.value("message", std::string("unknown error"))));
                return false;
            }
            return true;
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "U1 LAN: start_local_print failed: " << e.what();
        error_fn(GUI::format_wxstr(_L("The file was uploaded, but the print could not be started: %s"), e.what()));
        return false;
    }
}

} // namespace Slic3r
