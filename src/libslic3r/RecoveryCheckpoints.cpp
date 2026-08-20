///|/ TigerSlicer Phase 13: recoverable toolchanger G-code.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "RecoveryCheckpoints.hpp"

#include <algorithm>
#include <sstream>

namespace Slic3r {

const char *fault_kind_name(FaultKind kind)
{
    switch (kind) {
    case FaultKind::PickupFailure:   return "pickup failure";
    case FaultKind::ParkingFailure:  return "parking failure";
    case FaultKind::HeaterFault:     return "heater fault";
    case FaultKind::FilamentRunout:  return "filament runout";
    case FaultKind::CleaningFailure: return "cleaning failure";
    case FaultKind::UserPause:       return "pause";
    }
    return "unknown fault";
}

const char *pressure_state_name(PressureState state)
{
    switch (state) {
    case PressureState::Relieved:    return "relieved";
    case PressureState::Pressurized: return "pressurized";
    case PressureState::Unknown:     return "unknown";
    }
    return "unknown";
}

const char *recovery_severity_name(RecoverySeverity severity)
{
    switch (severity) {
    case RecoverySeverity::Info:    return "info";
    case RecoverySeverity::Warning: return "warning";
    case RecoverySeverity::Blocked: return "blocked";
    }
    return "info";
}

const char *resume_verdict_name(ResumeVerdict verdict)
{
    switch (verdict) {
    case ResumeVerdict::ResumeFromCheckpoint: return "resume from checkpoint";
    case ResumeVerdict::CannotResume:         return "cannot resume";
    }
    return "cannot resume";
}

bool RecoveryPlan::has_blocking_note() const
{
    return std::any_of(notes.begin(), notes.end(), [](const RecoveryNote &note) {
        return note.severity == RecoverySeverity::Blocked;
    });
}

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

void replace_all(std::string &text, const std::string &token, const std::string &value)
{
    if (token.empty())
        return;
    for (size_t at = text.find(token); at != std::string::npos;
         at        = text.find(token, at + value.size()))
        text.replace(at, token.size(), value);
}

// A hook that was never configured produces nothing at all. It is deliberately
// not an error here: the caller reports it as a limitation so the operator can
// see what the firmware did not offer.
std::string substitute(const std::string &hook,
                       const RecoveryCheckpoint *checkpoint,
                       int                       tool,
                       std::optional<double>     temperature)
{
    if (hook.empty())
        return {};
    std::string result = hook;
    replace_all(result, "[tool]", std::to_string(tool));
    if (checkpoint != nullptr) {
        replace_all(result, "[operation]", std::to_string(checkpoint->after_operation));
        replace_all(result, "[z]", number(checkpoint->print_z, 3));
        replace_all(result, "[e]", number(checkpoint->extruder_position_mm, 5));
        replace_all(result, "[elapsed]", number(checkpoint->elapsed_s, 1));
    }
    if (temperature.has_value())
        replace_all(result, "[temperature]", number(*temperature, 0));
    return result;
}

const ToolTemperature *find_temperature(const std::vector<ToolTemperature> &list, int tool)
{
    for (const ToolTemperature &entry : list)
        if (entry.tool == tool)
            return &entry;
    return nullptr;
}

} // namespace

