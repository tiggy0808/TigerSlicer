// Phase 25D: what bounded-Z batching does to the meaning of the print, as
// opposed to its shape. 25B decides the order and 25C proves the descents; the
// rules here are the ones that keep the reordered print the same print - every
// component keeps its own chronology, nothing is advanced ahead of what
// carries it, cross-tool interface planes are reached by both sides, a coarse
// layer never seals an access a finer tool still needs, and time is
// accumulated in emission order because global Z is no longer monotonic.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/BatchSemantics.hpp"

using namespace Slic3r;

namespace {

ScheduledOperation op(size_t component, size_t layer, int tool, double z_bottom,
                      double z_top, double duration, bool starts_batch = false)
{
    ScheduledOperation value;
    value.component    = component;
    value.layer        = layer;
    value.tool         = tool;
    value.z_bottom_mm  = z_bottom;
    value.z_top_mm     = z_top;
    value.duration_s   = duration;
    value.starts_batch = starts_batch;
    return value;
}

// Two independent towers, four layers each, batched two layers at a time: one
// tower leads, the other catches up. Component 0 is on T0, component 1 on T1.
BatchPlan two_towers()
{
    BatchPlan plan;
    plan.mode    = ScheduleMode::BoundedBatch;
    plan.verdict = BatchVerdict::Batched;

    plan.operations = {
        op(0, 0, 0, 0.0, 0.2, 30., true),
        op(0, 1, 0, 0.2, 0.4, 30.),
        op(1, 0, 1, 0.0, 0.2, 30., true),
        op(1, 1, 1, 0.2, 0.4, 30.),
        op(0, 2, 0, 0.4, 0.6, 30., true),
        op(0, 3, 0, 0.6, 0.8, 30.),
        op(1, 2, 1, 0.4, 0.6, 30., true),
        op(1, 3, 1, 0.6, 0.8, 30.),
    };
    // The lagging tower drops back to 0.2 mm and again to 0.6 mm.
    plan.operations[2].downward_move = true;
    plan.operations[6].downward_move = true;

    plan.metrics.total_time_s   = 315.;
    plan.metrics.toolchanges    = 3;
    plan.metrics.downward_moves = 2;
    plan.legacy_metrics.toolchanges = 7;
    plan.toolchanges_saved          = 4;
    return plan;
}

std::vector<ComponentSemantics> two_component_semantics()
{
    ComponentSemantics a;
    a.id                 = 0;
    a.material           = "PLA";
    a.bond_window_s      = 600.;
    a.cancellable        = true;
    a.cancel_object_name = "left tower";

    ComponentSemantics b = a;
    b.id                 = 1;
    b.cancel_object_name = "right tower";

    return { a, b };
}

SemanticPolicy policy()
{
    SemanticPolicy value;
    value.ooze_window_s     = 300.;
    value.toolchange_cost_s = 25.;
    value.reprime_cost_s    = 8.;
    return value;
}

bool has(const SemanticReview &review, SemanticIssue issue, SemanticSeverity severity)
{
    return std::any_of(review.findings.begin(), review.findings.end(),
                       [&](const SemanticFinding &finding) {
                           return finding.issue == issue && finding.severity == severity;
                       });
}

bool mentions(const SemanticReview &review, const std::string &fragment)
{
    return std::any_of(review.findings.begin(), review.findings.end(),
                       [&](const SemanticFinding &finding) {
                           return finding.message.find(fragment) != std::string::npos;
                       });
}

} // namespace

TEST_CASE("A well formed batch schedule keeps the meaning of the print",
          "[batch-semantics]")
{
    const SemanticReview review = review_batch_semantics(two_towers(),
                                                         two_component_semantics(), {}, {},
                                                         policy());

    CHECK(review.verdict == SemanticVerdict::Sound);
    CHECK(review.emittable());
    CHECK(review.count(SemanticSeverity::Blocking) == 0);
    CHECK(review.count(SemanticSeverity::Warning) == 0);
}

