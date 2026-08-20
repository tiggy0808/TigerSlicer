///|/ TigerSlicer Phase AUTO2: the FDM Auto Prepare & Slice planner's mapping
///|/ into the shared optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FdmAutoPlan.hpp"

namespace Slic3r {

const char *fdm_lock_kind_name(FdmLockKind kind)
{
    switch (kind) {
    case FdmLockKind::None:     return "none";
    case FdmLockKind::Colour:   return "colour";
    case FdmLockKind::Material: return "material";
    case FdmLockKind::Tool:     return "tool";
    }
    return "none";
}

namespace {

ConstraintResult satisfied(const std::string &name, const std::string &detail = "")
{
    return { name, ConstraintOutcome::Satisfied, detail, "" };
}

ConstraintResult violated(const std::string &name, const std::string &detail)
{
    return { name, ConstraintOutcome::Violated, detail, "" };
}

ConstraintResult unknown(const std::string &name, const std::string &detail,
                         const std::string &datum)
{
    return { name, ConstraintOutcome::Unknown, detail, datum };
}

// Does the candidate's proposed tool for this feature break the feature's lock?
bool breaks_lock(const FdmFeaturePlan &feature, std::string &description)
{
    if (! feature.lock.has_value() || feature.lock->kind == FdmLockKind::None)
        return false;
    const FdmFeatureLock &lock = *feature.lock;

    // A lock is broken when the resolved assignment departs from the locked
    // value. The proposed tool is only meaningful for Proposed/Unchanged; for
    // those a Tool lock is checked directly. Colour/Material locks are checked
    // against what the assigned tool carries.
    switch (lock.kind) {
    case FdmLockKind::Tool: {
        // Proposed and Unchanged both carry a meaningful proposed_tool; KeptManual
        // lands on current_tool. In all three the feature really prints on that
        // tool, so a Tool lock is checked directly. Only checking Proposed let an
        // Unchanged assignment silently override the user's lock.
        const bool proposed_meaningful =
            feature.assignment.outcome == AssignmentOutcome::Proposed ||
            feature.assignment.outcome == AssignmentOutcome::Unchanged;
        const bool kept_manual =
            feature.assignment.outcome == AssignmentOutcome::KeptManual;
        if (proposed_meaningful || kept_manual) {
            const int landed = proposed_meaningful ? feature.assignment.proposed_tool
                                                   : feature.assignment.current_tool;
            if (std::to_string(landed) != lock.value) {
                description = "tool lock " + lock.value + " on \"" + feature.name + "\"";
                return true;
            }
        }
        return false;
    }
    case FdmLockKind::Colour:
        if (! feature.assigned_tool_colour.empty() &&
            feature.assigned_tool_colour != lock.value) {
            description = "colour lock " + lock.value + " on \"" + feature.name + "\"";
            return true;
        }
        return false;
    case FdmLockKind::Material:
        if (! feature.assigned_tool_material.empty() &&
            feature.assigned_tool_material != lock.value) {
            description = "material lock " + lock.value + " on \"" + feature.name + "\"";
            return true;
        }
        return false;
    case FdmLockKind::None:
        return false;
    }
    return false;
}

} // namespace

AutoPlanCandidate map_fdm_candidate(const FdmCandidatePlan &plan)
{
    AutoPlanCandidate candidate;
    candidate.id      = plan.id;
    candidate.summary = plan.summary;

    for (const FdmFeaturePlan &feature : plan.features) {
        const std::string where = "\"" + feature.name + "\"";

        // Tool assignment verdict.
        switch (feature.assignment.outcome) {
        case AssignmentOutcome::NoCompatibleTool:
            candidate.constraints.push_back(
                violated("tool for " + where, "no tool on the machine can print it"));
            break;
        case AssignmentOutcome::NeedsMeasurement:
            candidate.constraints.push_back(unknown(
                "tool for " + where,
                "a property the tool choice depends on was never measured",
                feature.name + ": feature measurement"));
            break;
        case AssignmentOutcome::Ambiguous:
            // Not a block: the optimizer may choose among equally suitable
            // tools. Recorded as satisfied with a note so the choice is
            // explained.
            candidate.constraints.push_back(
                satisfied("tool for " + where, "several tools are equally suitable"));
            break;
        case AssignmentOutcome::Proposed:
        case AssignmentOutcome::Unchanged:
        case AssignmentOutcome::KeptManual:
            candidate.constraints.push_back(satisfied("tool for " + where));
            break;
        }

        // Flow verdict for the assigned tool.
        if (feature.flow.exceeds_limit)
            candidate.constraints.push_back(violated(
                "flow for " + where,
                "the extrusion wants more than the tool's verified volumetric limit"));
        else if (feature.flow.limit == FlowLimit::NoCalibrationForTool)
            candidate.constraints.push_back(unknown(
                "flow for " + where, "no verified flow curve for the assigned tool",
                feature.name + ": flow curve"));
        else
            candidate.constraints.push_back(satisfied("flow for " + where));

        // Colour: never remap a required colour to a visibly different filament
        // automatically. A tool loaded with a different colour is a violation,
        // not a low score; an unreported loaded colour is Unknown.
        if (! feature.required_colour.empty()) {
            if (feature.assigned_tool_colour.empty())
                candidate.constraints.push_back(unknown(
                    "colour for " + where, "the loaded colour of the assigned tool is not "
                                           "reported", feature.name + ": loaded colour"));
            else if (feature.assigned_tool_colour != feature.required_colour)
                candidate.constraints.push_back(violated(
                    "colour for " + where,
                    "the feature requires \"" + feature.required_colour +
                        "\" but the assigned tool is loaded with \"" +
                        feature.assigned_tool_colour +
                        "\"; a colour is never remapped to a visibly different filament "
                        "automatically"));
            else
                candidate.constraints.push_back(satisfied("colour for " + where));
        }

        // Material: a required material the tool does not carry is a violation;
        // an unreported material is Unknown.
        if (! feature.required_material.empty()) {
            if (feature.assigned_tool_material.empty())
                candidate.constraints.push_back(unknown(
                    "material for " + where, "the loaded material of the assigned tool is not "
                                             "reported", feature.name + ": loaded material"));
            else if (feature.assigned_tool_material != feature.required_material)
                candidate.constraints.push_back(violated(
                    "material for " + where,
                    "the feature requires \"" + feature.required_material +
                        "\" but the assigned tool carries \"" +
                        feature.assigned_tool_material + "\""));
            else
                candidate.constraints.push_back(satisfied("material for " + where));
        }

        // Locks: a candidate that would change a locked choice does not respect
        // locks, and names what it would break.
        std::string broken;
        if (breaks_lock(feature, broken)) {
            candidate.respects_locks = false;
            candidate.would_break_locks.push_back(broken);
        }
    }

    // Objectives passed through unchanged - this maps verdicts, it does not
    // re-score physics.
    candidate.objectives = {
        { "quality", plan.predicted_quality },
        { "time_s", plan.predicted_time_s },
        { "toolchanges", plan.predicted_toolchanges },
        { "waste_mm3", plan.predicted_waste_mm3 },
    };
    return candidate;
}

AutoPlanRequest build_fdm_request(const std::vector<FdmCandidatePlan> &candidates,
                                  const ObjectiveVector               &objectives,
                                  uint64_t                             seed)
{
    AutoPlanRequest request;
    request.objectives = objectives;
    request.seed       = seed;
    for (const FdmCandidatePlan &plan : candidates)
        request.candidates.push_back(map_fdm_candidate(plan));
    return request;
}

ObjectiveVector fdm_objectives_for_slider(double quality_to_speed)
{
    // 0 = fastest, 1 = finest. Clamp, then split the weight between quality and
    // the cost objectives. The slider only moves these soft weights.
    const double q = quality_to_speed < 0. ? 0. : quality_to_speed > 1. ? 1. : quality_to_speed;
    const double quality_weight = q;
    const double speed_weight    = 1. - q;

    ObjectiveVector vector;
    vector.objectives = {
        { "quality", ObjectiveDirection::HigherIsBetter, quality_weight },
        { "time_s", ObjectiveDirection::LowerIsBetter, speed_weight },
        // Toolchanges and waste always matter a little, independent of the
        // slider, so a wildly wasteful plan never wins purely on speed.
        { "toolchanges", ObjectiveDirection::LowerIsBetter, 0.1 },
        { "waste_mm3", ObjectiveDirection::LowerIsBetter, 0.05 },
    };
    return vector;
}

} // namespace Slic3r
