#include "PrinterSnapshot.hpp"
#include "U1PrintTask.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace Slic3r {
namespace PrinterPanel {

namespace {

using json = nlohmann::json;

// "cavity_fan" -> "Cavity fan"
std::string humanize(std::string name)
{
    std::replace(name.begin(), name.end(), '_', ' ');
    if (!name.empty())
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

Control make_control(const std::string &object)
{
    const auto space = object.find(' ');
    const std::string name = space == std::string::npos ? object : object.substr(space + 1);
    return {object, name, humanize(name)};
}

double num(const json &obj, const char *key, double fallback = 0.)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<double>() : fallback;
}

std::string str(const json &obj, const char *key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const json &object_or_empty(const json &state, const std::string &key)
{
    static const json empty = json::object();
    const auto it = state.find(key);
    return it != state.end() && it->is_object() ? *it : empty;
}

// Recursive merge: deltas only carry the fields that changed.
void merge_into(json &dst, const json &src)
{
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it->is_object() && dst.contains(it.key()) && dst[it.key()].is_object())
            merge_into(dst[it.key()], *it);
        else
            dst[it.key()] = *it;
    }
}

int percent(double fraction) { return int(std::lround(fraction * 100.)); }

} // namespace

Capabilities discover(const json &objects, const json &webcams)
{
    static const std::regex extruder_re("^extruder\\d*$");
    // Per-toolhead part fans (U1 "e2_fan") follow the active toolhead's M106.
    static const std::regex toolhead_fan_re("^(e|extruder|t|tool)\\d+_.*fan$", std::regex::icase);
    static const std::regex light_pin_re("(light|led|lamp)", std::regex::icase);
    static const std::regex chamber_re("(chamber|cavity|enclosure)", std::regex::icase);

    Capabilities caps;
    if (objects.is_array()) {
        for (const auto &o : objects) {
            if (!o.is_string())
                continue;
            const std::string object = o.get<std::string>();
            if (std::regex_match(object, extruder_re))
                caps.extruders.push_back(object);
            else if (object == "heater_bed")
                caps.heated_bed = true;
            else if (object == "fan")
                caps.part_fan = true;
            else if (object == "exclude_object")
                caps.exclude_object = true;
            else if (object == "print_task_config")
                caps.toolhead_materials = true;
            else if (object == "defect_detection")
                caps.defect_detection = true;
            else if (object == "filament_detect")
                caps.filament_tags = true;
            else if (object.rfind("fan_generic ", 0) == 0) {
                Control fan = make_control(object);
                if (!std::regex_match(fan.name, toolhead_fan_re))
                    caps.fans.push_back(fan);
            } else if (object.rfind("led ", 0) == 0 || object.rfind("neopixel ", 0) == 0)
                caps.lights.push_back(make_control(object));
            else if (object.rfind("output_pin ", 0) == 0) {
                Control pin = make_control(object);
                if (std::regex_search(pin.name, light_pin_re))
                    caps.lights.push_back(pin);
            } else if (object.rfind("temperature_sensor ", 0) == 0 && !caps.chamber_sensor) {
                Control sensor = make_control(object);
                if (std::regex_search(sensor.name, chamber_re))
                    caps.chamber_sensor = sensor;
            }
        }
    }
    // "extruder" before "extruder1" ... "extruder10".
    std::sort(caps.extruders.begin(), caps.extruders.end(), [](const std::string &a, const std::string &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });

    if (webcams.is_array()) {
        for (const auto &w : webcams) {
            if (!w.is_object() || !w.value("enabled", true))
                continue;
            Webcam cam;
            cam.name         = str(w, "name");
            cam.service      = str(w, "service");
            cam.stream_url   = str(w, "stream_url");
            cam.snapshot_url = str(w, "snapshot_url");
            cam.webrtc       = cam.service.find("webrtc") != std::string::npos;
            caps.webcams.push_back(cam);
        }
    }
    return caps;
}