TEST_CASE("Every component keeps its own chronological state", "[batch-semantics]")
{
    const SemanticReview review = review_batch_semantics(two_towers(),
                                                         two_component_semantics(), {}, {},
                                                         policy());

    REQUIRE(review.estimate.components.size() == 2);
    for (const ComponentTimeline &timeline : review.estimate.components) {
        CHECK(timeline.layers_printed == 4);
        CHECK(timeline.elapsed_s == 120.);
        CHECK(timeline.returns == 1);
    }
    // Left tower first, right tower second: the order they were first touched.
    CHECK(review.estimate.components[0].component == 0);
    CHECK(review.estimate.components[1].component == 1);
    CHECK(review.estimate.components[0].max_idle_s > 0.);
    REQUIRE(review.estimate.components[0].max_idle_before_operation.has_value());
    CHECK(*review.estimate.components[0].max_idle_before_operation == 4);
}

TEST_CASE("A component reordered against itself is refused", "[batch-semantics]")
{
    BatchPlan plan = two_towers();
    std::swap(plan.operations[0], plan.operations[1]);

    const SemanticReview review = review_batch_semantics(plan, two_component_semantics(), {},
                                                         {}, policy());

    CHECK(review.verdict == SemanticVerdict::Unsound);
    CHECK_FALSE(review.emittable());
    CHECK(has(review, SemanticIssue::Chronology, SemanticSeverity::Blocking));
    CHECK(mentions(review, "never a component against itself"));
}

TEST_CASE("A layer advanced ahead of what carries it is refused", "[batch-semantics]")
{
    std::vector<ComponentSemantics> components = two_component_semantics();
    // The right tower's third layer bridges onto the left tower's second.
    LayerPrerequisite need;
    need.component = 0;
    need.layer     = 3;
    need.what      = "the bridge it lands on";
    components[1].prerequisites.push_back({ size_t(2), need });

    BatchPlan plan = two_towers();
    // Print the right tower's layer 2 before the left tower has got to layer 3.
    std::rotate(plan.operations.begin() + 4, plan.operations.begin() + 6,
                plan.operations.begin() + 8);

    const SemanticReview review = review_batch_semantics(plan, components, {}, {}, policy());

    CHECK(review.verdict == SemanticVerdict::Unsound);
    CHECK(has(review, SemanticIssue::Prerequisite, SemanticSeverity::Blocking));
    CHECK(mentions(review, "the bridge it lands on"));
    CHECK(mentions(review, "print into free air"));
}

TEST_CASE("Neither side may pass a cross-tool interface plane alone",
          "[batch-semantics]")
{
    InterfaceRequirement interface;
    interface.region_a = 0;
    interface.region_b = 1;
    interface.tool_a   = 0;
    interface.tool_b   = 1;
    interface.print_z  = 0.4;

    SECTION("both sides reach it before either passes it") {
        const SemanticReview review = review_batch_semantics(
            two_towers(), two_component_semantics(), { interface }, {}, policy());
        CHECK(review.verdict == SemanticVerdict::Sound);
    }

    SECTION("one side running away from the plane is refused") {
        BatchPlan plan = two_towers();
        // The left tower prints all four layers before the right tower starts.
        plan.operations = { op(0, 0, 0, 0.0, 0.2, 30., true), op(0, 1, 0, 0.2, 0.4, 30.),
                            op(0, 2, 0, 0.4, 0.6, 30.),       op(0, 3, 0, 0.6, 0.8, 30.),
                            op(1, 0, 1, 0.0, 0.2, 30., true), op(1, 1, 1, 0.2, 0.4, 30.),
                            op(1, 2, 1, 0.4, 0.6, 30.),       op(1, 3, 1, 0.6, 0.8, 30.) };
        plan.operations[4].downward_move = true;

        const SemanticReview review = review_batch_semantics(
            plan, two_component_semantics(), { interface }, {}, policy());

        CHECK(review.verdict == SemanticVerdict::Unsound);
        CHECK(has(review, SemanticIssue::InterfacePlane, SemanticSeverity::Blocking));
        CHECK(mentions(review, "both have to be there"));
    }
}

