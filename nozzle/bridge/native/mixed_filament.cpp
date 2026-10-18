// See mixed_filament.hpp. Request shape (every op):
//   {"op": "...",
//    "physical": [{"color": "#RRGGBB", "type": "PLA", "name": "optional"}, ...],      // the loaded filaments, tool 1 first
//    "mixed":    [{"components": [1, 3], "weights": [7, 3],                           // 1-based physical filaments, integer weights
//                  "gradient": {"enabled": false, "range": "0.9,0.1", "curve": "", "per_part": false}}, ...]}
// Mixed slot k (0-based) is filament number physical.size() + k + 1 in the slice.
//
//   display          each mixed row with its blended colour, stored texts and problems
//   add              {"components", "weights", "gradient"?}: appends a row (a fixed blend that already exists is reported, not duplicated)
//   update           {"index", "components", "weights", "gradient"?}
//   remove           {"index"}: drops the row; later rows move down one, as do the filament numbers painted with them
//   delete_physical  {"filament"}: 1-based physical filament removed; rows that used it lose that component
//   match            {"target": "#RRGGBB", "mode": "materials" | "cmyw" | "rybw", "material": "PLA"}: the recipe that best matches
//   presets          every pair of physical filaments at 25/50/75, with colours
//   slice_overrides  the per-filament options that make a slice use these rows ("sublayer": true splits layers, false alternates them)
#include "mixed_filament.hpp"

#include <libslic3r/ColorDecomposeRecipe.hpp>
#include <libslic3r/FilamentMixer.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace nozzle_mf {

using namespace Slic3r;
using json = nlohmann::ordered_json;

namespace {

struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };
struct OpFailed : std::runtime_error { using std::runtime_error::runtime_error; };

struct Physical { std::string color = "#808080", type, name; };
struct Gradient { bool enabled = false; std::string range, curve; bool per_part = false; };
struct Row { std::vector<unsigned int> components; std::vector<int> weights; Gradient gradient; };

std::string upper_hex(std::string s) { for (char& c : s) c = char(std::toupper((unsigned char) c)); return s; }

bool valid_hex(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) if (!std::isxdigit((unsigned char) s[i])) return false;
    return true;
}

std::vector<Physical> read_physical(const json& req) {
    std::vector<Physical> out;
    if (!req.contains("physical") || !req["physical"].is_array() || req["physical"].empty())
        throw BadRequest("The request needs a non-empty \"physical\" list.");
    for (const json& p : req["physical"]) {
        Physical f;
        if (p.is_string()) f.color = p.get<std::string>();
        else if (p.is_object()) {
            if (p.contains("color") && p["color"].is_string()) f.color = p["color"].get<std::string>();
            if (p.contains("type") && p["type"].is_string()) f.type = p["type"].get<std::string>();
            if (p.contains("name") && p["name"].is_string()) f.name = p["name"].get<std::string>();
        } else throw BadRequest("Each physical filament is a colour string or an object.");
        if (!valid_hex(f.color)) throw BadRequest("Colours are \"#RRGGBB\": " + f.color);
        f.color = upper_hex(f.color);
        out.push_back(f);
    }
    return out;
}

Row read_row(const json& r, size_t physical_count) {
    Row row;
    if (!r.is_object() || !r.contains("components") || !r["components"].is_array())
        throw BadRequest("A mixed row needs \"components\".");
    for (const json& c : r["components"]) {
        if (!c.is_number_integer()) throw BadRequest("Components are filament numbers.");
        const int v = c.get<int>();
        if (v < 0 || size_t(v) > physical_count) throw BadRequest("Component " + std::to_string(v) + " is not a physical filament.");
        row.components.push_back(unsigned(v));
    }
    if (r.contains("weights") && r["weights"].is_array())
        for (const json& w : r["weights"]) {
            if (!w.is_number_integer() || w.get<int>() < 0) throw BadRequest("Weights are non-negative integers.");
            row.weights.push_back(w.get<int>());
        }
    if (row.weights.empty()) row.weights.assign(row.components.size(), 1);
    if (row.weights.size() != row.components.size()) throw BadRequest("\"weights\" and \"components\" differ in length.");
    if (r.contains("gradient") && r["gradient"].is_object()) {
        const json& g = r["gradient"];
        row.gradient.enabled = g.value("enabled", false);
        row.gradient.range = g.value("range", std::string());
        row.gradient.curve = g.value("curve", std::string());
        row.gradient.per_part = g.value("per_part", false);
    }
    return row;
}

