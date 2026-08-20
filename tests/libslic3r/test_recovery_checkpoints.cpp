// Phase 13: recoverable toolchanger G-code. The properties under test are all
// refusals: a checkpoint is never placed where the machine cannot be left, a
// partially completed unsafe operation is never resumed in place, unrecorded
// state never reads as safe state, and no command is emitted for a hook the
// firmware never declared.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/RecoveryCheckpoints.hpp"

using namespace Slic3r;

namespace {

RecoverableOperation op(size_t index, int tool, double z, double elapsed_s)
{
    RecoverableOperation value;
    value.index                   = index;
    value.tool                    = tool;
    value.print_z                 = z;
    value.elapsed_s               = elapsed_s;
    value.extruder_position_mm    = 10. * double(index);
    value.pressure                = PressureState::Relieved;
    value.safe_to_interrupt_after = true;
    value.on_synchronization_plane = false;
    ToolTemperature hot;
    hot.tool     = tool;
    hot.actual_c = 240.;
    hot.target_c = 240.;
    value.temperatures.push_back(hot);
    return value;
}

std::vector<RecoverableOperation> print()
{
    std::vector<RecoverableOperation> operations {
        op(0, 0, 0.2, 0.),
        op(1, 0, 0.2, 100.),
        op(2, 1, 0.2, 200.),
        op(3, 0, 0.4, 300.),
    };
    operations[1].on_synchronization_plane = true;
    operations[3].on_synchronization_plane = true;
    return operations;
}

RecoveryCapabilities full_capabilities()
{
    RecoveryCapabilities capabilities;
    capabilities.can_record_checkpoint   = true;
    capabilities.can_pause               = true;
    capabilities.can_verify_tool_state   = true;
    capabilities.can_restore_temperature = true;
    capabilities.can_reprime             = true;
    capabilities.checkpoint_gcode        = "SAVE_TS_CHECKPOINT OP=[operation] T=[tool] Z=[z] E=[e]";
    capabilities.verify_tool_gcode       = "VERIFY_TOOL EXPECT=[tool]";
    capabilities.restore_temperature_gcode = "M104 T[tool] S[temperature]";
    capabilities.reprime_gcode             = "TS_REPRIME TOOL=[tool]";
    capabilities.resume_gcode              = "TS_RESUME Z=[z] E=[e]";
    return capabilities;
}

RecoveryOptions enabled()
{
    RecoveryOptions options;
    options.enabled = true;
    return options;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Checkpointing is off unless it is asked for", "[recovery-checkpoints]")
{
    const RecoveryPlan plan =
        plan_recovery_checkpoints(print(), RecoveryOptions {}, full_capabilities());
    CHECK(plan.checkpoints.empty());
    CHECK(plan.error.empty());
    REQUIRE(plan.notes.size() == 1);
    CHECK(plan.notes.front().message.find("disabled") != std::string::npos);
}

TEST_CASE("Every safe boundary gets a checkpoint carrying the recorded state",
          "[recovery-checkpoints]")
{
    const RecoveryPlan plan =
        plan_recovery_checkpoints(print(), enabled(), full_capabilities());

    REQUIRE(plan.checkpoints.size() == 4);
    const RecoveryCheckpoint &second = plan.checkpoints[1];
    CHECK(second.after_operation == 1);
    CHECK(second.active_tool == 0);
    // The tool the print is about to need travels with the checkpoint.
    REQUIRE(second.expected_next_tool.has_value());
    CHECK(*second.expected_next_tool == 1);
    CHECK(second.on_synchronization_plane);
    CHECK_FALSE(second.requires_reprime);
    CHECK(second.emitted);
    CHECK(second.gcode == "SAVE_TS_CHECKPOINT OP=1 T=0 Z=0.200 E=10.00000");
}

TEST_CASE("An operation that cannot be interrupted safely gets no checkpoint",
          "[recovery-checkpoints]")
{
    std::vector<RecoverableOperation> operations = print();
    operations[2].safe_to_interrupt_after        = false;
    operations[2].unsafe_reason = "the toolchange has released T0 but not latched T1";

    const RecoveryPlan plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    REQUIRE(plan.checkpoints.size() == 3);
    for (const RecoveryCheckpoint &checkpoint : plan.checkpoints)
        CHECK(checkpoint.after_operation != 2);

    // The gap is explained, not merely absent.
    std::vector<std::string> messages;
    for (const RecoveryNote &note : plan.notes)
        messages.push_back(note.message);
    CHECK(mentions(messages, "not latched T1"));
}

TEST_CASE("A print with no safe boundary at all is blocked, not silently empty",
          "[recovery-checkpoints]")
{
    std::vector<RecoverableOperation> operations = print();
    for (RecoverableOperation &operation : operations)
        operation.safe_to_interrupt_after = false;

    const RecoveryPlan plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());
    CHECK(plan.checkpoints.empty());
    CHECK(plan.has_blocking_note());
}

TEST_CASE("Spacing and synchronization rules thin the checkpoints out",
          "[recovery-checkpoints]")
{
    SECTION("minimum interval") {
        RecoveryOptions options = enabled();
        options.min_interval_s  = 150.;
        const RecoveryPlan plan =
            plan_recovery_checkpoints(print(), options, full_capabilities());
        // Spacing runs from the last checkpoint actually placed, not from the
        // previous operation, so 100 s apart at a 150 s interval keeps every
        // other one.
        REQUIRE(plan.checkpoints.size() == 2);
        CHECK(plan.checkpoints[0].after_operation == 0);
        CHECK(plan.checkpoints[1].after_operation == 2);
    }

    SECTION("synchronization planes only") {
        RecoveryOptions options            = enabled();
        options.synchronization_planes_only = true;
        const RecoveryPlan plan =
            plan_recovery_checkpoints(print(), options, full_capabilities());
        REQUIRE(plan.checkpoints.size() == 2);
        CHECK(plan.checkpoints[0].after_operation == 1);
        CHECK(plan.checkpoints[1].after_operation == 3);
    }
}

TEST_CASE("An unrecorded pressure state forces a reprime", "[recovery-checkpoints]")
{
    std::vector<RecoverableOperation> operations = print();
    operations[0].pressure                       = PressureState::Unknown;
    operations[1].pressure                       = PressureState::Pressurized;

    const RecoveryPlan plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    REQUIRE(plan.checkpoints.size() == 4);
    CHECK(plan.checkpoints[0].requires_reprime);
    CHECK(mentions(plan.checkpoints[0].reprime_reasons, "cannot be assumed relieved"));
    CHECK(plan.checkpoints[1].requires_reprime);
    CHECK(mentions(plan.checkpoints[1].reprime_reasons, "left pressurized"));
    CHECK_FALSE(plan.checkpoints[2].requires_reprime);
}

TEST_CASE("A declared capability without a configured hook emits nothing",
          "[recovery-checkpoints]")
{
    RecoveryCapabilities capabilities = full_capabilities();
    capabilities.checkpoint_gcode.clear();

    const RecoveryPlan plan = plan_recovery_checkpoints(print(), enabled(), capabilities);
    // The checkpoints are still planned and reported; only the command is absent.
    REQUIRE(plan.checkpoints.size() == 4);
    for (const RecoveryCheckpoint &checkpoint : plan.checkpoints) {
        CHECK_FALSE(checkpoint.emitted);
        CHECK(checkpoint.gcode.empty());
    }
    CHECK(mentions(plan.limitations, "no checkpoint G-code is configured"));
}

TEST_CASE("Resuming winds back to the last checkpoint before the fault",
          "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    const ResumeDecision decision = plan_resume(plan, operations, 3, FaultKind::UserPause,
                                                full_capabilities());