TEST_CASE("A coarse layer may not seal an access a finer tool still needs",
          "[batch-semantics]")
{
    AccessRequirement need;
    need.component         = 0;
    need.fine_tool         = 1;
    need.what              = "the embedded channel";
    need.sealed_above_z_mm = 0.4;

    SECTION("sealing it before the fine tool has been in is refused") {
        const SemanticReview review = review_batch_semantics(
            two_towers(), two_component_semantics(), {}, { need }, policy());

        CHECK(review.verdict == SemanticVerdict::Unsound);
        CHECK(has(review, SemanticIssue::SealedAccess, SemanticSeverity::Blocking));
        CHECK(mentions(review, "the embedded channel"));
        CHECK(mentions(review, "has not been in there yet"));
    }

    SECTION("an unestablished seal height is unproven, not fine") {
        need.sealed_above_z_mm.reset();
        const SemanticReview review = review_batch_semantics(
            two_towers(), two_component_semantics(), {}, { need }, policy());

        CHECK(review.verdict == SemanticVerdict::Unproven);
        CHECK_FALSE(review.emittable());
        CHECK(mentions(review, "was never established"));
    }

    SECTION("the fine tool having been in there first clears it") {
        BatchPlan plan = two_towers();
        // T1 does its work in the left tower before the coarse layers close.
        plan.operations.insert(plan.operations.begin() + 2,
                               op(0, 2, 1, 0.4, 0.4, 12., true));
        plan.operations[3].starts_batch = true;
        for (size_t i = 3; i < plan.operations.size(); ++i)
            if (plan.operations[i].component == 0 && plan.operations[i].tool == 0 &&
                plan.operations[i].layer >= 2)
                ++plan.operations[i].layer;
        plan.metrics.total_time_s += 12.;

        const SemanticReview review = review_batch_semantics(
            plan, two_component_semantics(), {}, { need }, policy());

        CHECK_FALSE(has(review, SemanticIssue::SealedAccess, SemanticSeverity::Blocking));
    }
}

TEST_CASE("A component that cooled past its bond window is reported",
          "[batch-semantics]")
{
    std::vector<ComponentSemantics> components = two_component_semantics();
    components[0].bond_window_s                = 30.;

    const SemanticReview review = review_batch_semantics(two_towers(), components, {}, {},
                                                         policy());

    CHECK(review.verdict == SemanticVerdict::Degraded);
    CHECK(review.emittable());
    CHECK(has(review, SemanticIssue::Bonding, SemanticSeverity::Warning));
    CHECK(mentions(review, "will not be as strong as it would be in the ordinary schedule"));
}

TEST_CASE("An uncharacterized bond window leaves the schedule unproven",
          "[batch-semantics]")
{
    std::vector<ComponentSemantics> components = two_component_semantics();
    components[0].bond_window_s.reset();

    const SemanticReview review = review_batch_semantics(two_towers(), components, {}, {},
                                                         policy());

    CHECK(review.verdict == SemanticVerdict::Unproven);
    CHECK(mentions(review, "never characterized"));
    CHECK(mentions(review, "PLA"));
}

TEST_CASE("An uncharacterized ooze window cannot be assumed away", "[batch-semantics]")
{
    SemanticPolicy uncharacterized = policy();
    uncharacterized.ooze_window_s.reset();

    const SemanticReview review = review_batch_semantics(
        two_towers(), two_component_semantics(), {}, {}, uncharacterized);

    CHECK(review.verdict == SemanticVerdict::Unproven);
    CHECK(has(review, SemanticIssue::Ooze, SemanticSeverity::Warning));
    CHECK(mentions(review, "cannot be shown not to ooze"));
}

