///|/ TigerSlicer Phase 13: recoverable toolchanger G-code.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_RecoveryCheckpoints_hpp_
#define slic3r_RecoveryCheckpoints_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// What went wrong. The kind matters because different faults invalidate
// different parts of the recorded state: a heater fault leaves the position
// trustworthy but the temperatures wrong, while a failed pickup leaves the
// machine holding something unknown.
enum class FaultKind : uint8_t
{
    PickupFailure,
    ParkingFailure,
    HeaterFault,
    FilamentRunout,
    CleaningFailure,
    UserPause,
};

const char *fault_kind_name(FaultKind kind);

// Whether the melt zone is known to be under pressure. Unknown is a distinct
// state on purpose: an unrecorded pressure must force a reprime rather than
// be treated as relieved.
enum class PressureState : uint8_t
{
    Relieved,
    Pressurized,
    Unknown,
};

const char *pressure_state_name(PressureState state);

// A tool's temperature at a moment in the print. Both values are optional
// because "nobody recorded it" is not the same as "it was cold".
struct ToolTemperature
{
    int                   tool { 0 };
    std::optional<double> actual_c;
    std::optional<double> target_c;
};

// One unit of printing work, as far as recovery is concerned.
struct RecoverableOperation
{
    size_t index { 0 };
    int    tool { 0 };

    double print_z { 0. };
    bool   on_synchronization_plane { false };

    // False while a multi-step sequence is mid-flight - a toolchange between
    // release and latch, a purge that has started but not finished, a cut.
    // Stopping there leaves the machine in a state no checkpoint describes.
    bool        safe_to_interrupt_after { true };
    std::string unsafe_reason;

    double elapsed_s { 0. };

    // Absolute extruder position and pressure state at the END of this
    // operation.
    double        extruder_position_mm { 0. };
    PressureState pressure { PressureState::Unknown };

    // Temperatures at the end of this operation, for every tool that has one
    // recorded. A tool absent from this list has no recorded temperature.
    std::vector<ToolTemperature> temperatures;
};

// What the firmware has declared it can do. As everywhere else in
// TigerSlicer, a capability flag without a configured hook is disabled: the
// requirement is reported as a limitation and nothing is fabricated.
struct RecoveryCapabilities
{
    bool can_record_checkpoint { false };
    bool can_pause { false };
    // Can the firmware establish which tool is actually held, independently of
    // what the slicer believed?
    bool can_verify_tool_state { false };
    bool can_restore_temperature { false };
    bool can_reprime { false };

    // Tokens substituted where present: [operation] [tool] [z] [e] [elapsed]
    // [temperature].
    std::string checkpoint_gcode;
    std::string pause_gcode;
    std::string verify_tool_gcode;
    std::string restore_temperature_gcode;
    std::string reprime_gcode;
    std::string resume_gcode;
};

struct RecoveryOptions
{
    // Checkpointing is opt-in; the spec calls these optional safe points.
    bool enabled { false };

    // Do not emit checkpoints closer together than this, in print seconds. A
    // checkpoint per operation would cost more than it saves.
    double min_interval_s { 0. };

    // Only checkpoint where every tool's grid meets, so a resumed print cannot
    // land between two tools' layers.
    bool synchronization_planes_only { false };
};

enum class RecoverySeverity : uint8_t
{
    Info,
    Warning,
    Blocked,
};

const char *recovery_severity_name(RecoverySeverity severity);

struct RecoveryNote
{
    RecoverySeverity severity { RecoverySeverity::Info };
    std::string      message;
};

// Everything needed to restart the print at a boundary between operations.
struct RecoveryCheckpoint
{
    size_t after_operation { 0 };

    int                active_tool { 0 };
    std::optional<int> expected_next_tool;

    double print_z { 0. };
    bool   on_synchronization_plane { false };
    double elapsed_s { 0. };

    double        extruder_position_mm { 0. };
    PressureState pressure { PressureState::Unknown };

    std::vector<ToolTemperature> temperatures;

    bool                     requires_reprime { false };
    std::vector<std::string> reprime_reasons;

    // The command actually written, from the declared hook. Empty when the
    // firmware declared no usable hook: the checkpoint is still planned and
    // reported, but nothing is invented.
    std::string gcode;
    bool        emitted { false };
};

struct RecoveryPlan
{
    std::vector<RecoveryCheckpoint> checkpoints;
    std::vector<RecoveryNote>       notes;

    // Capabilities the plan needed and the firmware never declared.
    std::vector<std::string> limitations;

    // Non-empty when no plan could be produced at all.
    std::string error;

    bool has_blocking_note() const;
};

// Place safe checkpoints through a print.
//
// A checkpoint is only ever placed on a boundary the machine can actually be
// left at: after an operation that declares itself safe to interrupt. Unsafe
// operations are skipped over, and the reason travels into the plan so the
// gap is explained rather than merely absent.
RecoveryPlan plan_recovery_checkpoints(const std::vector<RecoverableOperation> &operations,
                                       const RecoveryOptions                   &options,
                                       const RecoveryCapabilities              &capabilities);

enum class ResumeVerdict : uint8_t
{
    // Restart from a recorded checkpoint, redoing everything after it.
    ResumeFromCheckpoint,
    // No honest resume exists. The print is not silently continued.
    CannotResume,
};

const char *resume_verdict_name(ResumeVerdict verdict);

struct ResumeDecision
{
    ResumeVerdict verdict { ResumeVerdict::CannotResume };

    // Index into RecoveryPlan::checkpoints, set only when resuming.
    std::optional<size_t> checkpoint;
    // The first operation that will be re-run.
    std::optional<size_t> restart_at_operation;

    bool requires_reprime { false };
    bool requires_reheat { false };
    bool requires_tool_verification { false };

    // Ordered commands, from declared hooks only.
    std::vector<std::string> actions;
    std::vector<std::string> reasons;
};

// Decide how - or whether - to resume after a fault during `interrupted_operation`.
//
// The interrupted operation is never resumed in place; recovery always winds
// back to the last checkpoint before it, so a partially completed operation is
// redone from a state that was actually recorded. When the fault invalidates
// something the checkpoint cannot restore, the result is CannotResume with the
// reason, not a best-effort continuation.
ResumeDecision plan_resume(const RecoveryPlan                      &plan,
                           const std::vector<RecoverableOperation> &operations,
                           size_t                                   interrupted_operation,
                           FaultKind                                fault,
                           const RecoveryCapabilities              &capabilities);

} // namespace Slic3r

#endif // slic3r_RecoveryCheckpoints_hpp_