RecoveryPlan plan_recovery_checkpoints(const std::vector<RecoverableOperation> &operations,
                                       const RecoveryOptions                   &options,
                                       const RecoveryCapabilities              &capabilities)
{
    RecoveryPlan plan;

    if (!options.enabled) {
        plan.notes.push_back({ RecoverySeverity::Info,
                               "recovery checkpoints are disabled; a fault will not be "
                               "resumable" });
        return plan;
    }

    if (operations.empty()) {
        plan.error = "there are no operations to checkpoint";
        return plan;
    }

    const bool can_write = capabilities.can_record_checkpoint &&
                           !capabilities.checkpoint_gcode.empty();
    if (!can_write)
        plan.limitations.push_back(
            capabilities.can_record_checkpoint
                ? "the firmware declares checkpoint support but no checkpoint G-code is "
                  "configured, so no checkpoint can be written"
                : "the firmware does not declare checkpoint support, so no checkpoint can be "
                  "written");

    bool   any_safe_boundary = false;
    bool   have_previous     = false;
    double previous_s        = 0.;

    for (size_t index = 0; index < operations.size(); ++index) {
        const RecoverableOperation &operation = operations[index];

        if (!operation.safe_to_interrupt_after) {
            // Recorded, not silently skipped: an operator looking at a long
            // stretch without checkpoints should be able to see why.
            plan.notes.push_back(
                { RecoverySeverity::Info,
                  "operation " + std::to_string(operation.index) +
                      " cannot be interrupted safely" +
                      (operation.unsafe_reason.empty() ? std::string {}
                                                       : ": " + operation.unsafe_reason) });
            continue;
        }
        any_safe_boundary = true;

        if (options.synchronization_planes_only && !operation.on_synchronization_plane)
            continue;

        if (have_previous && operation.elapsed_s - previous_s < options.min_interval_s)
            continue;

        RecoveryCheckpoint checkpoint;
        checkpoint.after_operation          = operation.index;
        checkpoint.active_tool              = operation.tool;
        checkpoint.print_z                  = operation.print_z;
        checkpoint.on_synchronization_plane = operation.on_synchronization_plane;
        checkpoint.elapsed_s                = operation.elapsed_s;
        checkpoint.extruder_position_mm     = operation.extruder_position_mm;
        checkpoint.pressure                 = operation.pressure;
        checkpoint.temperatures             = operation.temperatures;
        if (index + 1 < operations.size())
            checkpoint.expected_next_tool = operations[index + 1].tool;

        // Pressure that was never recorded must be treated as still present.
        // Assuming it was relieved is the assumption that blobs a resumed
        // print.
        if (operation.pressure == PressureState::Pressurized) {
            checkpoint.requires_reprime = true;
            checkpoint.reprime_reasons.push_back("the melt zone was left pressurized");
        } else if (operation.pressure == PressureState::Unknown) {
            checkpoint.requires_reprime = true;
            checkpoint.reprime_reasons.push_back(
                "the pressure state was not recorded, so it cannot be assumed relieved");
        }

        if (checkpoint.temperatures.empty())
            plan.notes.push_back({ RecoverySeverity::Warning,
                                   "checkpoint after operation " +
                                       std::to_string(operation.index) +
                                       " records no temperatures, so resuming from it would "
                                       "have to reheat blind" });

        if (can_write) {
            const ToolTemperature *active =
                find_temperature(checkpoint.temperatures, checkpoint.active_tool);
            checkpoint.gcode = substitute(capabilities.checkpoint_gcode, &checkpoint,
                                          checkpoint.active_tool,
                                          active != nullptr ? active->target_c : std::nullopt);
            checkpoint.emitted = !checkpoint.gcode.empty();
        }

        plan.checkpoints.push_back(checkpoint);
        have_previous = true;
        previous_s    = operation.elapsed_s;
    }

    if (plan.checkpoints.empty()) {
        if (!any_safe_boundary)
            plan.notes.push_back({ RecoverySeverity::Blocked,
                                   "no operation in this print can be interrupted safely, so "
                                   "no checkpoint can be placed" });
        else
            plan.notes.push_back({ RecoverySeverity::Warning,
                                   "the checkpoint spacing rules excluded every safe boundary, "
                                   "so this print has no checkpoints" });
    }

    return plan;
}

