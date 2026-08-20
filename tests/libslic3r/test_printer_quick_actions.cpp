// Phase N: quick actions. These buttons move heaters and axes on real
// hardware, so the tests exist to pin down that risk drives confirmation, that
// capability and state gate execution, and that an arbitrary command can never
// be offered as a one-click favourite.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrinterQuickActions.hpp"

using namespace Slic3r;

namespace {

QuickAction action(const std::string &id, ActionRisk risk, const std::string &capability,
                   std::vector<std::string> states = {})
{
    QuickAction item;
    item.id                  = id;
    item.label               = id;
    item.risk                = risk;
    item.required_capability = capability;
    item.allowed_states      = std::move(states);
    return item;
}

ActionContext context(const std::string &state = "idle",
                      std::vector<std::string> capabilities = { "home", "preheat", "estop", "web" })
{
    ActionContext ctx;
    ctx.printer_name = "Reference toolchanger";
    ctx.printer_state = state;
    ctx.capabilities  = std::move(capabilities);
    return ctx;
}

} // namespace

TEST_CASE("Read-only actions run without confirmation", "[quick-actions]")
{
    const ActionDecision decision =
        evaluate_quick_action(action("open_web", ActionRisk::ReadOnly, "web"), context());

    CHECK(decision.verdict == ActionVerdict::Allowed);
    CHECK(decision.confirmation == ConfirmationLevel::None);
}

TEST_CASE("Motion and heater actions require confirmation", "[quick-actions]")
{
    const ActionDecision motion =
        evaluate_quick_action(action("home", ActionRisk::Motion, "home"), context());
    CHECK(motion.verdict == ActionVerdict::NeedsConfirmation);
    CHECK(motion.confirmation == ConfirmationLevel::Confirm);

    const ActionDecision heater =
        evaluate_quick_action(action("preheat", ActionRisk::Heater, "preheat"), context());
    CHECK(heater.verdict == ActionVerdict::NeedsConfirmation);
}

TEST_CASE("Emergency stop demands the strongest confirmation", "[quick-actions]")
{
    const ActionDecision decision =
        evaluate_quick_action(action("estop", ActionRisk::Emergency, "estop"), context());

    CHECK(decision.verdict == ActionVerdict::NeedsConfirmation);
    // Stopping the wrong machine mid-print is costly enough to warrant naming it.
    CHECK(decision.confirmation == ConfirmationLevel::ConfirmTyped);
}

TEST_CASE("An unsupported capability refuses with a reason", "[quick-actions]")
{
    const ActionDecision decision = evaluate_quick_action(
        action("home", ActionRisk::Motion, "home"), context("idle", { "web" }));

    CHECK(decision.verdict == ActionVerdict::Refused);
    CHECK_FALSE(decision.reason.empty());
}

TEST_CASE("Actions are gated by printer state", "[quick-actions]")
{
    const QuickAction pause = action("pause", ActionRisk::JobControl, "pause", { "printing" });
    const std::vector<std::string> capabilities { "home", "preheat", "estop", "web", "pause" };

    CHECK(evaluate_quick_action(pause, context("printing", capabilities)).verdict ==
          ActionVerdict::NeedsConfirmation);
    // Pausing an idle printer is meaningless and must be refused, not silently
    // accepted.
    CHECK(evaluate_quick_action(pause, context("idle", capabilities)).verdict ==
          ActionVerdict::Refused);
}

TEST_CASE("A disconnected printer refuses every action", "[quick-actions]")
{
    ActionContext offline = context();
    offline.connected = false;

    CHECK(evaluate_quick_action(action("open_web", ActionRisk::ReadOnly, "web"), offline).verdict ==
          ActionVerdict::Refused);
}

TEST_CASE("Control actions require control permission", "[quick-actions]")
{
    ActionContext restricted = context();
    restricted.user_has_control_permission = false;

    CHECK(evaluate_quick_action(action("home", ActionRisk::Motion, "home"), restricted).verdict ==
          ActionVerdict::Refused);
    // Read-only navigation stays available.
    CHECK(evaluate_quick_action(action("open_web", ActionRisk::ReadOnly, "web"), restricted).verdict ==
          ActionVerdict::Allowed);
}

TEST_CASE("An action that touches the machine without declaring a capability is not offerable",
          "[quick-actions]")
{
    // This is what an unsanitized "run arbitrary command" field would look like.
    const QuickAction raw = action("send_raw", ActionRisk::Motion, "");
    CHECK_FALSE(is_offerable_quick_action(raw));
    CHECK(evaluate_quick_action(raw, context()).verdict == ActionVerdict::Refused);
}

TEST_CASE("Every decision produces an audit entry naming the target", "[quick-actions]")
{
    const ActionDecision decision =
        evaluate_quick_action(action("home", ActionRisk::Motion, "home"), context());

    CHECK(decision.audit_entry.find("home") != std::string::npos);
    CHECK(decision.audit_entry.find("Reference toolchanger") != std::string::npos);
}
