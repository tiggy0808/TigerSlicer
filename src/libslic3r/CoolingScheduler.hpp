///|/ TigerSlicer Phase 18: thermal and cooling-aware scheduling.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CoolingScheduler_hpp_
#define slic3r_CoolingScheduler_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/TelemetryImport.hpp" // Confidence

namespace Slic3r {

// This is a bounded, documented approximation. It is NOT computational fluid
// dynamics and must never be presented as one: it sums a small number of
// declared contributions and clamps the result. Every estimate carries the
// disclaimer with it so no caller can strip it off by accident.
const char *heat_model_disclaimer();

struct MaterialThermalModel
{
    std::string material;

    // The temperature above which the deposited material stops holding shape.
    // Absent means nobody characterized it, which caps the confidence of every
    // estimate that depends on it.
    std::optional<double> softening_c;
    // How fast this material sheds heat, relative to a nominal 1.0. Absent
    // means unknown.
    std::optional<double> cooling_factor;
};

struct EnvironmentModel
{
    std::optional<double> bed_temperature_c;
    std::optional<double> chamber_temperature_c;
    std::optional<double> ambient_temperature_c;
};

struct CoolingOperation
{
    size_t index { 0 };
    int    tool { 0 };

    std::string material;
    double      print_z { 0. };
    double      x { 0. };
    double      y { 0. };

    double extrusion_temperature_c { 0. };
    double deposited_volume_mm3 { 0. };
    double wall_thickness_mm { 0. };
    // How long this operation's layer takes; a short layer gives the previous
    // one less time to cool.
    double layer_time_s { 0. };
    // 0 to 1.
    double fan_fraction { 0. };

    bool is_bridge { false };
    bool is_overhang { false };
    bool is_first_layer { false };

    // Operations that must come before this one. Reordering never crosses
    // these; Phase 3 owns the dependency order and this phase respects it.
    std::vector<size_t> depends_on;
};

// The bounds that make this an approximation rather than a claim.
struct HeatModelBounds
{
    // The estimate is clamped into [extrusion temperature - this, extrusion
    // temperature + this]. Nothing this model produces escapes the clamp.
    double max_delta_c { 60. };
    // Two operations closer than this in XY and adjacent in Z are treated as
    // heating each other.
    double neighbourhood_mm { 5. };
    // How many recent operations are considered as neighbours at all.
    size_t recent_window { 12 };
};

struct HeatEstimate
{
    size_t operation { 0 };

    double     estimated_temperature_c { 0. };
    Confidence confidence { Confidence::Insufficient };
    // True when the clamp actually bit, so a reader knows the number is a
    // bound rather than a computation.
    bool clamped { false };

    // Every contribution that went into the number, named.
    std::vector<std::string> basis;
    // What was missing, and therefore what the confidence reflects.
    std::vector<std::string> gaps;
};

std::vector<HeatEstimate> estimate_heat_accumulation(
    const std::vector<CoolingOperation>      &operations,
    const std::vector<MaterialThermalModel>  &materials,
    const EnvironmentModel                   &environment,
    const HeatModelBounds                    &bounds);

// --- remedies --------------------------------------------------------------

enum class CoolingRemedy : uint8_t
{
    None,
    // Wait before starting. Always safe, so it is the low-confidence fallback.
    Delay,
    IncreaseFan,
    ReduceSpeed,
    LowerTemperature,
    // Move an independent operation elsewhere in the order.
    Reorder,
};

const char *cooling_remedy_name(CoolingRemedy remedy);

struct CoolingPolicy
{
    // Propose nothing until the estimate exceeds softening by this much.
    double tolerance_c { 0. };
    // The most a bounded temperature adjustment may move.
    double max_temperature_drop_c { 10. };
    double max_speed_reduction { 0.30 };
    double max_delay_s { 20. };

    // When the estimate's confidence is below this, only the conservative
    // remedy is offered.
    Confidence confident_at { Confidence::Medium };
};

struct CoolingAction
{
    size_t        operation { 0 };
    CoolingRemedy remedy { CoolingRemedy::None };

    // Seconds, fan fraction, speed multiplier or degrees, by remedy.
    double value { 0. };

    // Why this operation needed help and why this remedy was chosen over the
    // others. Never empty for a non-None remedy.
    std::vector<std::string> explanation;
};

struct CoolingPlan
{
    std::vector<HeatEstimate>  estimates;
    std::vector<CoolingAction> actions;

    // Operations that are too hot and that nothing may be done about, with the
    // reason each remedy was unavailable.
    std::vector<std::string> unresolved;
    std::vector<std::string> notes;

    std::string error;
};

// Plan cooling remedies over a schedule.
//
// Adhesion and dependencies are preserved absolutely: the first layer is never
// given more fan or a lower temperature, a bridge is never given less fan, and
// no reorder crosses a dependency. When the estimate behind a decision is not
// confident enough, only the conservative remedy is offered and the plan says
// why.
CoolingPlan plan_cooling(const std::vector<CoolingOperation>     &operations,
                         const std::vector<MaterialThermalModel> &materials,
                         const EnvironmentModel                  &environment,
                         const HeatModelBounds                   &bounds,
                         const CoolingPolicy                     &policy);

} // namespace Slic3r

#endif // slic3r_CoolingScheduler_hpp_