json subscription(const Capabilities &caps)
{
    json objects = json::object();
    for (const char *o : {"webhooks", "print_stats", "virtual_sdcard", "display_status", "gcode_move", "toolhead",
                          "pause_resume", "idle_timeout"})
        objects[o] = nullptr;
    for (const auto &e : caps.extruders)
        objects[e] = json::array({"temperature", "target"});
    if (caps.heated_bed)
        objects["heater_bed"] = json::array({"temperature", "target"});
    if (caps.chamber_sensor)
        objects[caps.chamber_sensor->object] = json::array({"temperature"});
    if (caps.part_fan)
        objects["fan"] = json::array({"speed"});
    for (const auto &f : caps.fans)
        objects[f.object] = json::array({"speed"});
    for (const auto &l : caps.lights)
        objects[l.object] = nullptr;
    if (caps.exclude_object)
        objects["exclude_object"] = nullptr;
    if (caps.toolhead_materials)
        objects["print_task_config"] = json::array({"filament_exist", "filament_type", "filament_color_rgba", "filament_vendor",
                                                    "filament_sub_type", "filament_official", "filament_edit"});
    if (caps.filament_tags)
        objects["filament_detect"] = json::array({"info"});
    if (caps.defect_detection)
        objects["defect_detection"] = nullptr;
    return objects;
}

void StatusTracker::merge(const json &delta)
{
    if (delta.is_object())
        merge_into(m_state, delta);
}

Snapshot StatusTracker::snapshot(std::optional<double> estimated_time) const
{
    Snapshot s;

    const json &webhooks = object_or_empty(m_state, "webhooks");
    s.ready              = str(webhooks, "state") == "ready";
    s.klippy_message     = str(webhooks, "state_message");
    if (!webhooks.empty() && !s.ready)
        s.warnings.push_back(s.klippy_message.empty() ? "Klipper is not ready." : s.klippy_message);

    const json &stats = object_or_empty(m_state, "print_stats");
    const std::string state = str(stats, "state");
    s.job_state = state == "printing"  ? JobState::Printing :
                  state == "paused"    ? JobState::Paused :
                  state == "complete"  ? JobState::Complete :
                  state == "cancelled" ? JobState::Cancelled :
                  state == "error"     ? JobState::Error :
                                         JobState::Standby;
    s.filename       = str(stats, "filename");
    s.print_duration = num(stats, "print_duration");
    s.job_message    = str(stats, "message");
    if (const json &info = object_or_empty(stats, "info"); !info.empty()) {
        if (info.contains("current_layer") && info["current_layer"].is_number())
            s.current_layer = info["current_layer"].get<int>();
        if (info.contains("total_layer") && info["total_layer"].is_number() && info["total_layer"].get<int>() > 0)
            s.total_layers = info["total_layer"].get<int>();
    }
    if (s.job_state == JobState::Error)
        s.warnings.push_back(s.job_message.empty() ? "The print stopped with an error." : s.job_message);

    // virtual_sdcard tracks file position, the most direct measure of progress.
    const json &sd = object_or_empty(m_state, "virtual_sdcard");
    s.progress     = std::clamp(num(sd, "progress", num(object_or_empty(m_state, "display_status"), "progress")), 0., 1.);
    if (s.job_state == JobState::Printing || s.job_state == JobState::Paused) {
        if (estimated_time && *estimated_time > 0.)
            s.remaining = std::max(0., *estimated_time * (1. - s.progress));
        else if (s.progress > 0.01 && s.print_duration > 0.)
            s.remaining = std::max(0., s.print_duration / s.progress - s.print_duration);
    }

    const std::string active = str(object_or_empty(m_state, "toolhead"), "extruder");
    for (const auto &e : m_caps.extruders) {
        const json &obj = object_or_empty(m_state, e);
        Toolhead    t;
        t.name               = e;
        t.heater.temperature = num(obj, "temperature");
        t.heater.target      = num(obj, "target");
        t.active             = e == active;
        s.toolheads.push_back(t);
    }
    if (m_caps.toolhead_materials) {
        const U1PrintTask::PrinterState u1 = U1PrintTask::parse_printer_state(object_or_empty(m_state, "print_task_config"));
        const json &cfg  = object_or_empty(m_state, "print_task_config");
        const json &tags = object_or_empty(m_state, "filament_detect");
        auto text_at = [](const json &obj, const char *key, size_t i) {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->is_array() || i >= it->size() || !(*it)[i].is_string())
                return std::string();
            const std::string v = (*it)[i].get<std::string>();
            return v == "NONE" ? std::string() : v;
        };
        auto flag_at = [](const json &obj, const char *key, size_t i) {
            const auto it = obj.find(key);
            return it != obj.end() && it->is_array() && i < it->size() && (*it)[i].is_boolean() && (*it)[i].get<bool>();
        };
        for (size_t i = 0; i < s.toolheads.size() && i < u1.toolheads.size(); ++i) {
            Toolhead &t = s.toolheads[i];
            t.loaded    = u1.toolheads[i].present;
            t.material  = u1.toolheads[i].type;
            t.color     = u1.toolheads[i].color;
            t.vendor    = text_at(cfg, "filament_vendor", i);
            t.sub_type  = text_at(cfg, "filament_sub_type", i);
            t.from_tag  = t.loaded && flag_at(cfg, "filament_official", i);
            t.editable  = t.loaded && !t.from_tag;
            const auto info = tags.find("info");
            if (info != tags.end() && info->is_array() && i < info->size() && (*info)[i].is_object() &&
                !(*info)[i].value("CARD_TYPE", std::string()).empty())
                t.tag = (*info)[i];
        }
    }

    if (m_caps.heated_bed) {
        const json &bed = object_or_empty(m_state, "heater_bed");
        s.bed           = Heater{num(bed, "temperature"), num(bed, "target")};
    }
    if (m_caps.chamber_sensor)
        if (const json &c = object_or_empty(m_state, m_caps.chamber_sensor->object); c.contains("temperature"))
            s.chamber = num(c, "temperature");

    const json &move = object_or_empty(m_state, "gcode_move");
    s.speed_percent  = percent(num(move, "speed_factor", 1.));
    s.flow_percent   = percent(num(move, "extrude_factor", 1.));

    if (m_caps.part_fan)
        s.part_fan_percent = percent(num(object_or_empty(m_state, "fan"), "speed"));
    for (const auto &f : m_caps.fans)
        s.fans.emplace_back(f, percent(num(object_or_empty(m_state, f.object), "speed")));
    for (const auto &l : m_caps.lights) {
        const json &obj = object_or_empty(m_state, l.object);
        bool        on  = num(obj, "value") > 0.;
        if (const auto it = obj.find("color_data"); it != obj.end() && it->is_array())
            for (const auto &led : *it)
                if (led.is_array())
                    for (const auto &channel : led)
                        on = on || (channel.is_number() && channel.get<double>() > 0.);
        s.lights.emplace_back(l, on);
    }
    return s;
}