ResumeDecision plan_resume(const RecoveryPlan                      &plan,
                           const std::vector<RecoverableOperation> &operations,
                           size_t                                   interrupted_operation,
                           FaultKind                                fault,
                           const RecoveryCapabilities              &capabilities)
{
    ResumeDecision decision;

    // The interrupted operation is never resumed where it stopped. Recovery
    // rewinds to a boundary that was actually recorded and redoes the work.
    const RecoveryCheckpoint *chosen = nullptr;
    size_t                    chosen_index = 0;
    for (size_t index = 0; index < plan.checkpoints.size(); ++index) {
        const RecoveryCheckpoint &checkpoint = plan.checkpoints[index];
        if (checkpoint.after_operation >= interrupted_operation)
            break;
        chosen       = &checkpoint;
        chosen_index = index;
    }

    if (chosen == nullptr) {
        decision.reasons.push_back("no checkpoint was recorded before operation " +
                                   std::to_string(interrupted_operation) +
                                   ", so there is no state to resume from");
        return decision;
    }

    decision.reasons.push_back(std::string(fault_kind_name(fault)) + " during operation " +
                               std::to_string(interrupted_operation) +
                               "; the last recorded checkpoint is after operation " +
                               std::to_string(chosen->after_operation));

    // Everything between the checkpoint and the fault is redone. If any of it
    // was unsafe to interrupt, that is exactly why we wound back this far.
    for (const RecoverableOperation &operation : operations)
        if (operation.index > chosen->after_operation &&
            operation.index <= interrupted_operation && !operation.safe_to_interrupt_after) {
            decision.reasons.push_back("operation " + std::to_string(operation.index) +
                                       " is not safe to interrupt, so it is redone in full "
                                       "rather than continued");
            break;
        }

    // A pickup or parking failure means the machine may be holding a tool
    // nobody expected. Resuming without establishing that is how a toolhead
    // gets driven into the print.
    if (fault == FaultKind::PickupFailure || fault == FaultKind::ParkingFailure) {
        decision.requires_tool_verification = true;
        if (!capabilities.can_verify_tool_state || capabilities.verify_tool_gcode.empty()) {
            decision.reasons.push_back(
                "after a " + std::string(fault_kind_name(fault)) +
                " the tool actually held is unknown and the firmware cannot verify it");
            return decision;
        }
        decision.actions.push_back(
            substitute(capabilities.verify_tool_gcode, chosen, chosen->active_tool, std::nullopt));
    }

    // Temperatures have to come back before anything extrudes. A tool with no
    // recorded target cannot be restored, and guessing one is worse than
    // stopping.
    const bool can_heat =
        capabilities.can_restore_temperature && !capabilities.restore_temperature_gcode.empty();
    std::vector<std::string> heat_actions;
    for (const ToolTemperature &entry : chosen->temperatures) {
        if (!entry.target_c.has_value())
            continue;
        decision.requires_reheat = true;
        if (can_heat)
            heat_actions.push_back(substitute(capabilities.restore_temperature_gcode, chosen,
                                              entry.tool, entry.target_c));
    }

    if (fault == FaultKind::HeaterFault)
        decision.requires_reheat = true;

    if (decision.requires_reheat) {
        if (!can_heat) {
            decision.reasons.push_back(
                "the recorded temperatures cannot be restored because the firmware declares no "
                "usable temperature hook");
            return decision;
        }
        if (heat_actions.empty()) {
            decision.reasons.push_back("no target temperature was recorded at this checkpoint, "
                                       "so the tools cannot be brought back to a known state");
            return decision;
        }
        decision.actions.insert(decision.actions.end(), heat_actions.begin(),
                                heat_actions.end());
    }

    decision.requires_reprime = chosen->requires_reprime;
    for (const std::string &reason : chosen->reprime_reasons)
        decision.reasons.push_back(reason);

    // A runout or a failed clean means the nozzle contents are not what the
    // checkpoint describes, whatever the pressure state said.
    if (fault == FaultKind::FilamentRunout) {
        decision.requires_reprime = true;
        decision.reasons.push_back("the filament ran out, so the melt zone must be refilled "
                                   "before printing resumes");
    } else if (fault == FaultKind::CleaningFailure) {
        decision.requires_reprime = true;
        decision.reasons.push_back("cleaning did not complete, so the nozzle must be purged "
                                   "before printing resumes");
    }

    if (decision.requires_reprime) {
        if (!capabilities.can_reprime || capabilities.reprime_gcode.empty()) {
            decision.reasons.push_back("a reprime is required but the firmware declares no "
                                       "usable reprime hook");
            return decision;
        }
        decision.actions.push_back(
            substitute(capabilities.reprime_gcode, chosen, chosen->active_tool, std::nullopt));
    }

    if (capabilities.resume_gcode.empty()) {
        decision.reasons.push_back("no resume G-code is configured, so the print cannot be "
                                   "returned to the checkpoint position");
        return decision;
    }
    decision.actions.push_back(
        substitute(capabilities.resume_gcode, chosen, chosen->active_tool, std::nullopt));

    decision.verdict              = ResumeVerdict::ResumeFromCheckpoint;
    decision.checkpoint           = chosen_index;
    decision.restart_at_operation = chosen->after_operation + 1;
    return decision;
}

} // namespace Slic3r
