///|/ TigerSlicer Phase 4: toolchange-aware global schedule optimization.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ToolchangeOptimizer_hpp_
#define slic3r_ToolchangeOptimizer_hpp_

#include <string>
#include <vector>

#include "libslic3r/MultiRateLayerSchedule.hpp"
#include "libslic3r/ToolchangeCostModel.hpp"

namespace Slic3r {

// What the optimizer is being asked to trade away.
//
// These are genuinely different objectives, not presets of one objective: the
// fastest schedule and the least wasteful schedule disagree whenever a cheap
// change sits between two expensive purges.
enum class OptimizationPolicy : uint8_t
{
    Fastest,                    // minimize total elapsed time
    Balanced,                   // time, weighted against waste
    BestDetail,                 // never delay work a quality edge depends on
    FewestChanges,              // minimize the number of toolchanges
    LeastWaste,                 // minimize purged material
    PreserveExplicitAssignment, // keep user-assigned work in its original order
};

const char *optimization_policy_name(OptimizationPolicy policy);

// Everything the optimizer needs that the schedule itself does not carry.
//
// Per-operation vectors are indexed by operation id. A short vector is a
// configuration error, not a licence to assume zero: `optimize_toolchanges`
// refuses rather than costing work it was never told about.
struct OptimizerInputs
{
    std::vector<double> extrusion_s;
    // Work whose tool the user chose explicitly. The optimizer never reassigns
    // any tool, so this only affects ordering freedom, never ownership.
    std::vector<bool> explicitly_assigned;

    MacroTimingContract contract;
    // Indexed by tool id.
    std::vector<ToolThermalState> thermal;

    double dock_travel_mm { 0. };
    double travel_speed_mm_s { 0. };
    double purge_flow_mm3_s { 0. };
    int    brush_passes { 0 };

    // Purge volume for a from -> to transition, row-major, tool_count square.
    // Empty means "not characterized", which is reported, never guessed at 0.
    std::vector<double> purge_volume_mm3;
    size_t tool_count { 0 };

    int  initial_tool { 0 };
    bool probe_after_pickup { false };
    bool allow_retry_allowance { false };

    double purge_volume_for(int from_tool, int to_tool) const;
};

// Why a toolchange appears in a schedule. Every change in the optimized order
// carries one of these, so a report can say what it could not remove.
enum class ToolchangeNecessity : uint8_t
{
    // No unscheduled work on the current tool was ready: a dependency was
    // holding all of it back.
    DependencyForced,
    // The current tool has no remaining work at all.
    ToolExhausted,
    // Work on the current tool existed and was ready, but the policy chose to
    // change anyway. Only reachable under BestDetail.
    PolicyChosen,
};

const char *toolchange_necessity_name(ToolchangeNecessity necessity);

struct ToolchangeEvent
{
    size_t operation { 0 };     // the operation the change was made for
    int    from_tool { 0 };
    int    to_tool { 0 };
    ToolchangeCost      cost;
    ToolchangeNecessity necessity { ToolchangeNecessity::ToolExhausted };
    // Names the constraint responsible, so a refusal to optimize further
    // explains itself instead of asserting that nothing can be done.
    std::string explanation;
};

struct ScheduleEvaluation
{
    std::vector<size_t>          order;
    std::vector<ToolchangeEvent> toolchanges;
    PrintTimeBreakdown           breakdown;
    double                       purge_volume_mm3 { 0. };

    size_t toolchange_count() const { return toolchanges.size(); }
    double total_s() const { return breakdown.total_s(); }
};

struct OptimizationResult
{
    ScheduleEvaluation baseline;
    ScheduleEvaluation optimized;

    // False when the optimizer could not beat the baseline. `optimized` then
    // holds the baseline verbatim: a schedule is never reported as improved
    // when it is not.
    bool improved { false };

    // Changes present in the baseline that the optimized order removed, and
    // any transition it introduced. Both are reported: batching work often
    // trades several changes for one, and showing only the removals would
    // overstate the result.
    std::vector<std::string> eliminated;
    std::vector<std::string> introduced;
    // Changes the optimizer kept, with the reason it could not remove them.
    std::vector<std::string> unavoidable;
    // Anything that limited the search, so a capped result never reads as an
    // exhaustive one.
    std::vector<std::string> notes;

    // Non-empty when nothing could be produced. Both evaluations are empty.
    std::string error;
};

// Cost one specific ordering. `order` must be a valid topological order of the
// schedule; it is verified, and a violation is an error rather than a silently
// mis-costed result.
ScheduleEvaluation evaluate_schedule(const MultiRateSchedule &schedule,
                                     const std::vector<size_t> &order,
                                     const OptimizerInputs &inputs,
                                     std::string *error = nullptr);

// Reorder the schedule to reduce toolchange cost under `policy`.
//
// The optimizer only ever reorders. It never reassigns a tool, so an explicit
// colour or tool choice cannot be altered by optimization at all: the tool of
// every operation is identical in the baseline and optimized orders. Z order,
// support, interface, enclosure and collision constraints are respected
// because the result is required to be a topological order of the same
// dependency graph.
OptimizationResult optimize_toolchanges(const MultiRateSchedule &schedule,
                                        const OptimizerInputs &inputs,
                                        OptimizationPolicy policy);

} // namespace Slic3r

#endif // slic3r_ToolchangeOptimizer_hpp_
