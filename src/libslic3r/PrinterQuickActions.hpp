///|/ TigerSlicer Phase N: capability-gated printer quick actions.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterQuickActions_hpp_
#define slic3r_PrinterQuickActions_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// What an action can do to the machine or the print. Risk drives how much
// confirmation is required, so it is a property of the action, not of the UI
// that happens to invoke it.
enum class ActionRisk
{
    ReadOnly,   // navigation/status only; safe to run immediately
    Motion,     // moves an axis or tool
    Heater,     // energises a heater
    JobControl, // pauses, resumes or cancels a running print
    Emergency,  // emergency stop
};

enum class ConfirmationLevel
{
    None,
    Confirm,        // a simple yes/no naming the printer
    ConfirmTyped,   // must re-enter the printer's name
};

struct QuickAction
{
    std::string id;
    std::string label;
    ActionRisk  risk { ActionRisk::ReadOnly };
    // Connector capability this action needs, e.g. "home", "preheat".
    std::string required_capability;
    // Printer states in which the action makes sense. Empty means any state.
    std::vector<std::string> allowed_states;
};

// Everything known about the target at the moment the user asks.
struct ActionContext
{
    std::string printer_name;
    std::string printer_state;                  // "idle", "printing", ...
    std::vector<std::string> capabilities;
    bool        connected { true };
    bool        user_has_control_permission { true };
};

enum class ActionVerdict
{
    Allowed,
    NeedsConfirmation,
    Refused,
};

struct ActionDecision
{
    ActionVerdict     verdict { ActionVerdict::Refused };
    ConfirmationLevel confirmation { ConfirmationLevel::None };
    std::string       reason;
    // Text the audit log should record. Never contains credentials.
    std::string       audit_entry;
};

// Decide whether an action may run, and how much confirmation it needs.
//
// This function never performs the action. Refusals are explained so the user
// is not left guessing why a button did nothing.
ActionDecision evaluate_quick_action(const QuickAction &action, const ActionContext &context);

// Reject an arbitrary free-text command being smuggled in as a "quick action".
// Only actions with a declared capability and known risk may be offered.
bool is_offerable_quick_action(const QuickAction &action);

} // namespace Slic3r

#endif // slic3r_PrinterQuickActions_hpp_
