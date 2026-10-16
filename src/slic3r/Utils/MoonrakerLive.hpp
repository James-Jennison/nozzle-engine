#ifndef slic3r_MoonrakerLive_hpp_
#define slic3r_MoonrakerLive_hpp_

#include "libslic3r/PrinterSnapshot.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

/// Live connection to one Moonraker printer for the native printer panel.
///
/// A worker thread discovers the printer's capabilities over HTTP, subscribes
/// to its objects on /websocket and keeps a PrinterPanel::StatusTracker up to
/// date, reconnecting with backoff when the link drops. Callbacks run on the
/// worker thread; the GUI marshals them to the main thread itself.
class MoonrakerLiveClient
{
public:
    enum class Link { Connecting, Connected, Unreachable };

    struct Callbacks
    {
        std::function<void(const PrinterPanel::Capabilities &)> capabilities;
        std::function<void(const PrinterPanel::Snapshot &)>     snapshot;
        std::function<void(Link, const std::string &detail)>    link;
    };
    using ErrorFn = std::function<void(const std::string &error)>;

    /// *base_url* is the printer root, e.g. "http://192.168.1.113".
    MoonrakerLiveClient(std::string base_url, Callbacks callbacks);
    ~MoonrakerLiveClient();

    MoonrakerLiveClient(const MoonrakerLiveClient &)            = delete;
    MoonrakerLiveClient &operator=(const MoonrakerLiveClient &) = delete;

    const std::string &base_url() const { return m_base_url; }

    /// Fire-and-forget controls. *on_error* runs on an HTTP thread.
    void send_gcode(const std::string &script, ErrorFn on_error) const;
    void pause(ErrorFn on_error) const { post("/printer/print/pause", "{}", std::move(on_error)); }
    void resume(ErrorFn on_error) const { post("/printer/print/resume", "{}", std::move(on_error)); }
    void cancel(ErrorFn on_error) const { post("/printer/print/cancel", "{}", std::move(on_error)); }

private:
    void run();
    bool session();
    /// *json_body* must not be empty: Http posts without a body read from an
    /// unset curl read callback and crash.
    void post(const std::string &path, const std::string &json_body, ErrorFn on_error) const;
    std::optional<double> estimated_time(const std::string &filename) const;

    std::string       m_base_url;
    Callbacks         m_callbacks;
    std::atomic<bool> m_stop{false};
    std::thread       m_thread;
};

} // namespace Slic3r

#endif // slic3r_MoonrakerLive_hpp_
