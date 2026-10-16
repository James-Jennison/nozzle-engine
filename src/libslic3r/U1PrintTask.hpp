#ifndef slic3r_U1PrintTask_hpp_
#define slic3r_U1PrintTask_hpp_

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

/// Request building for the Snapmaker U1's native LAN print start.
///
/// The U1's Moonraker exposes POST /server/files/start_local_print
/// (Snapmaker/u1-moonraker, components/snapmakercloud.py). Each key in
/// "options" is upper-cased into a parameter of Klipper's
/// SDCARD_PRINT_FILE_WITH_PARAMETERS, which hands them to
/// print_task_config.cmd_SET_PRINT_TASK_PARAMETERS (Snapmaker/u1-klipper).
/// The firmware applies the filament -> toolhead remap itself, so the G-code
/// is uploaded unchanged.
///
/// No wxWidgets or network dependency, so it can be unit-tested directly.
namespace U1PrintTask {

/// Firmware limits from print_task_config.py.
constexpr int LOGICAL_EXTRUDER_COUNT  = 32;
constexpr int PHYSICAL_EXTRUDER_COUNT = 4;

/// Marks a slicer filament that the print does not use / should not be remapped.
constexpr int UNMAPPED = -1;

struct Options
{
    /// Index = logical extruder (slicer filament / T index), value = physical
    /// toolhead 0..3, or UNMAPPED to leave that entry at the firmware default.
    std::vector<int> filament_to_toolhead;

    /// Unset options are omitted so the printer keeps its own default.
    std::optional<bool> bed_level;
    std::optional<bool> flow_calibrate;
    std::optional<bool> shaper_calibrate;
    std::optional<bool> time_lapse_camera;

    /// Physical toolheads to flow-calibrate; only sent when flow_calibrate is true.
    std::vector<int> flow_calibrate_toolheads;
};

/// A filament as the send dialog sees it, either loaded in a toolhead or used by the plate.
struct Filament
{
    bool        present = false; ///< Loaded in the toolhead / used by this plate.
    std::string type;            ///< e.g. "PLA", upper-case.
    std::string color;           ///< "#RRGGBB", upper-case; empty when unknown.
};

/// What the printer currently reports in the print_task_config object
/// (GET /printer/objects/query?print_task_config).
struct PrinterState
{
    std::vector<Filament> toolheads; ///< Always PHYSICAL_EXTRUDER_COUNT entries.
    bool bed_level         = false;
    bool flow_calibrate    = false;
    bool shaper_calibrate  = false;
    bool time_lapse_camera = false;
};

/// Parses the print_task_config status object. Missing or malformed fields fall
/// back to "empty toolhead" / false instead of failing, so the dialog still opens.
PrinterState parse_printer_state(const nlohmann::json &print_task_config);

/// Suggests a toolhead for each plate filament: a loaded toolhead with the same
/// type and the closest color, else any loaded toolhead with the closest color,
/// else the same index. Filaments the plate does not use stay UNMAPPED.
std::vector<int> suggest_mapping(const std::vector<Filament> &plate_filaments, const std::vector<Filament> &toolheads);

/// Builds the filament report the slicer's Sync Filament flow consumes
/// (print_task_config's filament fields plus per-nozzle "nozzle_diameters"
/// as "0.4"-style strings and "nozzle_volume_types") from a Moonraker
/// status object holding "print_task_config" and "extruder".."extruder3".
/// Returns null when print_task_config is missing.
nlohmann::json build_filament_report(const nlohmann::json &status);

/// SET_PRINT_FILAMENT_CONFIG for the spool on *toolhead* (0-based). *color*
/// is "#RRGGBB". Quotes and control characters are stripped from the text
/// fields; empty text becomes "NONE" as the firmware reports it. The firmware
/// refuses tag-read ("official") spools unless *force* is set, which is how
/// PAXX extended firmware's filament manager overrides tag data too.
/// Returns empty when toolhead or color is invalid.
std::string set_filament_command(int toolhead, const std::string &vendor, const std::string &type,
                                 const std::string &sub_type, const std::string &color, bool force = false);

/// Clears and re-reads the RFID tag on *toolhead* (FILAMENT_DT_CLEAR +
/// FILAMENT_DT_UPDATE, stock and PAXX firmware). Empty when out of range.
std::string reread_tag_command(int toolhead);

/// Common values the U1's RFID protocol maps (PAXX filament_detect docs);
/// offered as suggestions, other text is still accepted.
const std::vector<std::string> &known_filament_types();
const std::vector<std::string> &known_filament_sub_types();

/// Returns false and sets *error* when the firmware would reject the options.
bool validate(const Options &options, std::string &error);

/// Formats mapped entries as the firmware's MAP_TABLE literal, e.g. "[[0,2],[1,0]]".
/// UNMAPPED entries are skipped. The firmware parses it with ast.literal_eval.
std::string format_map_table(const std::vector<int> &filament_to_toolhead);

/// Formats a list of toolhead indices, e.g. "[0,3]".
std::string format_index_list(const std::vector<int> &indices);

/// Body for POST /server/files/start_local_print. *path* is relative to the
/// printer's gcodes root, exactly as uploaded. Booleans are sent as 0/1 because
/// the firmware reads them with get_int and Python's str(True) would not parse.
/// Call validate() first; this does not re-check the options.
nlohmann::json build_start_local_print_body(const std::string &path, int print_plate, const Options &options);

} // namespace U1PrintTask
} // namespace Slic3r

#endif // slic3r_U1PrintTask_hpp_
