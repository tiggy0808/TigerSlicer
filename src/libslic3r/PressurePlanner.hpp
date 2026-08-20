///|/ TigerSlicer Phase 17: per-tool pressure-transient planning.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PressurePlanner_hpp_
#define slic3r_PressurePlanner_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// What is happening to the melt zone at a point in the print.
enum class PressureEventKind : uint8_t
{
    Steady,
    Accelerate,
    Decelerate,
    FlowChange,
    WidthChange,
    HeightChange,
    BridgeEntry,
    BridgeExit,
    Retract,
    Unretract,
    Park,
    Idle,
    Reheat,
    Prime,
    Pickup,
};

const char *pressure_event_kind_name(PressureEventKind kind);

// Whether the melt zone is at the pressure the current extrusion needs.
enum class PressureCondition : uint8_t
{
    Established,
    // Deliberately let down - retracted, parked, idle.
    Released,
    // Coming back up but not there yet.
    Recovering,
    // Nobody knows. Never treated as Established.
    Unknown,
};

const char *pressure_condition_name(PressureCondition condition);

// Pressure advance for one tool, either as a single value or as a curve over
// flow. The curve is never extrapolated: a flow outside the measured range
// produces no value, exactly as the flow curves in Phase 8 behave.
struct PressureAdvanceCurve
{
    int         tool { -1 };
    std::string material;
    // Absent means the curve was not characterized per temperature.
    std::optional<double> temperature_c;
    // Half-width of the temperature band this curve speaks for.
    double temperature_tolerance_c { 10. };

    // Flow (mm3/s) -> advance (s), ascending by flow. A single point is a
    // constant value.
    std::vector<std::pair<double, double>> points;

    bool covers(double flow_mm3_s) const;
};

// Resolve the advance for a concrete condition.
//
// Returns nothing, with a reason, when no curve covers it. An unresolved
// advance must not fall back to zero: zero is a real setting that means "no
// compensation", not "we do not know".
std::optional<double> resolve_pressure_advance(const std::vector<PressureAdvanceCurve> &curves,
                                               int                                      tool,
                                               const std::string     &material,
                                               std::optional<double>  temperature_c,
                                               double                 flow_mm3_s,
                                               std::string           *reason);

// One stretch of extrusion, as far as pressure is concerned.
struct PressureSegment
{
    size_t index { 0 };
    int    tool { 0 };

    PressureEventKind kind { PressureEventKind::Steady };

    std::string           material;
    std::optional<double> temperature_c;

    double flow_mm3_s { 0. };
    double extrusion_width_mm { 0. };
    double layer_height_mm { 0. };

    // How much travel is available before this segment starts printing, for a
    // lead-in to occupy.
    double available_lead_in_mm { 0. };
    // True when this segment prints something the user will see. Preparation
    // extrusion must never land here.
    bool on_visible_surface { false };
};

// What the firmware declared. As everywhere, a capability without a hook is
// disabled and reported rather than fabricated.
struct PressureCapabilities
{
    bool can_set_pressure_advance { false };
    // Tokens: [tool] [advance].
    std::string set_advance_gcode;
};

struct PressurePolicy
{
    // Ignore advance changes smaller than this; retuning for noise costs a
    // command and gains nothing.
    double min_advance_change_s { 0.002 };
    // How much lead-in a full pressure restoration needs.
    double restore_lead_in_mm { 2.0 };
};

enum class PressureActionKind : uint8_t
{
    SetAdvance,
    LeadIn,
    Restore,
    Release,
};

const char *pressure_action_kind_name(PressureActionKind kind);

struct PressureAction
{
    size_t             at_segment { 0 };
    int                tool { 0 };
    PressureActionKind kind { PressureActionKind::SetAdvance };

    // The advance being set, or the lead-in length in mm.
    double value { 0. };

    std::string reason;
    // Present only when a declared hook produced it.
    std::string gcode;
    bool        emitted { false };
};

enum class PressureSeverity : uint8_t
{
    Info,
    Warning,
    Blocked,
};

const char *pressure_severity_name(PressureSeverity severity);

struct PressureNote
{
    size_t           at_segment { 0 };
    int              tool { -1 };
    PressureSeverity severity { PressureSeverity::Info };
    std::string      message;
};

struct PressurePlan
{
    std::vector<PressureAction> actions;
    std::vector<PressureNote>   notes;

    // Pressure condition at the end of each segment, same length as the input.
    std::vector<PressureCondition> conditions;

    // Capabilities the plan needed and the firmware never declared.
    std::vector<std::string> limitations;

    std::string error;

    bool has_blocking_note() const;
};

// Plan pressure across a print.
//
// Two rules do the real work. A bridge, or any segment resumed after the
// pressure was released, needs a lead-in before it starts printing; when there
// is not enough travel for one, that is a blocking note rather than a shortened
// lead-in that silently under-extrudes. And a lead-in is never placed on a
// visible surface: preparation extrusion belongs anywhere except where it will
// be seen.
PressurePlan plan_pressure(const std::vector<PressureSegment>       &segments,
                           const std::vector<PressureAdvanceCurve>  &curves,
                           const PressureCapabilities               &capabilities,
                           const PressurePolicy                     &policy);

} // namespace Slic3r

#endif // slic3r_PressurePlanner_hpp_