std::vector<Row> read_mixed(const json& req, size_t physical_count) {
    std::vector<Row> rows;
    if (req.contains("mixed") && req["mixed"].is_array())
        for (const json& r : req["mixed"]) rows.push_back(read_row(r, physical_count));
    return rows;
}

json row_json(const Row& row) {
    json comps = json::array(), weights = json::array();
    for (unsigned c : row.components) comps.push_back(c);
    for (int w : row.weights) weights.push_back(w);
    return json{{"components", comps}, {"weights", weights},
                {"gradient", {{"enabled", row.gradient.enabled}, {"range", row.gradient.range}, {"curve", row.gradient.curve},
                              {"per_part", row.gradient.per_part}}}};
}

// The colour a row prints as: its components blended by weight (gradient rows show the starting mix).
std::string blended_color(const Row& row, const std::vector<Physical>& physical) {
    std::vector<std::string> colors;
    std::vector<int> weights;
    for (size_t i = 0; i < row.components.size(); ++i) {
        const unsigned c = row.components[i];
        if (c == 0 || c > physical.size() || row.weights[i] <= 0) continue;
        colors.push_back(physical[c - 1].color);
        weights.push_back(row.weights[i]);
    }
    if (colors.empty()) return "#808080";
    if (colors.size() == 1) return colors.front();
    return upper_hex(blend_color_multi(colors, weights));
}

// Problems the slicer would reject or warn about, in plain words.
std::vector<std::string> row_problems(const Row& row, const std::vector<Physical>& physical) {
    std::vector<std::string> problems;
    std::set<unsigned> distinct;
    for (unsigned c : row.components) if (c) distinct.insert(c);
    for (unsigned c : row.components)
        if (c == 0) problems.push_back("A component filament was removed.");
    if (distinct.size() < 2) problems.push_back("A mix needs at least two different filaments.");
    int positive = 0;
    for (int w : row.weights) if (w > 0) ++positive;
    if (positive < 2) problems.push_back("At least two components need a weight above zero.");
    std::set<std::string> types;
    for (unsigned c : distinct) if (c <= physical.size() && !physical[c - 1].type.empty()) types.insert(physical[c - 1].type);
    if (types.size() > 1) problems.push_back("The components are different materials, which print at different temperatures.");
    if (row.gradient.enabled && row.components.size() != 2) problems.push_back("A gradient blends exactly two filaments.");
    return problems;
}

json describe(const std::vector<Physical>& physical, const std::vector<Row>& rows) {
    json out = json::array();
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        json j = row_json(r);
        j["filament"] = physical.size() + i + 1;
        j["color"] = blended_color(r, physical);
        j["components_text"] = format_mixed_components(r.components);
        j["ratios_text"] = format_mixed_ratios(r.weights);
        json problems = json::array();
        for (const std::string& p : row_problems(r, physical)) problems.push_back(p);
        j["problems"] = problems;
        out.push_back(j);
    }
    return out;
}

// ConfigBase view of the rows, for find_fixed_mixed_filament().
int find_existing(const std::vector<Row>& rows, const Row& wanted) {
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].gradient.enabled || wanted.gradient.enabled) continue;
        if (format_mixed_components(rows[i].components) == format_mixed_components(wanted.components) &&
            format_mixed_ratios(rows[i].weights) == format_mixed_ratios(wanted.weights))
            return int(i);
    }
    return -1;
}

double delta_e(const std::string& a, const std::string& b) {
    ColorDecomposeRgb ra, rb;
    if (!color_decompose_hex_to_rgb(a, ra) || !color_decompose_hex_to_rgb(b, rb)) return 0.;
    auto lab = [](const ColorDecomposeRgb& c, double out[3]) {
        auto lin = [](double v) { v /= 255.; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        const double r = lin(c.r), g = lin(c.g), bl = lin(c.b);
        double x = (0.4124 * r + 0.3576 * g + 0.1805 * bl) / 0.95047, y = 0.2126 * r + 0.7152 * g + 0.0722 * bl, z = (0.0193 * r + 0.1192 * g + 0.9505 * bl) / 1.08883;
        auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16. / 116.; };
        out[0] = 116. * f(y) - 16.; out[1] = 500. * (f(x) - f(y)); out[2] = 200. * (f(y) - f(z));
    };
    double la[3], lb[3];
    lab(ra, la); lab(rb, lb);
    return std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2]));
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
    return s;
}

json op_display(const json& req) {
    const auto physical = read_physical(req);
    const auto rows = read_mixed(req, physical.size());
    return json{{"mixed", describe(physical, rows)}};
}