TEST_CASE("A tool left sitting hot past its ooze window is reported",
          "[batch-semantics]")
{
    SemanticPolicy oozy   = policy();
    oozy.ooze_window_s    = 20.;

    const SemanticReview review = review_batch_semantics(
        two_towers(), two_component_semantics(), {}, {}, oozy);

    CHECK(review.verdict == SemanticVerdict::Degraded);
    CHECK(has(review, SemanticIssue::Ooze, SemanticSeverity::Warning));
    CHECK(mentions(review, "sat hot and idle"));
}

TEST_CASE("Repriming that costs more than the toolchanges it saved is reported",
          "[batch-semantics]")
{
    SemanticPolicy expensive   = policy();
    expensive.reprime_cost_s   = 60.;
    expensive.toolchange_cost_s = 5.;

    const SemanticReview review = review_batch_semantics(
        two_towers(), two_component_semantics(), {}, {}, expensive);

    // Two returns at 60 s against four toolchanges saved at 5 s.
    CHECK(review.estimate.returns == 2);
    CHECK(review.estimate.reprime_time_s == 120.);
    CHECK(has(review, SemanticIssue::ReprimeCost, SemanticSeverity::Warning));
    CHECK(mentions(review, "slower than the ordinary one"));
}

TEST_CASE("A saving that was never computed is not weighed against anything",
          "[batch-semantics]")
{
    BatchPlan plan = two_towers();
    plan.toolchanges_saved.reset();

    const SemanticReview review = review_batch_semantics(plan, two_component_semantics(), {},
                                                         {}, policy());

    CHECK(review.verdict == SemanticVerdict::Unproven);
    CHECK(mentions(review, "nothing to weigh that against"));
}

TEST_CASE("Estimation across a non-monotonic global Z", "[batch-semantics]")
{
    const BatchPlan      plan   = two_towers();
    const SemanticReview review = review_batch_semantics(plan, two_component_semantics(), {},
                                                         {}, policy());

    CHECK(review.estimate.non_monotonic_z);
    CHECK(review.estimate.peak_z_mm == 0.8);
    // Eight operations of 30 s, three toolchanges at 25 s, two reprimes at 8 s.
    CHECK(review.estimate.toolchanges == 3);
    CHECK(review.estimate.returns == 2);
    CHECK(review.estimate.total_time_s == 240. + 75. + 16.);
    REQUIRE(review.estimate.final_tool.has_value());
    CHECK(*review.estimate.final_tool == 1);

    SECTION("a total that does not match the operations is refused") {
        BatchPlan wrong          = plan;
        wrong.metrics.total_time_s = 200.;
        const SemanticReview bad = review_batch_semantics(wrong, two_component_semantics(),
                                                          {}, {}, policy());
        CHECK(bad.verdict == SemanticVerdict::Unsound);
        CHECK(has(bad, SemanticIssue::Estimation, SemanticSeverity::Blocking));
        CHECK(mentions(bad, "an estimate of a different print"));
    }

    SECTION("metrics that deny the descents are refused") {
        BatchPlan wrong                = plan;
        wrong.metrics.downward_moves   = 0;
        const SemanticReview bad = review_batch_semantics(wrong, two_component_semantics(),
                                                          {}, {}, policy());
        CHECK(bad.verdict == SemanticVerdict::Unsound);
        CHECK(mentions(bad, "do not describe it"));
    }
}

TEST_CASE("Batch boundaries are where a checkpoint can go", "[batch-semantics]")
{
    const std::vector<RecoverableOperation> boundaries =
        batch_boundary_operations(two_towers(), two_component_semantics());

    // The end of each of the four runs.
    REQUIRE(boundaries.size() == 4);
    CHECK(boundaries[0].index == 1);
    CHECK(boundaries[1].index == 3);
    CHECK(boundaries[2].index == 5);
    CHECK(boundaries[3].index == 7);
    for (const RecoverableOperation &boundary : boundaries) {
        CHECK(boundary.safe_to_interrupt_after);
        // Nothing recorded the melt zone, and Unknown forces a reprime rather
        // than pretending it was relieved.
        CHECK(boundary.pressure == PressureState::Unknown);
    }
}

