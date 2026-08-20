///|/ TigerSlicer Phase 25D: scheduling semantics and quality for the
///|/ experimental bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BatchSemantics_hpp_
#define slic3r_BatchSemantics_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "BoundedBatchScheduler.hpp"
#include "DetailDecomposition.hpp"
#include "RecoveryCheckpoints.hpp"

namespace Slic3r {

// What batching did to the meaning of the print, as opposed to its shape.
enum class SemanticVerdict : uint8_t
{
    Sound,
    // Printable, but something about it is worse than the ordinary schedule.
    Degraded,
    // A rule that protects the part was broken. Never exported.
    Unsound,
    // Something needed to judge it was never measured. Also never exported:
    // "not shown to be wrong" is not "shown to be right".
    Unproven,
};

const char *semantic_verdict_name(SemanticVerdict verdict);

enum class SemanticSeverity : uint8_t
{
    Info,
    Warning,
    Blocking,
};

const char *semantic_severity_name(SemanticSeverity severity);

// Severity only ever escalates.
SemanticSeverity worse_of(SemanticSeverity a, SemanticSeverity b);

enum class SemanticIssue : uint8_t
{
    Chronology,
    Prerequisite,
    InterfacePlane,
    SealedAccess,
    Bonding,
    Ooze,
    ReprimeCost,
    Estimation,
    Checkpoint,
    CancelObject,
};

const char *semantic_issue_name(SemanticIssue issue);

struct SemanticFinding
{
    SemanticIssue    issue { SemanticIssue::Chronology };
    SemanticSeverity severity { SemanticSeverity::Info };

    // The scheduled operation the finding attaches to, where there is one.
    std::optional<size_t> operation;
    std::optional<size_t> component;

    // True when the finding exists because something was never measured,
    // rather than because something was measured and found wrong.
    bool unmeasured { false };

    std::string message;
};

// One layer of one component that has to exist before another can be printed.
// This is what stops a batch from advancing walls, infill, surfaces or
// supports ahead of what carries them.
struct LayerPrerequisite
{
    size_t component { 0 };
    size_t layer { 0 };
    // Why, in the words a reader would want back out of a refusal.
    std::string what;
};

// A place a finer tool has to reach into before a coarser layer closes over
// it. Phase 19 already knows where these are; this phase only checks that the
// schedule does not seal one.
struct AccessRequirement
{
    size_t      component { 0 };
    int         fine_tool { 0 };
    std::string what;
    // The height above which the access is sealed. Absent means nobody
    // established it, and the schedule is then unproven rather than fine.
    std::optional<double> sealed_above_z_mm;
};

// What one component needs from the schedule beyond its own layer order.
struct ComponentSemantics
{
    size_t      id { 0 };
    std::string material;

    // Every prerequisite in another component, keyed by this component's
    // layer index.
    std::vector<std::pair<size_t, LayerPrerequisite>> prerequisites;

    // How long a surface may sit before the next layer no longer bonds to it
    // properly. Absent means it was never characterized.
    std::optional<double> bond_window_s;

    // Metadata that makes an object cancellable mid-print. An unnamed
    // cancellable object is metadata that cannot be acted on.
    bool        cancellable { false };
    std::string cancel_object_name;
};

struct SemanticPolicy
{
    // How long a tool may sit hot and idle before it oozes. Absent means it
    // was never characterized for this machine.
    std::optional<double> ooze_window_s;

    // What a toolchange and a reprime cost. The toolchange figure has to be
    // the one the scheduler priced with, because the schedule's own reported
    // total is reconstructed from the operations at this price and a
    // disagreement is reported as an estimate that does not describe the
    // print.
    double toolchange_cost_s { 25.0 };
    double reprime_cost_s { 8.0 };

    // Tolerance on z comparisons and on the time reconciliation.
    double tolerance_mm { 1e-6 };
    double tolerance_s { 1e-3 };

