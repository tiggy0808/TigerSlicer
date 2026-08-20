///|/ TigerSlicer Phase N: capability-gated printer quick actions.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterQuickActions.hpp"

#include <algorithm>

namespace Slic3r {

namespace {

ConfirmationLevel confirmation_for(ActionRisk risk)
{
    switch (risk) {
    case ActionRisk::ReadOnly:   return ConfirmationLevel::None;
    case ActionRisk::Motion:     return ConfirmationLevel::Confirm;
    case ActionRisk::Heater:     return ConfirmationLevel::Confirm;
    case ActionRisk::JobControl: return ConfirmationLevel::Confirm;
    // Stopping the wrong machine mid-print is expensive, so the user must
    // name the printer rather than click through a dialog.
    case ActionRisk::Emergency:  return ConfirmationLevel::ConfirmTyped;
    }
    return ConfirmationLevel::Confirm;
}

} // namespace

bool is_offerable_quick_action(const QuickAction &action)
{
    if (action.id.empty() || action.label.empty())
        return false;
    // Anything that touches the machine must name the capability it needs.
    // This is what stops a raw "send this G-code" field being presented as a
    // one-click favourite.
    if (action.risk != ActionRisk::ReadOnly && action.required_capability.empty())
        return false;
    return true;
}

ActionDecision evaluate_quick_action(const QuickAction &action, const ActionContext &context)
{
    ActionDecision decision;
    decision.audit_entry = "action=" + action.id + " printer=" + context.printer_name +
                           " state=" + context.printer_state;

    if (!is_offerable_quick_action(action)) {
        decision.verdict = ActionVerdict::Refused;
        decision.reason  = "This action is not safe to offer as a quick action.";
        return decision;
    }

    if (!context.connected) {
        decision.verdict = ActionVerdict::Refused;
        decision.reason  = "The printer is not connected.";
        return decision;
    }

    if (action.risk != ActionRisk::ReadOnly && !context.user_has_control_permission) {
        decision.verdict = ActionVerdict::Refused;
        decision.reason  = "You do not have permission to control this printer.";
        return decision;
    }

    if (!action.required_capability.empty() &&
        std::find(context.capabilities.begin(), context.capabilities.end(),
                  action.required_capability) == context.capabilities.end()) {
        decision.verdict = ActionVerdict::Refused;
        decision.reason  = "This printer does not support that action.";
        return decision;
    }

    if (!action.allowed_states.empty() &&
        std::find(action.allowed_states.begin(), action.allowed_states.end(),
                  context.printer_state) == action.allowed_states.end()) {
        decision.verdict = ActionVerdict::Refused;
        decision.reason  = "That action is not available while the printer is " +
                           context.printer_state + ".";
        return decision;
    }

    decision.confirmation = confirmation_for(action.risk);
    decision.verdict = decision.confirmation == ConfirmationLevel::None
                           ? ActionVerdict::Allowed
                           : ActionVerdict::NeedsConfirmation;
    return decision;
}

} // namespace Slic3r
