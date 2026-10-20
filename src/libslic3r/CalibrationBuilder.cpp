///|/ TigerSlicer calibration builder: one tool for every calibration print.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CalibrationBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace calib_builder {

const std::vector<SettingInfo> &settings()
{
    // A first sweep that is useful on most printers. The pressure advance range suits direct drive;
    // Bowden printers need more, which the dialog leaves to the user.
    static const std::vector<SettingInfo> table = {
        { Setting::Temperature,           "temperature",             "Nozzle temperature",      "°C", 0, 230., 190., 5.,     150., 350., false },
        { Setting::PressureAdvance,       "pressure_advance",        "Pressure advance",        "",        3, 0.,   0.08, 0.005,  0.,   2.,   false },
        { Setting::RetractionLength,      "retraction_length",       "Retraction length",       "mm",      1, 0.2,  2.,   0.2,    0.,   10.,  false },
        { Setting::RetractionSpeed,       "retraction_speed",        "Retraction speed",        "mm/s",    0, 20.,  60.,  10.,    1.,   200., false },
        { Setting::FlowRatio,             "flow_ratio",              "Flow ratio",              "",        2, 0.9,  1.1,  0.02,   0.5,  1.5,  true  },
        { Setting::PrintSpeed,            "print_speed",             "Print speed",             "mm/s",    0, 50.,  250., 50.,    5.,   1000.,true  },
        { Setting::Acceleration,          "acceleration",            "Acceleration",            "mm/s²", 0, 2000., 12000., 2000., 100., 50000., true },
        // The lowest values are ones the printer acts on: the G-code writer sends no jerk below 0.01 and
        // no input shaping frequency or damping of 0.
        { Setting::Jerk,                  "jerk",                    "Jerk (square corner velocity)", "mm/s", 1, 1., 15., 2., 0.1, 100., false },
        { Setting::JunctionDeviation,     "junction_deviation",      "Junction deviation",      "mm",      3, 0.02, 0.2,  0.02,   0.001, 1.,  false },
        { Setting::InputShapingFrequency, "input_shaping_frequency", "Input shaping frequency", "Hz",      1, 15.,  110., 5.,     1.,   300., false },
        { Setting::InputShapingDamping,   "input_shaping_damping",   "Input shaping damping",   "",        2, 0.05, 0.5,  0.05,   0.01, 1.,   false },
    };
    return table;
}

const SettingInfo &info(Setting setting)
{
    for (const SettingInfo &s : settings())
        if (s.setting == setting)
            return s;
    return settings().front();
}

bool parse_setting(const std::string &id, Setting &setting)
{
    for (const SettingInfo &s : settings())
        if (id == s.id) {
            setting = s.setting;
            return true;
        }
    return false;
}

std::vector<double> sweep(double from, double to, double step, std::string *error)
{
    auto fail = [error](const std::string &why) {
        if (error != nullptr)
            *error = why;
        return std::vector<double>();
    };
    if (!(step > 0.) || !std::isfinite(from) || !std::isfinite(to))
        return fail("The step must be more than zero.");
    // Rounding must not drop an end that lies on a step: 0.2 / 0.05 is 3.99999...
    // Counted as a double first: a tiny step over a huge range does not fit a size_t.
    const double steps = std::floor(std::abs(to - from) / step + 1e-6);
    if (!(steps < double(kMaxValues)))
        return fail("That is more than " + std::to_string(kMaxValues) + " values. Use a bigger step or a smaller range.");
    const size_t count = size_t(steps) + 1;
    const double direction = to >= from ? 1. : -1.;
    std::vector<double> values;
    values.reserve(count);
    for (size_t i = 0; i < count; ++i)
        // Rounded to 1e-9 so 0.1 + 0.2 prints and compares as 0.3.
        values.push_back(std::round((from + direction * double(i) * step) * 1e9) / 1e9);
    return values;
}

std::string format_value(Setting setting, double value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", info(setting).decimals, value);
    std::string text = buf;
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (text.back() == '.')
            text.pop_back();
    }
    if (text == "-0")
        text = "0";
    return text;
}