namespace Commands {

std::string set_speed(int percent) { return "M220 S" + std::to_string(std::clamp(percent, 10, 500)); }
std::string set_flow(int percent) { return "M221 S" + std::to_string(std::clamp(percent, 50, 200)); }
std::string set_part_fan(int percent)
{
    return "M106 S" + std::to_string(int(std::lround(std::clamp(percent, 0, 100) * 2.55)));
}
std::string set_fan(const Control &fan, int percent)
{
    char speed[16];
    std::snprintf(speed, sizeof(speed), "%.2f", std::clamp(percent, 0, 100) / 100.);
    return "SET_FAN_SPEED FAN=" + fan.name + " SPEED=" + speed;
}
std::string set_light(const Control &light, bool on)
{
    const char *v = on ? "1" : "0";
    if (light.object.rfind("output_pin ", 0) == 0)
        return "SET_PIN PIN=" + light.name + " VALUE=" + v;
    return "SET_LED LED=" + light.name + " RED=" + v + " GREEN=" + v + " BLUE=" + v + " WHITE=" + v;
}
std::string exclude_object(const std::string &name) { return "EXCLUDE_OBJECT NAME=\"" + name + "\""; }

} // namespace Commands

std::string format_duration(double seconds)
{
    const long total = std::max(0L, std::lround(seconds));
    const long h = total / 3600, m = (total % 3600) / 60;
    if (h > 0)
        return std::to_string(h) + " h " + std::to_string(m) + " min";
    if (m > 0)
        return std::to_string(m) + " min";
    return std::to_string(total) + " s";
}

} // namespace PrinterPanel
} // namespace Slic3r