    // Checkpointing at batch boundaries is opt-in, exactly as in Phase 13.
    bool                 checkpoint_batch_boundaries { false };
    RecoveryOptions      recovery_options;
    RecoveryCapabilities recovery_capabilities;
};

// Where one component actually got to, walked in schedule order rather than in
// global z order. This is the whole point: with a non-monotonic global Z, the
// only correct chronology is the order the operations are emitted in.
struct ComponentTimeline
{
    size_t component { 0 };
    int    tool { 0 };

    size_t layers_printed { 0 };
    double elapsed_s { 0. };
    // When this component was last touched, on the schedule's clock.
    double last_touched_s { 0. };
    double last_z_mm { 0. };

    // How long it sat untouched, worst case, and where that happened.
    double                max_idle_s { 0. };
    std::optional<size_t> max_idle_before_operation;

    // How many times the schedule came back to it after leaving.
    size_t returns { 0 };
};

struct ScheduleEstimate
{
    double total_time_s { 0. };
    double toolchange_time_s { 0. };
    double reprime_time_s { 0. };

    size_t toolchanges { 0 };
    size_t returns { 0 };

    // The highest z reached, and whether the schedule ever went back below it.
    double peak_z_mm { 0. };
    bool   non_monotonic_z { false };

    // The tool actually held at the end. Absent for an empty schedule: no
    // operation means no active tool, not tool zero.
    std::optional<int> final_tool;

    std::vector<ComponentTimeline> components;
};

// A cancellable object's operations, as they actually fall in the schedule.
// Batching splits an object into several runs; metadata that names only the
// first run cannot cancel the object.
struct CancelObjectRegion
{
    size_t      component { 0 };
    std::string name;

    // Every contiguous run of operations belonging to this object, as
    // [first, last] indices into the schedule.
    std::vector<std::pair<size_t, size_t>> runs;

    bool contiguous { false };
    // False when the object is cancellable but the metadata could not be made
    // to describe it, with the reason in the findings.
    bool actionable { false };
};

struct SemanticReview
{
    SemanticVerdict verdict { SemanticVerdict::Unproven };

    ScheduleEstimate                estimate;
    std::vector<CancelObjectRegion> cancel_objects;

    // Present only when checkpointing was asked for.
    std::optional<RecoveryPlan> checkpoints;

    std::vector<SemanticFinding> findings;
    std::vector<std::string>     notes;

    // True when the schedule may be emitted: sound, or degraded and no worse.
    bool emittable() const;

    size_t count(SemanticSeverity severity) const;
};

// Walk a batched schedule in emission order and accumulate what it actually
// does.
//
// Time, material, pressure and the active tool are all accumulated in schedule
// order, never in z order, because a bounded-Z batch schedule visits z
// non-monotonically and z order is then a different print from the one that
// will be run.
ScheduleEstimate estimate_schedule(const BatchPlan             &plan,
                                   const std::vector<ComponentSemantics> &components,
                                   const SemanticPolicy        &policy);

// The batch boundaries a recovery checkpoint would go on, as Phase 13
// operations. A boundary mid-batch is not offered: the machine is only left
// where a component has just finished a run.
std::vector<RecoverableOperation> batch_boundary_operations(
    const BatchPlan &plan, const std::vector<ComponentSemantics> &components);

// Check what batching did to the meaning of the print.
//
// Every component keeps its own chronology; nothing is advanced ahead of what
// carries it; cross-tool interface planes are reached by both sides before
// either goes past them; a coarse layer never seals an access a finer tool
// still needs; a component that sat cooling while others printed is reported;
// idle long enough to ooze, or to cost more in repriming than the schedule
// saved in toolchanges, is reported against the saving it claimed; and a
// cancellable object split across batches is only called cancellable when the
// metadata describes every run of it.
//
// Anything that could not be measured leaves the review Unproven. That is a
// separate answer from Unsound, and neither is emittable.
SemanticReview review_batch_semantics(const BatchPlan                         &plan,
                                      const std::vector<ComponentSemantics>   &components,
                                      const std::vector<InterfaceRequirement> &interfaces,
                                      const std::vector<AccessRequirement>    &access,
                                      const SemanticPolicy                    &policy);

} // namespace Slic3r

#endif // slic3r_BatchSemantics_hpp_