    REQUIRE(decision.verdict == ResumeVerdict::ResumeFromCheckpoint);
    REQUIRE(decision.checkpoint.has_value());
    CHECK(plan.checkpoints[*decision.checkpoint].after_operation == 2);
    // The interrupted operation is redone, never continued from the middle.
    REQUIRE(decision.restart_at_operation.has_value());
    CHECK(*decision.restart_at_operation == 3);
    CHECK(decision.actions.back() == "TS_RESUME Z=0.200 E=20.00000");
}

TEST_CASE("A fault before any checkpoint cannot be resumed", "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    const ResumeDecision decision = plan_resume(plan, operations, 0, FaultKind::UserPause,
                                                full_capabilities());
    CHECK(decision.verdict == ResumeVerdict::CannotResume);
    CHECK_FALSE(decision.checkpoint.has_value());
    CHECK(mentions(decision.reasons, "no state to resume from"));
}

TEST_CASE("An unsafe operation between the checkpoint and the fault is redone in full",
          "[recovery-checkpoints]")
{
    std::vector<RecoverableOperation> operations = print();
    operations[2].safe_to_interrupt_after        = false;
    operations[2].unsafe_reason                  = "purge in progress";

    const RecoveryPlan plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());
    const ResumeDecision decision = plan_resume(plan, operations, 3, FaultKind::UserPause,
                                                full_capabilities());

    REQUIRE(decision.verdict == ResumeVerdict::ResumeFromCheckpoint);
    CHECK(plan.checkpoints[*decision.checkpoint].after_operation == 1);
    CHECK(*decision.restart_at_operation == 2);
    CHECK(mentions(decision.reasons, "redone in full"));
}

