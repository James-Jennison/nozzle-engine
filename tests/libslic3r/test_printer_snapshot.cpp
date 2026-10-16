#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/PrinterSnapshot.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;
using namespace Slic3r::PrinterPanel;

namespace {

// Read-only captures of printer.objects.list + a status query from real printers.
json fixture(const std::string &name)
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/moonraker/" + name + "_snapshot.json");
    REQUIRE(in.good());
    return json::parse(in);
}

const json u1_webcams = json::parse(R"([{"name":"case","enabled":true,"service":"webrtc-camerastreamer",
    "stream_url":"/webcam/webrtc","snapshot_url":"/webcam/snapshot.jpg"}])");
const json cc_webcams = json::parse(R"([{"name":"webcam","enabled":true,"service":"mjpegstreamer-adaptive",
    "stream_url":"/webcam/?action=stream","snapshot_url":"/webcam/?action=snapshot"}])");

} // namespace

TEST_CASE("Snapmaker U1 capabilities are discovered from its objects", "[PrinterSnapshot]")
{
    const json        u1   = fixture("u1");
    const Capabilities caps = discover(u1["objects"], u1_webcams);

    CHECK(caps.extruders == std::vector<std::string>{"extruder", "extruder1", "extruder2", "extruder3"});
    CHECK(caps.heated_bed);
    CHECK(caps.part_fan);
    CHECK(caps.toolhead_materials);
    CHECK(caps.defect_detection);
    REQUIRE(caps.chamber_sensor);
    CHECK(caps.chamber_sensor->name == "cavity");
    REQUIRE(caps.lights.size() == 1);
    CHECK(caps.lights[0].label == "Cavity led");

    // Per-toolhead fans (e1_fan...) are driven by M106, not user controls.
    std::vector<std::string> fans;
    for (const auto &f : caps.fans)
        fans.push_back(f.name);
    CHECK(std::find(fans.begin(), fans.end(), "cavity_fan") != fans.end());
    CHECK(std::find(fans.begin(), fans.end(), "e2_fan") == fans.end());

    REQUIRE(caps.webcams.size() == 1);
    CHECK(caps.webcams[0].webrtc);
}

TEST_CASE("Snapmaker U1 snapshot reflects the running multi-toolhead print", "[PrinterSnapshot]")
{
    const json    u1 = fixture("u1");
    StatusTracker tracker(discover(u1["objects"], u1_webcams));
    tracker.merge(u1["status"]);
    const Snapshot s = tracker.snapshot();

    CHECK(s.ready);
    CHECK(s.warnings.empty());
    CHECK(s.job_state == JobState::Printing);
    CHECK(s.filename == "Cube_TPU_27m50s.gcode");
    CHECK(s.current_layer == 44);
    CHECK(s.total_layers == 135);
    CHECK(s.progress == Catch::Approx(0.313).margin(0.001));
    REQUIRE(s.remaining);
    CHECK(*s.remaining > 0.);

    REQUIRE(s.toolheads.size() == 4);
    CHECK(s.toolheads[2].active);
    CHECK_FALSE(s.toolheads[0].active);
    CHECK(s.toolheads[2].heater.temperature == Catch::Approx(240.));
    CHECK(s.toolheads[2].material == "TPU");
    CHECK(s.toolheads[2].color == "#DD0000");
    CHECK(s.toolheads[0].material == "PLA");
    REQUIRE(s.bed);
    CHECK(s.bed->target == Catch::Approx(35.));
    REQUIRE(s.chamber);
    CHECK(s.part_fan_percent == 100);
    REQUIRE(s.lights.size() == 1);
    CHECK(s.lights[0].second); // cavity LED white channel is on
}

TEST_CASE("Elegoo CC gets a single-extruder panel without U1 extras", "[PrinterSnapshot]")
{
    const json         cc   = fixture("elegoo_cc");
    const Capabilities caps = discover(cc["objects"], cc_webcams);

    CHECK(caps.extruders == std::vector<std::string>{"extruder"});
    CHECK_FALSE(caps.toolhead_materials);
    CHECK_FALSE(caps.defect_detection);
    REQUIRE(caps.chamber_sensor);
    CHECK(caps.chamber_sensor->name == "chamber");
    CHECK(caps.lights.size() == 2);
    CHECK(caps.fans.size() == 2); // aux_fan, case_fan
    REQUIRE(caps.webcams.size() == 1);
    CHECK_FALSE(caps.webcams[0].webrtc);

    StatusTracker tracker(caps);
    tracker.merge(cc["status"]);
    const Snapshot s = tracker.snapshot();
    REQUIRE(s.toolheads.size() == 1);
    CHECK(s.toolheads[0].active);
    CHECK(s.toolheads[0].material.empty());
    CHECK(s.chamber);
    CHECK(s.job_state != JobState::Printing);
    CHECK_FALSE(s.remaining);
}

