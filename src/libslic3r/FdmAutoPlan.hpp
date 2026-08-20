///|/ TigerSlicer Phase AUTO2: the FDM Auto Prepare & Slice planner's mapping
///|/ into the shared optimizer.
///|/
///|/ AUTO2 must reuse the existing engines through their canonical APIs and
///|/ must not create competing implementations. So this module does not
///|/ re-decide tool assignment, flow limits or detail decomposition: it
///|/ *consumes* their result types (ToolProposal from ToolAssignment, FlowResult
///|/ from FlowCurves) and maps their verdicts into the AUTO1 hard-constraint /
///|/ objective vocabulary, applying the FDM-specific lock and colour policy on
///|/ top. Testable libslic3r logic.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FdmAutoPlan_hpp_
#define slic3r_FdmAutoPlan_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "AutoPlanOptimizer.hpp"
#include "FlowCurves.hpp"
#include "ToolAssignment.hpp"

namespace Slic3r {

// A user lock on one feature: a colour, a material, or a specific tool the user
// fixed. Auto planning may optimize only unlocked choices.
enum class FdmLockKind : uint8_t
{
    None,
    Colour,
    Material,
    Tool,
};

const char *fdm_lock_kind_name(FdmLockKind kind);

struct FdmFeatureLock
{
    size_t      feature { 0 };
    FdmLockKind kind { FdmLockKind::None };
    std::string value;   // the locked colour / material name / tool id
};

// One feature as the FDM planner sees it after the existing engines ran: the
// tool assignment the ToolAssignment engine proposed, the flow result the
// FlowCurves engine returned for the tool that would print it, the colour the
// feature requires, and the colour the assigned tool is actually loaded with.
struct FdmFeaturePlan
{
    size_t      feature { 0 };
    std::string name;

    // From ToolAssignment - consumed, not recomputed.
    ToolProposal assignment;

    // From FlowCurves for the assigned tool - consumed, not recomputed.
    FlowResult flow;

    // The colour/material the feature requires, and what the assigned tool is
    // loaded with. Empty required means the feature imposes none; empty loaded
    // means the machine did not report it (which is Unknown, not a match).
    std::string required_colour;
    std::string assigned_tool_colour;
    std::string required_material;
    std::string assigned_tool_material;

    // A lock on this feature, if any.
    std::optional<FdmFeatureLock> lock;
};

// One whole FDM candidate configuration: the per-feature plans plus the
// candidate's rolled-up objective values (from the existing time/toolchange/
// waste estimators, again consumed not recomputed).
struct FdmCandidatePlan
{
    std::string id;
    std::string summary;

    std::vector<FdmFeaturePlan> features;

    // Objective values, supplied by the estimators.
    double predicted_quality { 0. };   // higher better
    double predicted_time_s { 0. };    // lower better
    double predicted_toolchanges { 0. }; // lower better
    double predicted_waste_mm3 { 0. }; // lower better
};

// Map one FDM candidate configuration into an AutoPlanCandidate for the shared
// optimizer.
//
// Each feature's existing-engine verdict becomes a hard constraint:
//   - ToolAssignment NoCompatibleTool  -> Violated (nothing can print it)
//   - ToolAssignment NeedsMeasurement  -> Unknown  (blocks; surfaces the datum)
//   - FlowCurves exceeds_limit         -> Violated (wants more than the tool gives)
//   - FlowCurves NoCalibrationForTool  -> Unknown  (no verified flow curve)
//   - a required colour the assigned tool is not loaded with -> Violated
//       (never remap a colour to a visibly different filament automatically)
//   - a required colour/material the machine did not report  -> Unknown
//   - a lock the candidate would break -> the candidate does not respect locks
// Ambiguous assignment is a note, not a block: the optimizer may choose among
// equally suitable tools and the choice is explained.
//
// The objective values are passed through unchanged; this maps verdicts, it
// does not re-score physics.
AutoPlanCandidate map_fdm_candidate(const FdmCandidatePlan &plan);

// Build an AutoPlanRequest from several FDM candidate configurations and the
// FDM objective weights (from the quality-to-speed slider). A convenience over
// mapping each candidate and assembling the request.
AutoPlanRequest build_fdm_request(const std::vector<FdmCandidatePlan> &candidates,
                                  const ObjectiveVector               &objectives,
                                  uint64_t                             seed);

// The FDM objective vector for a slider position, 0 (fastest) .. 1 (finest).
// The slider moves only these soft weights; it never affects a hard constraint.
ObjectiveVector fdm_objectives_for_slider(double quality_to_speed);

} // namespace Slic3r

#endif // slic3r_FdmAutoPlan_hpp_