bool fits_decimals(Setting setting, double value)
{
    const double scaled = value * std::pow(10., info(setting).decimals);
    return std::isfinite(scaled) && std::abs(scaled - std::round(scaled)) < 1e-6;
}

size_t band_at(double z, double band_height, size_t band_count)
{
    if (band_count == 0 || !(band_height > 0.) || z <= 0.)
        return 0;
    return std::min(size_t(std::floor(z / band_height)), band_count - 1);
}

std::string to_json(const Setup &setup)
{
    nlohmann::json j;
    j["setting"]      = info(setup.setting).id;
    j["from"]         = setup.from;
    j["to"]           = setup.to;
    j["step"]         = setup.step;
    j["layout"]       = setup.layout == Layout::Grid ? "grid" : "tower";
    j["band_height"]  = setup.band_height;
    j["model"]        = setup.model_path;
    j["layer_height"] = setup.layer_height;
    j["print_speed"]  = setup.print_speed;
    return j.dump();
}

bool from_json(const std::string &json, Setup &setup)
{
    try {
        const nlohmann::json j = nlohmann::json::parse(json);
        Setup out;
        if (!j.contains("setting") || !parse_setting(j.at("setting").get<std::string>(), out.setting))
            return false;
        out.from         = j.value("from", info(out.setting).from);
        out.to           = j.value("to", info(out.setting).to);
        out.step         = j.value("step", info(out.setting).step);
        out.layout       = j.value("layout", std::string("tower")) == "grid" ? Layout::Grid : Layout::Tower;
        out.band_height  = j.value("band_height", 5.);
        out.model_path   = j.value("model", std::string());
        out.layer_height = j.value("layer_height", 0.);
        out.print_speed  = j.value("print_speed", 0.);
        setup            = out;
        return true;
    } catch (...) {
        return false;
    }
}

// --- Geometry ---------------------------------------------------------------------------------

namespace {

// An axis-aligned box from (x0, y0, z0) to (x1, y1, z1).
indexed_triangle_set box(double x0, double y0, double z0, double x1, double y1, double z1)
{
    indexed_triangle_set its = its_make_cube(x1 - x0, y1 - y0, z1 - z0);
    for (stl_vertex &v : its.vertices)
        v += stl_vertex(float(x0), float(y0), float(z0));
    return its;
}

struct Rect
{
    double u0, v0, u1, v1;
};

// Seven segments, a (top) to g (middle), in a cell w wide and h tall with strokes t thick.
std::array<Rect, 7> segments(double w, double h, double t)
{
    return { Rect { 0., h - t, w, h },          // a
             Rect { w - t, h / 2., w, h },      // b
             Rect { w - t, 0., w, h / 2. },     // c
             Rect { 0., 0., w, t },             // d
             Rect { 0., 0., t, h / 2. },        // e
             Rect { 0., h / 2., t, h },         // f
             Rect { 0., (h - t) / 2., w, (h + t) / 2. } }; // g
}

// The segments lit for each digit, as a bit mask with a = bit 0 ... g = bit 6.
constexpr std::array<unsigned, 10> kDigitSegments = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

} // namespace

indexed_triangle_set label_mesh(const std::string &text, double digit_height, double depth, double *width)
{
    const double h   = digit_height;
    const double w   = 0.6 * h;
    // Strokes at least one extrusion wide, so a small label still prints.
    const double t   = std::max(0.16 * h, 0.45);
    const double gap = 0.25 * h;
    indexed_triangle_set out;
    double x = 0., right = 0.;
    for (char c : text) {
        std::vector<Rect> rects;
        double            advance = 0.;
        if (c >= '0' && c <= '9') {
            const std::array<Rect, 7> segs = segments(w, h, t);
            for (int s = 0; s < 7; ++s)
                if (kDigitSegments[size_t(c - '0')] & (1u << s))
                    rects.push_back(segs[size_t(s)]);
            advance = w;
        } else if (c == '.') {
            rects.push_back({ 0., 0., t, t });
            advance = t;
        } else if (c == '-') {
            rects.push_back(segments(w, h, t)[6]);
            advance = w;
        } else
            continue; // nothing to draw it with
        for (const Rect &r : rects)
            its_merge(out, box(x + r.u0, r.v0, 0., x + r.u1, r.v1, depth));
        right = x + advance;
        x += advance + gap;
    }
    if (width != nullptr)
        *width = right;
    return out;
}