TEST_CASE("Checkpointing the batch boundaries needs firmware that can do it",
          "[batch-semantics]")
{
    SemanticPolicy checkpointing            = policy();
    checkpointing.checkpoint_batch_boundaries = true;

    SECTION("a firmware with no hook gets a limitation, not a fabricated command") {
        const SemanticReview review = review_batch_semantics(
            two_towers(), two_component_semantics(), {}, {}, checkpointing);

        REQUIRE(review.checkpoints.has_value());
        CHECK(has(review, SemanticIssue::Checkpoint, SemanticSeverity::Warning));
        for (const RecoveryCheckpoint &checkpoint : review.checkpoints->checkpoints)
            CHECK_FALSE(checkpoint.emitted);
    }

    SECTION("a firmware that declared one gets checkpoints at the boundaries") {
        checkpointing.recovery_capabilities.can_record_checkpoint = true;
        checkpointing.recovery_capabilities.can_reprime           = true;
        checkpointing.recovery_capabilities.checkpoint_gcode = "M117 CP [operation]";
        checkpointing.recovery_capabilities.reprime_gcode    = "G1 E5";

        const SemanticReview review = review_batch_semantics(
            two_towers(), two_component_semantics(), {}, {}, checkpointing);

        REQUIRE(review.checkpoints.has_value());
        CHECK(review.checkpoints->checkpoints.size() == 4);
        for (const RecoveryCheckpoint &checkpoint : review.checkpoints->checkpoints) {
            CHECK(checkpoint.emitted);
            // Pressure was never recorded at a boundary, so every resume has
            // to reprime.
            CHECK(checkpoint.requires_reprime);
        }
    }
}

TEST_CASE("Cancel-object metadata has to describe every run of a split object",
          "[batch-semantics]")
{
    const SemanticReview review = review_batch_semantics(two_towers(),
                                                         two_component_semantics(), {}, {},
                                                         policy());

    REQUIRE(review.cancel_objects.size() == 2);
    for (const CancelObjectRegion &region : review.cancel_objects) {
        CHECK(region.actionable);
        CHECK_FALSE(region.contiguous);
        CHECK(region.runs.size() == 2);
    }
    CHECK(review.cancel_objects[0].runs[0] == std::make_pair(size_t(0), size_t(1)));
    CHECK(review.cancel_objects[0].runs[1] == std::make_pair(size_t(4), size_t(5)));
    CHECK(mentions(review, "would cancel part of an object and leave the rest printing"));
}

TEST_CASE("A cancellable object with no name is metadata that cancels nothing",
          "[batch-semantics]")
{
    std::vector<ComponentSemantics> components = two_component_semantics();
    components[1].cancel_object_name.clear();

    const SemanticReview review = review_batch_semantics(two_towers(), components, {}, {},
                                                         policy());

    CHECK(review.verdict == SemanticVerdict::Degraded);
    CHECK(has(review, SemanticIssue::CancelObject, SemanticSeverity::Warning));
    CHECK(mentions(review, "names nothing a firmware could cancel"));
    REQUIRE(review.cancel_objects.size() == 2);
    CHECK_FALSE(review.cancel_objects[1].actionable);
}

TEST_CASE("A schedule with nothing in it has nothing to be wrong about",
          "[batch-semantics]")
{
    BatchPlan empty;
    empty.mode    = ScheduleMode::Legacy;
    empty.verdict = BatchVerdict::Disabled;

    const SemanticReview review = review_batch_semantics(empty, {}, {}, {}, policy());
    CHECK(review.verdict == SemanticVerdict::Sound);
    CHECK(review.findings.empty());
    CHECK_FALSE(review.estimate.final_tool.has_value());
}
