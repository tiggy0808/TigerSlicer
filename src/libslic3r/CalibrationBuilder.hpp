///|/ TigerSlicer calibration builder: one tool for every calibration print.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CalibrationBuilder_hpp_
#define slic3r_CalibrationBuilder_hpp_

#include <cstddef>
#include <string>
#include <vector>

#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {
namespace calib_builder {

// A setting the builder can sweep. Presets store the id; new settings go at the end, and settings() sets
// the order of the dialog's list.
enum class Setting : int {
    Temperature = 0,
    PressureAdvance,
    RetractionLength,
    RetractionSpeed,
    FlowRatio,
    PrintSpeed,
    Acceleration,
    Jerk,
    InputShapingFrequency,
    InputShapingDamping,
    JunctionDeviation,
};

// Tower: one model whose value changes every band_height mm of height.
// Grid: one copy of the model per value, each with its own override.
enum class Layout : int { Tower = 0, Grid };

struct SettingInfo
{
    Setting     setting;
    const char *id;       // stable name, used by presets
    const char *label;
    const char *unit;     // empty when the value has no unit
    int         decimals; // digits after the point in labels and in the dialog
    double      from, to, step; // a sensible first sweep
    double      min, max; // accepted values
    bool        grid;     // can also be a grid of copies (one value per copy)
};

const std::vector<SettingInfo> &settings();
const SettingInfo              &info(Setting setting);
bool                            parse_setting(const std::string &id, Setting &setting);

// More values than this make a tower too tall to be useful and a grid too big for most beds.
constexpr size_t kMaxValues = 40;
// Lower bands leave no room between the strokes of their digits.
constexpr double kMinBandHeight = 4.;
constexpr double kMaxBandHeight = 30.;

// True when `value` needs no more digits after the point than the setting shows, so the value printed
// on the model is the value used.
bool fits_decimals(Setting setting, double value);

// Why this printer cannot run a test of `setting`, or empty when it can. Jerk and junction deviation are
// exclusive (Marlin 2 with junction deviation on ignores jerk), and only some firmware takes input
// shaping from G-code.
std::string printer_problem(Setting setting, const DynamicPrintConfig &printer);

// The values from `from` to `to`, `step` apart; `step` is a distance, so the sweep can also run down.
// `to` is included when it lies on the step (within rounding). On an invalid sweep (step not above
// zero, or more than kMaxValues values) returns nothing and says why in `error`.
std::vector<double> sweep(double from, double to, double step, std::string *error = nullptr);

// The value as printed on the model and shown in the dialog: the setting's decimals, no trailing zeros.
std::string format_value(Setting setting, double value);

// The band a height falls in: bands band_height tall from z = 0, clamped to the last one.
size_t band_at(double z, double band_height, size_t band_count);

// What the dialog builds, and what a preset stores.
struct Setup
{
    Setting     setting { Setting::Temperature };
    double      from { 0. }, to { 0. }, step { 0. };
    Layout      layout { Layout::Tower };
    double      band_height { 5. };
    std::string model_path;          // empty: the built-in tower or tile
    double      layer_height { 0. }; // 0: keep the print profile's
    double      print_speed { 0. };  // 0: keep the print profile's (unused when PrintSpeed is the setting)
};
std::string to_json(const Setup &setup);
bool        from_json(const std::string &json, Setup &setup);

// Raised seven-segment digits for `text` (digits, '.', '-'): lying in the XY plane, the text running
// along +X and reading up +Y, `digit_height` tall and `depth` thick from z = 0. Returns the width too.
indexed_triangle_set label_mesh(const std::string &text, double digit_height, double depth, double *width = nullptr);

// The built-in tower: a 20 x 20 mm block per value, band_height tall, a shallow groove between bands,
// and each band's value in raised digits on its front (-Y) face.
TriangleMesh make_tower(Setting setting, const std::vector<double> &values, double band_height);
// The built-in grid tile: 22 x 22 x 4 mm with its value in raised digits along the front of the top.
TriangleMesh make_tile(Setting setting, double value);

// The per-object overrides that set `value` on one copy of a grid. Empty for a setting that has no
// grid (it is changed by printer commands while printing, so it cannot differ between copies).
DynamicPrintConfig object_overrides(Setting setting, double value);

// Prepares the print, filament and printer profiles for a sweep that starts at `first` and reaches
// `largest`: the tested setting starts at `first`, and whatever would hide its effect is turned off
// (cooling slowdowns, adaptive pressure advance, per-feature jerk, machine limits below the sweep).
void prepare_profiles(Setting setting, double first, double largest, DynamicPrintConfig &print, DynamicPrintConfig &filament,
                      DynamicPrintConfig &printer);

// True for printers that limit cornering with junction deviation instead of jerk.
bool uses_junction_deviation(const DynamicPrintConfig &printer);

} // namespace calib_builder
} // namespace Slic3r

#endif // slic3r_CalibrationBuilder_hpp_