json op_add(const json& req) {
    const auto physical = read_physical(req);
    auto rows = read_mixed(req, physical.size());
    const Row row = read_row(req, physical.size());
    const auto problems = row_problems(row, physical);
    if (!problems.empty() && std::any_of(problems.begin(), problems.end(), [](const std::string& p) { return p.find("at least two") != std::string::npos || p.find("removed") != std::string::npos; }))
        throw OpFailed(problems.front());
    const int existing = find_existing(rows, row);
    json out;
    if (existing >= 0) {
        out["existing"] = existing;
        out["added"] = false;
    } else {
        rows.push_back(row);
        out["added"] = true;
        out["index"] = rows.size() - 1;
        out["filament"] = physical.size() + rows.size();
    }
    out["mixed"] = describe(physical, rows);
    return out;
}

json op_update(const json& req) {
    const auto physical = read_physical(req);
    auto rows = read_mixed(req, physical.size());
    if (!req.contains("index") || !req["index"].is_number_integer()) throw BadRequest("\"index\" is required.");
    const int index = req["index"].get<int>();
    if (index < 0 || size_t(index) >= rows.size()) throw BadRequest("No mixed row " + std::to_string(index));
    rows[size_t(index)] = read_row(req, physical.size());
    return json{{"mixed", describe(physical, rows)}};
}

json op_remove(const json& req) {
    const auto physical = read_physical(req);
    auto rows = read_mixed(req, physical.size());
    if (!req.contains("index") || !req["index"].is_number_integer()) throw BadRequest("\"index\" is required.");
    const int index = req["index"].get<int>();
    if (index < 0 || size_t(index) >= rows.size()) throw BadRequest("No mixed row " + std::to_string(index));
    rows.erase(rows.begin() + index);
    // Filament numbers above the removed row shift down by one wherever they are painted or assigned.
    return json{{"removed_filament", physical.size() + size_t(index) + 1}, {"mixed", describe(physical, rows)}};
}

json op_delete_physical(const json& req) {
    auto physical = read_physical(req);
    auto rows = read_mixed(req, physical.size());
    if (!req.contains("filament") || !req["filament"].is_number_integer()) throw BadRequest("\"filament\" is required.");
    const int deleted = req["filament"].get<int>();
    if (deleted < 1 || size_t(deleted) > physical.size()) throw BadRequest("No physical filament " + std::to_string(deleted));
    for (Row& row : rows)
        for (unsigned& c : row.components) {
            if (c == unsigned(deleted)) c = 0;       // the same sentinel Orca uses for a removed component
            else if (c > unsigned(deleted)) --c;
        }
    physical.erase(physical.begin() + (deleted - 1));
    json phys = json::array();
    for (const Physical& p : physical) phys.push_back({{"color", p.color}, {"type", p.type}, {"name", p.name}});
    return json{{"physical", phys}, {"mixed", describe(physical, rows)}};
}

json op_match(const json& req) {
    const auto physical = read_physical(req);
    if (!req.contains("target") || !req["target"].is_string() || !valid_hex(req["target"].get<std::string>()))
        throw BadRequest("\"target\" is a \"#RRGGBB\" colour.");
    const std::string target = upper_hex(req["target"].get<std::string>());
    const std::string mode = req.value("mode", std::string("materials"));
    const std::string material = req.value("material", std::string());
    ColorDecomposeRgb rgb;
    color_decompose_hex_to_rgb(target, rgb);
    ColorDecomposeRecipeResult result;
    if (mode == "materials") {
        std::vector<ColorDecomposePhysicalFilament> list;
        for (size_t i = 0; i < physical.size(); ++i) {
            ColorDecomposePhysicalFilament f;
            f.color_hex = physical[i].color; f.name = physical[i].name; f.type = physical[i].type; f.filament_index = unsigned(i + 1);
            list.push_back(f);
        }
        result = recommend_from_physical_filaments(rgb, list, material);
    } else if (mode == "cmyw" || mode == "rybw") {
        result = lookup_standard_recipe(rgb, mode == "cmyw" ? ColorDecomposeRecipeMode::CMYW : ColorDecomposeRecipeMode::RYBW, material);
    } else throw BadRequest("\"mode\" is materials, cmyw or rybw.");
    if (!result.valid || result.components.empty()) throw OpFailed("No recipe matches " + target + " with these filaments.");
    json comps = json::array();
    Row row;
    for (const auto& c : result.components) {
        comps.push_back({{"filament", c.filament_index}, {"base_color", c.base_color}, {"color", c.color_hex}, {"ratio", c.ratio}});
        if (c.filament_index > 0) { row.components.push_back(c.filament_index); row.weights.push_back(c.ratio); }
    }
    json out{{"target", target}, {"matched_color", upper_hex(result.matched_color_hex)},
             {"delta_e", delta_e(target, result.matched_color_hex)}, {"components", comps}};
    if (row.components.size() >= 2) { out["row"] = row_json(row); out["row_color"] = blended_color(row, physical); }
    return out;
}