namespace {

constexpr double kTowerSide   = 20.;
constexpr double kGrooveInset = 0.6;
constexpr double kLabelRaise  = 0.6; // how far the digits stand out
constexpr double kLabelEmbed  = 0.1; // how far they reach into the face, so they join it
constexpr double kTileSide    = 22.;
constexpr double kTileHeight  = 4.;

// A label that fits `max_width`, as tall as `max_height` allows.
indexed_triangle_set fitted_label(const std::string &text, double max_height, double max_width, double *width)
{
    double h = max_height, w = 0.;
    indexed_triangle_set label = label_mesh(text, h, kLabelRaise + kLabelEmbed, &w);
    if (w > max_width && w > 0.) {
        h *= max_width / w;
        label = label_mesh(text, h, kLabelRaise + kLabelEmbed, &w);
    }
    if (width != nullptr)
        *width = w;
    return label;
}

} // namespace

TriangleMesh make_tower(Setting setting, const std::vector<double> &values, double band_height)
{
    indexed_triangle_set tower;
    const double groove = std::min(0.6, 0.15 * band_height);
    for (size_t k = 0; k < values.size(); ++k) {
        const double bottom = double(k) * band_height;
        const double top    = bottom + band_height;
        const bool   last   = k + 1 == values.size();
        // A narrower neck between bands marks where one value ends.
        const double body_top = last ? top : top - groove;
        its_merge(tower, box(0., 0., bottom, kTowerSide, kTowerSide, body_top));
        if (!last)
            its_merge(tower, box(kGrooveInset, kGrooveInset, body_top, kTowerSide - kGrooveInset, kTowerSide - kGrooveInset, top));
        // The value on the front (-Y) face, centred on the band.
        const double body = body_top - bottom;
        double       w    = 0.;
        indexed_triangle_set label = fitted_label(format_value(setting, values[k]), std::min(0.6 * body, 6.), kTowerSide - 2., &w);
        const double x0 = (kTowerSide - w) / 2.;
        double       z0 = bottom + body / 2.;
        if (!label.vertices.empty()) {
            const BoundingBoxf3 lb = bounding_box(label);
            z0 -= (lb.max.y() - lb.min.y()) / 2.;
        }
        // Text plane (u, v, depth) to the face: x = u, z = v, y = embed - depth (a proper rotation).
        for (stl_vertex &v : label.vertices)
            v = stl_vertex(float(x0 + v.x()), float(kLabelEmbed - v.z()), float(z0 + v.y()));
        its_merge(tower, label);
    }
    TriangleMesh mesh(std::move(tower));
    return mesh;
}

TriangleMesh make_tile(Setting setting, double value)
{
    indexed_triangle_set tile = box(0., 0., 0., kTileSide, kTileSide, kTileHeight);
    double               w    = 0.;
    indexed_triangle_set label = fitted_label(format_value(setting, value), 4., kTileSide - 4., &w);
    const double x0 = (kTileSide - w) / 2.;
    for (stl_vertex &v : label.vertices)
        v = stl_vertex(float(x0 + v.x()), float(2. + v.y()), float(kTileHeight - kLabelEmbed + v.z()));
    its_merge(tile, label);
    return TriangleMesh(std::move(tile));
}

// --- Configuration ----------------------------------------------------------------------------

DynamicPrintConfig object_overrides(Setting setting, double value)
{
    DynamicPrintConfig cfg;
    switch (setting) {
    case Setting::FlowRatio:
        cfg.set_key_value("print_flow_ratio", new ConfigOptionFloat(value));
        break;
    case Setting::PrintSpeed:
        for (const char *key : { "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed" })
            cfg.set_key_value(key, new ConfigOptionFloatsNullable({ value }));
        break;
    case Setting::Acceleration:
        // default_acceleration too: with it at 0 no acceleration is emitted at all.
        for (const char *key : { "default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration", "top_surface_acceleration" })
            cfg.set_key_value(key, new ConfigOptionFloatsNullable({ value }));
        for (const char *key : { "sparse_infill_acceleration", "internal_solid_infill_acceleration" })
            cfg.set_key_value(key, new ConfigOptionFloatsOrPercentsNullable({ FloatOrPercent(value, false) }));
        break;
    default: break;
    }
    return cfg;
}

