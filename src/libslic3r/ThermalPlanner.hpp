///|/ TigerSlicer Phase 6: predictive heating and ooze control.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ThermalPlanner_hpp_
#define slic3r_ThermalPlanner_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/ToolchangeCostModel.hpp"

namespace Slic3r {

// What the material tolerates. Every limit is optional because an unknown
// limit must read as "not characterized", never as "no limit".
struct MaterialThermalLimits
{
    std::string material;

    // How long this material may sit at printing temperature without
    // degrading. Absent means nobody has characterized it.
    std::optional<double> max_hot_idle_s;
    // Temperature above which a stationary nozzle oozes.
    std::optional<double> ooze_onset_c;
    // Absolute bounds; a target outside them is refused, not clamped.
    std::optional<double> min_active_c;
    std::optional<double> max_active_c;
};

// What the firmware has actually declared it can do.
//
// TigerSlicer never invents a thermal command. A capability that is not
// declared here produces a reported limitation, not a guessed G-code line.
struct FirmwareThermalCapabilities
{
    bool can_set_tool_temperature { false };
    bool can_set_standby_temperature { false };
    bool can_wait_for_temperature { false };

    // Configurable hooks. `[tool]` and `[temperature]` are substituted. An
    // empty hook means the command was not configured, which disables it even
    // when the corresponding capability flag is set.
    std::string set_temperature_gcode;
    std::string set_standby_gcode;
    std::string wait_temperature_gcode;
};

// When a tool is actually printing, in elapsed print time.
struct ToolUsageWindow
{
    int    tool { 0 };
    double start_s { 0. };
    double end_s { 0. };

    double duration_s() const { return end_s > start_s ? end_s - start_s : 0.; }
};

enum class ThermalActionKind : uint8_t
{
    Preheat,             // begin bringing a tool up before it is needed
    HoldStandby,         // drop to standby between nearby uses
    DropToIdle,          // drop further, for a long gap or after the last use
    WaitForTemperature,  // the print must actually wait
};

const char *thermal_action_kind_name(ThermalActionKind kind);

struct ThermalAction
{
    double            at_s { 0. };
    int               tool { 0 };
    ThermalActionKind kind { ThermalActionKind::Preheat };
    double            target_c { 0. };

    // Why this action exists, for Preview.
    std::string reason;
    // The command that will be emitted, from the declared hook. Empty when
    // the firmware did not declare the capability: the action is still
    // reported so the plan is legible, but nothing is fabricated.
    std::string gcode;
    bool        emitted { false };
};

enum class ThermalSeverity : uint8_t
{
    Info,
    Warning,
    Blocked,
};

const char *thermal_severity_name(ThermalSeverity severity);

struct ThermalNote
{
    int             tool { -1 };
    ThermalSeverity severity { ThermalSeverity::Info };
    std::string     message;
};

// What preparing a tool for use actually requires, given how it was left.
struct PrimeRequirement
{
    double purge_volume_mm3 { 0. };
    int    brush_passes { 0 };
    bool   needs_pressure_recovery { false };
    std::vector<std::string> reasons;
};

struct ThermalPlan
{
    std::vector<ThermalAction> actions;
    std::vector<ThermalNote>   notes;

    // Time the print will actually spend waiting on a heater, and time tools
    // spend sitting at printing temperature without printing.
    double total_heating_wait_s { 0. };
    double total_hot_idle_s { 0. };

    // Capabilities the plan needed but the firmware never declared.
    std::vector<std::string> limitations;

    // Non-empty when no plan could be produced.
    std::string error;

    bool has_blocking_note() const;
};

// Plan active, standby and idle temperatures across a print.
//
// Heating is scheduled just in time: a tool is brought up exactly early
// enough to be ready, which avoids both a wait and a long hot idle. When the
// available lead time is genuinely too short, the resulting wait is reported
// rather than absorbed.
ThermalPlan plan_thermals(const std::vector<ToolUsageWindow>        &windows,
                          const std::vector<ToolThermalState>       &thermal,
                          const std::vector<MaterialThermalLimits>  &materials,
                          const FirmwareThermalCapabilities         &firmware);

// Work out prime/wipe preparation for one tool pickup.
//
// `idle_s` is how long the tool sat unused, `temperature_c` where it sat, and
// `depressurized` whether pressure was deliberately released. A base volume of
// zero with an uncharacterized material yields a reported requirement of zero
// and a reason saying so, rather than an invented number.
PrimeRequirement estimate_prime_requirement(const MaterialThermalLimits &material,
                                            double idle_s,
                                            double temperature_c,
                                            bool   depressurized,
                                            double base_purge_volume_mm3,
                                            int    base_brush_passes);

} // namespace Slic3r

#endif // slic3r_ThermalPlanner_hpp_