TEST_CASE("Status deltas merge field by field", "[PrinterSnapshot]")
{
    Capabilities caps;
    caps.extruders  = {"extruder"};
    caps.heated_bed = true;
    StatusTracker tracker(caps);
    tracker.merge(json::parse(R"({"webhooks":{"state":"ready"},"extruder":{"temperature":20,"target":0},
        "heater_bed":{"temperature":21,"target":60},"print_stats":{"state":"printing","print_duration":100},
        "virtual_sdcard":{"progress":0.25}})"));
    tracker.merge(json::parse(R"({"extruder":{"temperature":215.4}})"));

    Snapshot s = tracker.snapshot();
    CHECK(s.toolheads[0].heater.temperature == Catch::Approx(215.4));
    CHECK(s.toolheads[0].heater.target == Catch::Approx(0.)); // untouched by the delta
    CHECK(s.bed->target == Catch::Approx(60.));
    CHECK(*s.remaining == Catch::Approx(300.));                     // duration-based
    CHECK(*tracker.snapshot(1000.).remaining == Catch::Approx(750.)); // slicer estimate wins

    tracker.merge(json::parse(R"({"webhooks":{"state":"shutdown","state_message":"MCU 'mcu' shutdown"}})"));
    s = tracker.snapshot();
    CHECK_FALSE(s.ready);
    REQUIRE(s.warnings.size() == 1);
    CHECK(s.warnings[0] == "MCU 'mcu' shutdown");
}

TEST_CASE("Panel commands are standard Klipper G-code", "[PrinterSnapshot]")
{
    CHECK(Commands::set_speed(150) == "M220 S150");
    CHECK(Commands::set_speed(9999) == "M220 S500");
    CHECK(Commands::set_flow(98) == "M221 S98");
    CHECK(Commands::set_part_fan(100) == "M106 S255");
    CHECK(Commands::set_part_fan(40) == "M106 S102");
    CHECK(Commands::set_fan({"fan_generic cavity_fan", "cavity_fan", "Cavity fan"}, 40) ==
          "SET_FAN_SPEED FAN=cavity_fan SPEED=0.40");
    CHECK(Commands::set_light({"led case", "case", "Case"}, true) == "SET_LED LED=case RED=1 GREEN=1 BLUE=1 WHITE=1");
    CHECK(Commands::set_light({"output_pin caselight", "caselight", "Caselight"}, false) == "SET_PIN PIN=caselight VALUE=0");
    CHECK(Commands::exclude_object("Cube_id_0") == "EXCLUDE_OBJECT NAME=\"Cube_id_0\"");
    CHECK(format_duration(5040) == "1 h 24 min");
    CHECK(format_duration(720) == "12 min");
    CHECK(format_duration(45) == "45 s");
}

TEST_CASE("U1 toolheads report vendor, sub-type and whether a tag set them", "[PrinterSnapshot]")
{
    const json    u1 = fixture("u1");
    StatusTracker tracker(discover(u1["objects"], u1_webcams));
    tracker.merge(u1["status"]);
    tracker.merge(json::parse(R"({"print_task_config":{"filament_official":[true,false,false,true],
        "filament_vendor":["Polymaker","Generic","NONE","amazon basics"],"filament_sub_type":["Basic","NONE","95A","95A"]}})"));
    const Snapshot s = tracker.snapshot();
    REQUIRE(s.toolheads.size() == 4);
    CHECK(s.toolheads[0].from_tag);
    CHECK_FALSE(s.toolheads[0].editable);
    CHECK(s.toolheads[0].vendor == "Polymaker");
    CHECK(s.toolheads[1].editable);
    CHECK(s.toolheads[1].sub_type.empty()); // "NONE" is shown as empty
    CHECK(s.toolheads[2].vendor.empty());
    CHECK(s.toolheads[3].vendor == "amazon basics");
}