TEST_CASE("A pickup failure will not resume unless the held tool can be verified",
          "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    SECTION("the firmware can verify") {
        const ResumeDecision decision = plan_resume(plan, operations, 2,
                                                    FaultKind::PickupFailure,
                                                    full_capabilities());
        REQUIRE(decision.verdict == ResumeVerdict::ResumeFromCheckpoint);
        CHECK(decision.requires_tool_verification);
        CHECK(decision.actions.front() == "VERIFY_TOOL EXPECT=0");
    }

    SECTION("the firmware cannot verify") {
        RecoveryCapabilities capabilities = full_capabilities();
        capabilities.can_verify_tool_state = false;
        const ResumeDecision decision =
            plan_resume(plan, operations, 2, FaultKind::PickupFailure, capabilities);
        CHECK(decision.verdict == ResumeVerdict::CannotResume);
        CHECK(mentions(decision.reasons, "the tool actually held is unknown"));
    }
}

TEST_CASE("A runout and a failed clean both force a reprime", "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    for (FaultKind fault : { FaultKind::FilamentRunout, FaultKind::CleaningFailure }) {
        const ResumeDecision decision =
            plan_resume(plan, operations, 3, fault, full_capabilities());
        REQUIRE(decision.verdict == ResumeVerdict::ResumeFromCheckpoint);
        CHECK(decision.requires_reprime);
        CHECK(mentions(decision.actions, "TS_REPRIME TOOL=1"));
    }
}

TEST_CASE("A reprime that the firmware cannot perform stops the resume",
          "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    RecoveryCapabilities capabilities = full_capabilities();
    capabilities.reprime_gcode.clear();

    const ResumeDecision decision =
        plan_resume(plan, operations, 3, FaultKind::FilamentRunout, capabilities);
    CHECK(decision.verdict == ResumeVerdict::CannotResume);
    CHECK(mentions(decision.reasons, "no usable reprime hook"));
}

TEST_CASE("Recorded temperatures are restored before anything extrudes",
          "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    const ResumeDecision decision =
        plan_resume(plan, operations, 3, FaultKind::HeaterFault, full_capabilities());
    REQUIRE(decision.verdict == ResumeVerdict::ResumeFromCheckpoint);
    CHECK(decision.requires_reheat);
    CHECK(mentions(decision.actions, "M104 T1 S240"));
    // Heating comes before the resume move.
    CHECK(decision.actions.back().find("TS_RESUME") != std::string::npos);
}

TEST_CASE("A checkpoint with no recorded target temperature cannot be resumed after a "
          "heater fault",
          "[recovery-checkpoints]")
{
    std::vector<RecoverableOperation> operations = print();
    for (RecoverableOperation &operation : operations)
        operation.temperatures.clear();

    const RecoveryPlan plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());
    const ResumeDecision decision =
        plan_resume(plan, operations, 3, FaultKind::HeaterFault, full_capabilities());

    CHECK(decision.verdict == ResumeVerdict::CannotResume);
    CHECK(mentions(decision.reasons, "no target temperature was recorded"));
}

TEST_CASE("Without a resume hook the print is not returned to position",
          "[recovery-checkpoints]")
{
    const std::vector<RecoverableOperation> operations = print();
    const RecoveryPlan                      plan =
        plan_recovery_checkpoints(operations, enabled(), full_capabilities());

    RecoveryCapabilities capabilities = full_capabilities();
    capabilities.resume_gcode.clear();

    const ResumeDecision decision =
        plan_resume(plan, operations, 3, FaultKind::UserPause, capabilities);
    CHECK(decision.verdict == ResumeVerdict::CannotResume);
    CHECK(mentions(decision.reasons, "no resume G-code is configured"));
}