json op_presets(const json& req) {
    const auto physical = read_physical(req);
    json out = json::array();
    for (size_t a = 0; a < physical.size(); ++a)
        for (size_t b = a + 1; b < physical.size(); ++b)
            for (int pct : {25, 50, 75}) {
                Row r;
                r.components = {unsigned(a + 1), unsigned(b + 1)};
                r.weights = {100 - pct, pct};
                json j = row_json(r);
                j["color"] = blended_color(r, physical);
                out.push_back(j);
            }
    return json{{"presets", out}};
}

json op_slice_overrides(const json& req) {
    const auto physical = read_physical(req);
    const auto rows = read_mixed(req, physical.size());
    const bool sublayer = req.value("sublayer", true);
    std::vector<std::string> colours, is_mixed, comps, ratios, grad, range, curve, per_part;
    for (const Physical& p : physical) {
        colours.push_back(p.color); is_mixed.push_back("0"); comps.push_back(""); ratios.push_back("");
        grad.push_back("0"); range.push_back(""); curve.push_back(""); per_part.push_back("0");
    }
    bool any_gradient = false;
    for (const Row& r : rows) {
        for (const std::string& p : row_problems(r, physical))
            if (p.find("at least two") != std::string::npos || p.find("removed") != std::string::npos || p.find("exactly two") != std::string::npos)
                throw OpFailed("A mixed row cannot be sliced: " + p);
        colours.push_back(blended_color(r, physical));
        is_mixed.push_back("1");
        comps.push_back(format_mixed_components(r.components));
        ratios.push_back(format_mixed_ratios(r.weights));
        grad.push_back(r.gradient.enabled ? "1" : "0");
        range.push_back(r.gradient.range);
        curve.push_back(r.gradient.curve);
        per_part.push_back(r.gradient.per_part ? "1" : "0");
        any_gradient = any_gradient || r.gradient.enabled;
    }
    // A gradient moves the ratio from layer to layer, which only the sub-layer path can do.
    const bool use_sublayer = sublayer || any_gradient;
    // Bools join with ',' and strings with ';' in a config value; filament colours are strings.
    json o = {{"filament_colour", join(colours, ';')}, {"filament_is_mixed", join(is_mixed, ',')},
              {"filament_mixed_components", join(comps, ';')}, {"filament_mixed_sublayer_ratios", join(ratios, ';')},
              {"filament_mixed_gradient", join(grad, ',')}, {"filament_mixed_gradient_range", join(range, ';')},
              {"filament_mixed_gradient_curve", join(curve, ';')}, {"filament_mixed_gradient_per_part", join(per_part, ',')},
              {"enable_mixed_color_sublayer", use_sublayer ? "1" : "0"}};
    return json{{"overrides", o}, {"slots", physical.size() + rows.size()}};
}

} // namespace

int run_mixed_filament(const std::string& request_text, std::string& response) {
    try {
        const json req = json::parse(request_text);
        if (!req.is_object() || !req.contains("op") || !req["op"].is_string()) throw BadRequest("The request needs an \"op\".");
        const std::string op = req["op"].get<std::string>();
        json out;
        if (op == "display") out = op_display(req);
        else if (op == "add") out = op_add(req);
        else if (op == "update") out = op_update(req);
        else if (op == "remove") out = op_remove(req);
        else if (op == "delete_physical") out = op_delete_physical(req);
        else if (op == "match") out = op_match(req);
        else if (op == "presets") out = op_presets(req);
        else if (op == "slice_overrides") out = op_slice_overrides(req);
        else throw BadRequest("Unknown op \"" + op + "\".");
        response = out.dump();
        return 0;
    } catch (const BadRequest& e) {
        response = json{{"error", e.what()}}.dump();
        return 2;
    } catch (const json::exception& e) {
        response = json{{"error", std::string("Bad request: ") + e.what()}}.dump();
        return 2;
    } catch (const std::exception& e) {
        response = json{{"error", e.what()}}.dump();
        return 1;
    }
}

} // namespace nozzle_mf