bool uses_junction_deviation(const DynamicPrintConfig &printer)
{
    const auto *flavor = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    const auto *jd     = printer.option<ConfigOptionFloats>("machine_max_junction_deviation");
    return flavor != nullptr && flavor->value == gcfMarlinFirmware && jd != nullptr && !jd->values.empty() && jd->values.front() > 0.;
}

std::string printer_problem(Setting setting, const DynamicPrintConfig &printer)
{
    switch (setting) {
    case Setting::Jerk:
        if (uses_junction_deviation(printer))
            return "This printer uses junction deviation, not jerk. Test junction deviation instead.";
        break;
    case Setting::JunctionDeviation:
        if (!uses_junction_deviation(printer))
            return "Junction deviation needs Marlin 2 with a maximum junction deviation above 0 in the printer's motion "
                   "ability. Test jerk instead.";
        break;
    case Setting::InputShapingFrequency:
    case Setting::InputShapingDamping: {
        const auto *flavor = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
        if (flavor == nullptr || (flavor->value != gcfKlipper && flavor->value != gcfRepRapFirmware && flavor->value != gcfMarlinFirmware))
            return "Input shaping can only be set from G-code on Klipper, RepRapFirmware and Marlin 2.";
        break;
    }
    default: break;
    }
    return {};
}

namespace {

// Sets every entry of a vector option (at least one), creating the option if the profile lacks it.
template<class Opt, class T> void set_all(DynamicPrintConfig &cfg, const std::string &key, const T &value)
{
    Opt *opt = cfg.option<Opt>(key, true);
    if (opt == nullptr)
        return;
    if (opt->values.empty())
        opt->values.push_back(value);
    for (auto &v : opt->values)
        v = value;
}

// Raises a vector machine limit to at least `value`.
void raise_limit(DynamicPrintConfig &cfg, const std::string &key, double value)
{
    ConfigOptionFloats *opt = cfg.option<ConfigOptionFloats>(key, true);
    if (opt == nullptr)
        return;
    if (opt->values.empty())
        opt->values.push_back(value);
    for (double &v : opt->values)
        v = std::max(v, value);
}

double first_or(const DynamicPrintConfig &cfg, const std::string &key, double fallback)
{
    const ConfigOptionFloats *opt = cfg.option<ConfigOptionFloats>(key);
    return opt != nullptr && !opt->values.empty() ? opt->values.front() : fallback;
}

void no_cooling_slowdown(DynamicPrintConfig &filament)
{
    set_all<ConfigOptionBools>(filament, "slow_down_for_layer_cooling", (unsigned char) false);
    set_all<ConfigOptionFloats>(filament, "slow_down_layer_time", 0.);
    set_all<ConfigOptionFloats>(filament, "slow_down_min_speed", 0.);
}

void pressure_advance_fixed(DynamicPrintConfig &filament)
{
    const ConfigOptionBools *on = filament.option<ConfigOptionBools>("enable_pressure_advance");
    if (on == nullptr || on->values.empty() || !on->values.front()) {
        set_all<ConfigOptionBools>(filament, "enable_pressure_advance", (unsigned char) true);
        set_all<ConfigOptionFloats>(filament, "pressure_advance", 0.);
    }
    set_all<ConfigOptionBools>(filament, "adaptive_pressure_advance", (unsigned char) false);
}

// A single-wall tower printed as fast as the machine allows, so ringing and corners show
// (what the old input shaping and cornering tests printed).
void ringing_tower(DynamicPrintConfig &print, DynamicPrintConfig &filament, DynamicPrintConfig &printer)
{
    no_cooling_slowdown(filament);
    pressure_advance_fixed(filament);
    set_all<ConfigOptionBools>(print, "enable_overhang_speed", (unsigned char) false);
    print.set_key_value("wall_loops", new ConfigOptionInt(1));
    print.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print.set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print.set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print.set_key_value("spiral_mode", new ConfigOptionBool(true));
    print.set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    const double max_speed = std::min(first_or(printer, "machine_max_speed_x", 200.), first_or(printer, "machine_max_speed_y", 200.));
    const double max_accel = first_or(printer, "machine_max_acceleration_extruding", 5000.);
    set_all<ConfigOptionFloatsNullable>(print, "outer_wall_speed", max_speed);
    set_all<ConfigOptionFloatsNullable>(print, "default_acceleration", max_accel);
    set_all<ConfigOptionFloatsNullable>(print, "outer_wall_acceleration", max_accel);
}

// Turns per-feature jerk (or junction deviation) off and lifts the machine limit to `largest`,
// so the value the test sets with a command is the one the printer uses.
void cornering_from_commands(DynamicPrintConfig &print, DynamicPrintConfig &printer, double largest, bool junction_deviation)
{
    if (junction_deviation) {
        raise_limit(printer, "machine_max_junction_deviation", largest);
        set_all<ConfigOptionFloatsNullable>(print, "default_junction_deviation", 0.);
    } else {
        raise_limit(printer, "machine_max_jerk_x", largest);
        raise_limit(printer, "machine_max_jerk_y", largest);
        set_all<ConfigOptionFloatsNullable>(print, "default_jerk", 0.);
    }
}

} // namespace

