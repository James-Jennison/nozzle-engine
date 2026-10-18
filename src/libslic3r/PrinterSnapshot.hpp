#ifndef slic3r_PrinterSnapshot_hpp_
#define slic3r_PrinterSnapshot_hpp_

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

/// Printer-agnostic model behind the native printer panel.
///
/// Everything is discovered from what Moonraker reports (printer.objects.list,
/// /server/webcams/list), so the same panel serves a 4-toolhead Snapmaker U1,
/// a single-extruder Elegoo CC or any other Klipper printer. Model-specific
/// extras (the U1's loaded-filament info, defect detection) switch on when the
/// printer exposes the object that carries them, not by printer name.
///
/// No wxWidgets or network dependency, so it is unit-tested against captured
/// printer snapshots in tests/data/moonraker.
namespace PrinterPanel {

struct Webcam
{
    std::string name;
    std::string service;      ///< e.g. "webrtc-camerastreamer", "mjpegstreamer-adaptive"
    std::string stream_url;   ///< relative to the printer root unless absolute
    std::string snapshot_url;
    bool        webrtc = false;
};

struct Control
{
    std::string object; ///< Moonraker object, e.g. "fan_generic cavity_fan"
    std::string name;   ///< Klipper name, e.g. "cavity_fan"
    std::string label;  ///< "Cavity fan"
};

/// What this printer has. Sections of the panel without data are hidden.
struct Capabilities
{
    std::vector<std::string> extruders;         ///< "extruder", "extruder1", ...
    bool                     heated_bed = false;
    std::optional<Control>   chamber_sensor;
    bool                     part_fan = false;  ///< [fan], set with M106
    std::vector<Control>     fans;              ///< user-adjustable fan_generic
    std::vector<Control>     lights;            ///< led / output_pin lights
    bool                     exclude_object = false;
    bool                     toolhead_materials = false; ///< U1 print_task_config
    bool                     defect_detection = false;   ///< U1
    bool                     filament_tags = false;      ///< U1 filament_detect (RFID)
    std::vector<Webcam>      webcams;
};

/// *objects* is printer.objects.list's "objects" array; *webcams* is the
/// "webcams" array from /server/webcams/list (may be null).
Capabilities discover(const nlohmann::json &objects, const nlohmann::json &webcams);

/// Objects to subscribe to (printer.objects.subscribe "objects" argument).
nlohmann::json subscription(const Capabilities &caps);

enum class JobState { Standby, Printing, Paused, Complete, Cancelled, Error };

struct Heater
{
    double temperature = 0.;
    double target      = 0.;
};

struct Toolhead
{
    std::string name;   ///< Klipper extruder object
    Heater      heater;
    bool        active = false;
    // Loaded material, when the printer reports it (U1).
    bool        loaded = false;
    std::string material;
    std::string color;  ///< "#RRGGBB"
    std::string vendor;
    std::string sub_type;
    bool        from_tag = false; ///< read from an RFID tag; the printer won't accept edits
    bool        editable = false; ///< loaded and set manually
    nlohmann::json tag;           ///< raw filament_detect entry when a tag was read
};

struct Snapshot
{
    bool        ready = false;        ///< Klippy ready
    std::string klippy_message;       ///< why not ready

    JobState    job_state = JobState::Standby;
    std::string filename;
    double      progress = 0.;        ///< 0..1
    double      print_duration = 0.;  ///< seconds actually printing
    std::optional<double> remaining;  ///< seconds
    std::optional<int>    current_layer;
    std::optional<int>    total_layers;
    std::string job_message;

    std::vector<Toolhead> toolheads;
    std::optional<Heater> bed;
    std::optional<double> chamber;
    int                   speed_percent = 100;
    int                   flow_percent  = 100;
    std::optional<int>    part_fan_percent;
    std::vector<std::pair<Control, int>>  fans;   ///< percent
    std::vector<std::pair<Control, bool>> lights; ///< on/off

    /// Printer problems for the status slot; empty means "printing normally".
    std::vector<std::string> warnings;
};

/// Accumulates Moonraker status: the initial query result, then each
/// notify_status_update delta (which only carries changed fields).
class StatusTracker
{
public:
    explicit StatusTracker(Capabilities caps) : m_caps(std::move(caps)) {}

    void merge(const nlohmann::json &status_delta);
    /// *estimated_time* is the slicer's total estimate for the current file
    /// (from file metadata), used for a steadier remaining time.
    Snapshot snapshot(std::optional<double> estimated_time = std::nullopt) const;

    const Capabilities &capabilities() const { return m_caps; }

private:
    Capabilities   m_caps;
    nlohmann::json m_state = nlohmann::json::object();
};

/// G-code scripts for the panel's controls (sent via /printer/gcode/script).
namespace Commands {
std::string set_speed(int percent);             ///< M220
std::string set_flow(int percent);              ///< M221
std::string set_part_fan(int percent);          ///< M106
std::string set_fan(const Control &fan, int percent);
std::string set_light(const Control &light, bool on);
std::string exclude_object(const std::string &name);
} // namespace Commands

std::string format_duration(double seconds); ///< "1 h 24 min", "12 min", "45 s"

} // namespace PrinterPanel
} // namespace Slic3r

#endif // slic3r_PrinterSnapshot_hpp_
