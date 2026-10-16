#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/SemanticPaint.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r;
namespace SP = Slic3r::SemanticPaint;

TEST_CASE("SemanticPaint: a cube splits into its six sides", "[SemanticPaint]")
{
    const TriangleMesh cube = make_cube(10., 10., 10.);
    const auto seg = SP::segment_mesh(cube.its);
    REQUIRE(seg.segments.size() == 6);
    REQUIRE(seg.face_segment.size() == cube.its.indices.size());
    double total = 0.;
    for (const auto &s : seg.segments) {
        CHECK(s.area_fraction == Catch::Approx(1. / 6.).margin(1e-6));
        total += s.area_fraction;
    }
    CHECK(total == Catch::Approx(1.).margin(1e-6));
}

TEST_CASE("SemanticPaint: a sphere stays in few segments", "[SemanticPaint]")
{
    const TriangleMesh sphere = make_sphere(10., 0.2);
    const auto seg = SP::segment_mesh(sphere.its);
    CHECK(seg.segments.size() >= 1);
    CHECK(seg.segments.size() <= 32);
}

TEST_CASE("SemanticPaint: views render non-empty and encode to PNG", "[SemanticPaint]")
{
    const TriangleMesh cube = make_cube(10., 20., 30.);
    const auto seg = SP::segment_mesh(cube.its);
    for (const auto &view : SP::default_views()) {
        const SP::Image img = SP::render_view(cube.its, seg, view, 128);
        REQUIRE(img.rgb.size() == 128u * 128u * 3u);
        size_t background = 0;
        for (size_t i = 0; i < img.rgb.size(); i += 3)
            background += img.rgb[i] == 235 && img.rgb[i + 1] == 235 && img.rgb[i + 2] == 235;
        CHECK(background < 128u * 128u);
        CHECK(background > 0);
        const std::string png = SP::encode_png(img);
        REQUIRE(png.size() > 8);
        CHECK(png.substr(1, 3) == "PNG");
    }
}

TEST_CASE("SemanticPaint: base64", "[SemanticPaint]")
{
    CHECK(SP::base64_encode("") == "");
    CHECK(SP::base64_encode("f") == "Zg==");
    CHECK(SP::base64_encode("fo") == "Zm8=");
    CHECK(SP::base64_encode("foo") == "Zm9v");
    CHECK(SP::base64_encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("SemanticPaint: colour names", "[SemanticPaint]")
{
    CHECK(SP::color_name("#000000") == "black");
    CHECK(SP::color_name("#FFFFFF") == "white");
    CHECK(SP::color_name("#FF0000") == "red");
    CHECK(SP::color_name("#0000FF") == "blue");
    CHECK(SP::color_name("#00C800") == "green");
}

TEST_CASE("SemanticPaint: prompt and provider round trips", "[SemanticPaint]")
{
    const TriangleMesh cube = make_cube(10., 10., 10.);
    const auto seg = SP::segment_mesh(cube.its);
    const std::vector<SP::FilamentInfo> fil{{1, "PLA", "#FF0000", "Red"}, {2, "PLA", "#FFFFFF", ""}};
    const std::string prompt = SP::build_prompt(seg, fil, "box", {"front", "top"}, "make it festive");
    CHECK(prompt.find("box") != std::string::npos);
    CHECK(prompt.find("festive") != std::string::npos);
    CHECK(prompt.find("#FF0000") != std::string::npos);

    const auto claude = nlohmann::json::parse(SP::claude_request_json("m", prompt, {"AAAA", "BBBB"}));
    CHECK(claude["model"] == "m");
    CHECK(claude["messages"][0]["content"].size() == 3);
    CHECK(claude["messages"][0]["content"][0]["source"]["data"] == "AAAA");

    const auto ollama = nlohmann::json::parse(SP::ollama_request_json("m", prompt, {"AAAA"}));
    CHECK(ollama["stream"] == false);
    CHECK(ollama["messages"][0]["images"][0] == "AAAA");

    CHECK(SP::claude_reply_text(R"({"content":[{"type":"text","text":"hi "},{"type":"text","text":"there"}]})") == "hi there");
    CHECK(SP::ollama_reply_text(R"({"message":{"role":"assistant","content":"yo"}})") == "yo");
    CHECK(SP::claude_reply_text("not json").empty());
}

TEST_CASE("SemanticPaint: assignment parsing", "[SemanticPaint]")
{
    const std::string reply = "Sure! ```json\n{\"object\":\"a {weird} box\",\"assignments\":[{\"segment\":0,\"filament\":2},"
                              "{\"segment\":\"1\",\"filament\":1},{\"segment\":2,\"filament\":9},{\"segment\":7,\"filament\":1}]}\n```";
    const auto out = SP::parse_assignments(reply, 3, 2);
    REQUIRE(out.has_value());
    CHECK(*out == std::vector<int>{2, 1, 0});
    CHECK_FALSE(SP::parse_assignments("no json here", 3, 2).has_value());
    CHECK_FALSE(SP::parse_assignments("{\"foo\":1}", 3, 2).has_value());

    const TriangleMesh cube = make_cube(1., 1., 1.);
    const auto seg = SP::segment_mesh(cube.its);
    const auto faces = SP::face_filaments(seg, std::vector<int>(seg.segments.size(), 2));
    REQUIRE(faces.size() == cube.its.indices.size());
    for (int f : faces) CHECK(f == 2);
}