void prepare_profiles(Setting setting, double first, double largest, DynamicPrintConfig &print, DynamicPrintConfig &filament,
                      DynamicPrintConfig &printer)
{
    // Nothing that reshapes the print or the motion under the test.
    printer.set_key_value("resonance_avoidance", new ConfigOptionBool(false));
    print.set_key_value("precise_z_height", new ConfigOptionBool(false));
    print.set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    switch (setting) {
    case Setting::Temperature: {
        const int t = int(std::lround(first));
        set_all<ConfigOptionInts>(filament, "nozzle_temperature", t);
        set_all<ConfigOptionInts>(filament, "nozzle_temperature_initial_layer", t);
        break;
    }
    case Setting::PressureAdvance:
        set_all<ConfigOptionBools>(filament, "enable_pressure_advance", (unsigned char) true);
        set_all<ConfigOptionFloats>(filament, "pressure_advance", first);
        set_all<ConfigOptionBools>(filament, "adaptive_pressure_advance", (unsigned char) false);
        break;
    case Setting::RetractionLength:
        printer.set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
        set_all<ConfigOptionFloats>(printer, "retraction_length", first);
        break;
    case Setting::RetractionSpeed:
        printer.set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
        set_all<ConfigOptionFloats>(printer, "retraction_speed", first);
        break;
    case Setting::FlowRatio: break;
    case Setting::PrintSpeed:
        no_cooling_slowdown(filament);
        set_all<ConfigOptionBools>(print, "enable_overhang_speed", (unsigned char) false);
        // The test shows what happens at the speed asked for, not at the volumetric limit.
        raise_limit(filament, "filament_max_volumetric_speed", 200.);
        raise_limit(printer, "machine_max_speed_x", largest);
        raise_limit(printer, "machine_max_speed_y", largest);
        break;
    case Setting::Acceleration:
        raise_limit(printer, "machine_max_acceleration_extruding", largest);
        raise_limit(printer, "machine_max_acceleration_x", largest);
        raise_limit(printer, "machine_max_acceleration_y", largest);
        break;
    case Setting::Jerk:
    case Setting::JunctionDeviation:
        ringing_tower(print, filament, printer);
        cornering_from_commands(print, printer, largest, setting == Setting::JunctionDeviation);
        break;
    case Setting::InputShapingFrequency:
    case Setting::InputShapingDamping: {
        ringing_tower(print, filament, printer);
        // Some cornering so the input shaper has something to smooth (the old tests' values).
        const auto *flavor = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
        const double jerk  = flavor != nullptr && flavor->value == gcfKlipper ? 5. : 10.;
        const bool   jd    = uses_junction_deviation(printer);
        cornering_from_commands(print, printer, jd ? 0.25 : jerk, jd);
        printer.set_key_value("input_shaping_emit", new ConfigOptionBool(false));
        break;
    }
    }
}

} // namespace calib_builder
} // namespace Slic3r
