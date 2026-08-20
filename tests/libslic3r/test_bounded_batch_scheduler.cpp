// Phase 25B: the bounded lead/lag scheduler. This is the experimental mode, so
// the tests spend most of their effort on the ways it must refuse: it is off
// by default, it will not run unless Phase 25A proved the components
// independent, it will not run over components too close for the toolhead to
// work between, and it will not run at all without explicit permission to move
// z downward - because coming back to a lagging component means descending.
// When it does run, the ordinary schedule's metrics come back alongside its
// own so the two can be compared rather than taken on trust, and every batch
// that stopped short says what stopped it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/BoundedBatchScheduler.hpp"

using namespace Slic3r;

namespace {

BatchComponent tower(size_t id, int tool, double x0, size_t layer_count = 10,
                     double height = 0.2, double layer_time = 30.)
{
    BatchComponent value;
    value.id    = id;
    value.min_x = x0;
    value.max_x = x0 + 20.;
    value.min_y = 0.;
    value.max_y = 20.;

    for (size_t index = 0; index < layer_count; ++index) {
        BatchLayer layer;
        layer.z_bottom_mm  = double(index) * height;
        layer.z_top_mm     = double(index + 1) * height;
        layer.tool         = tool;
        layer.print_time_s = layer_time;
        value.layers.push_back(layer);
    }
    return value;
}

// A Phase 25A result that proved every one of these components independent.
ComponentDecomposition proven(const std::vector<size_t> &ids)
{
    ComponentDecomposition result;
    for (size_t id : ids) {
        DependencyComponent component;
        component.id      = id;
        component.verdict = IndependenceVerdict::Independent;
        component.regions = { id };
        result.components.push_back(component);
    }
    result.batching_eligible = ids.size() > 1;
    return result;
}

BatchingOptions batching()
{
    BatchingOptions options;
    options.enabled                     = true;
    options.policy                      = BatchPolicy::Aggressive;
    options.max_layers_per_batch        = 8;
    options.max_z_lead_mm               = 1.0;
    options.max_z_lead_layers           = 20;
    options.min_component_separation_mm = 30.;
    options.max_idle_s                  = 600.;
    options.allow_downward_z            = true;
    return options;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("The ordinary schedule interleaves every component by layer",
          "[bounded-batching]")
{
    const std::vector<ScheduledOperation> operations =
        ordinary_schedule({ tower(0, 0, 0.), tower(1, 1, 100.) });

    REQUIRE(operations.size() == 20);
    // T0, T1, T0, T1 ... one toolchange between every pair of layers.
    CHECK(std::count_if(operations.begin(), operations.end(),
                        [](const ScheduledOperation &operation) {
                            return operation.toolchange_before;
                        }) == 19);
}

TEST_CASE("Batching is off until it is asked for", "[bounded-batching]")
{
    const BatchPlan plan =
        plan_bounded_batches({ tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }),
                             BatchingOptions {});

    CHECK(plan.mode == ScheduleMode::Legacy);
    CHECK(plan.verdict == BatchVerdict::Disabled);
    CHECK(plan.operations.size() == 20);
    CHECK(mentions(plan.notes, "nothing about it has changed"));
}

TEST_CASE("The two-tower reference case removes toolchanges", "[bounded-batching]")
{
    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), batching());

    CHECK(plan.mode == ScheduleMode::BoundedBatch);
    CHECK(plan.verdict == BatchVerdict::Batched);

    // Every layer is still printed, once.
    CHECK(plan.operations.size() == 20);
    CHECK(plan.legacy_metrics.toolchanges == 19);
    CHECK(plan.metrics.toolchanges == 3);
    REQUIRE(plan.toolchanges_saved.has_value());
    CHECK(*plan.toolchanges_saved == 16);
    REQUIRE(plan.time_saved_s.has_value());
    CHECK(*plan.time_saved_s > 0.);

    // The lead never exceeds what was allowed, and the descents are counted.
    CHECK(plan.metrics.max_z_lead_mm <= 1.0 + 1e-6);
    CHECK(plan.metrics.downward_moves == 3);
    CHECK(mentions(plan.notes, "still has to be proven clear before it is emitted"));
    CHECK(mentions(plan.limits, "maximum z lead of 1.00 mm"));
}

TEST_CASE("Batching needs the components to have been proven independent",
          "[bounded-batching]")
{
    ComponentDecomposition unproven = proven({ 0, 1 });
    unproven.batching_eligible      = false;

    SECTION("falls back where a reported fallback is allowed") {
        const BatchPlan plan = plan_bounded_batches(
            { tower(0, 0, 0.), tower(1, 1, 100.) }, unproven, batching());

        CHECK(plan.mode == ScheduleMode::FallbackLegacy);
        CHECK(plan.verdict == BatchVerdict::FellBack);
        CHECK(plan.operations.size() == 20);
        CHECK(mentions(plan.notes, "not shown to decompose into independent components"));
        CHECK(mentions(plan.notes, "reported rather than silent"));
    }

    SECTION("blocks where it is not") {
        BatchingOptions options          = batching();
        options.allow_reported_fallback  = false;

        const BatchPlan plan = plan_bounded_batches(
            { tower(0, 0, 0.), tower(1, 1, 100.) }, unproven, options);

        CHECK(plan.verdict == BatchVerdict::Blocked);
        CHECK(plan.operations.empty());
        CHECK(mentions(plan.notes, "nothing is offered for export"));
    }
}

TEST_CASE("A component missing from the decomposition is not assumed independent",
          "[bounded-batching]")
{
    ComponentDecomposition partial = proven({ 0, 1 });
    partial.components.pop_back();

    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, partial, batching());

    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(mentions(plan.notes, "whether anything depends on it is unknown"));
}

TEST_CASE("Without permission to descend, nothing may run ahead", "[bounded-batching]")
{
    BatchingOptions options    = batching();
    options.allow_downward_z   = false;

    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), options);

    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(mentions(plan.notes, "downward z motion has not been permitted"));
}

TEST_CASE("Components too close for the toolhead are not batched",
          "[bounded-batching]")
{
    // Five millimetres apart, against a thirty millimetre requirement.
    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 25.) }, proven({ 0, 1 }), batching());

    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(mentions(plan.notes, "closer than the"));
    CHECK(mentions(plan.notes, "without reaching the other"));
}

TEST_CASE("Batching that does not pay for itself is not used", "[bounded-batching]")
{
    // Both towers on one tool: there were no toolchanges to remove, and
    // repriming between batches costs real time.
    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 0, 100.) }, proven({ 0, 1 }), batching());

    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(mentions(plan.notes, "does not clear the"));
    REQUIRE(plan.toolchanges_saved.has_value());
    CHECK(*plan.toolchanges_saved == 0);
}

TEST_CASE("A conservative policy batches less than an aggressive one",
          "[bounded-batching]")
{
    BatchingOptions careful = batching();
    careful.policy          = BatchPolicy::Conservative;

    const BatchPlan bold = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), batching());
    const BatchPlan timid = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), careful);

    CHECK(timid.metrics.toolchanges > bold.metrics.toolchanges);
    CHECK(timid.metrics.max_z_lead_mm < bold.metrics.max_z_lead_mm);
    CHECK(mentions(timid.notes, "conservative policy spends 50%"));
}

TEST_CASE("A waiting component that would sit too long cuts the batch short",
          "[bounded-batching]")
{
    BatchingOptions impatient = batching();
    impatient.max_idle_s      = 40.;

    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), impatient);

    CHECK(mentions(plan.limits, "maximum idle time"));
    CHECK(mentions(plan.limits, "oozes and cools"));
}

TEST_CASE("A fixed batch size is an upper bound, not permission",
          "[bounded-batching]")
{
    BatchingOptions fixed        = batching();
    fixed.automatic_batch_size   = false;
    fixed.max_layers_per_batch   = 50;

    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), fixed);

    CHECK(mentions(plan.notes, "every dependency, lead and clearance limit still applies"));
    // The z-lead limit still bounds the batch well under the fifty asked for.
    CHECK(plan.metrics.max_z_lead_mm <= 1.0 + 1e-6);
}

TEST_CASE("The maximum z lead in layers bounds a batch of thin layers",
          "[bounded-batching]")
{
    BatchingOptions options      = batching();
    options.max_layers_per_batch = 40;
    options.max_z_lead_mm        = 100.;
    options.max_z_lead_layers    = 3;

    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), options);

    CHECK(mentions(plan.limits, "maximum z lead of 3 layers"));
}

TEST_CASE("A single component has nothing to interleave with", "[bounded-batching]")
{
    ComponentDecomposition alone = proven({ 0 });
    alone.batching_eligible      = true;

    const BatchPlan plan = plan_bounded_batches({ tower(0, 0, 0.) }, alone, batching());
    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(mentions(plan.notes, "nothing to interleave it with"));
}

TEST_CASE("The same print always schedules the same way", "[bounded-batching]")
{
    const std::vector<BatchComponent> towers {
        tower(2, 0, 0.), tower(0, 1, 100.), tower(1, 2, 200.)
    };

    const BatchPlan first  = plan_bounded_batches(towers, proven({ 0, 1, 2 }), batching());
    const BatchPlan second = plan_bounded_batches(towers, proven({ 0, 1, 2 }), batching());

    REQUIRE(first.operations.size() == second.operations.size());
    for (size_t index = 0; index < first.operations.size(); ++index) {
        CHECK(first.operations[index].component == second.operations[index].component);
        CHECK(first.operations[index].layer == second.operations[index].layer);
    }
    CHECK(first.metrics.toolchanges == second.metrics.toolchanges);
}

TEST_CASE("Four towers on four tools still batch", "[bounded-batching]")
{
    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.), tower(2, 2, 200.), tower(3, 3, 300.) },
        proven({ 0, 1, 2, 3 }), batching());

    CHECK(plan.mode == ScheduleMode::BoundedBatch);
    CHECK(plan.metrics.toolchanges < plan.legacy_metrics.toolchanges);
    CHECK(plan.operations.size() == 40);
}

TEST_CASE("The clearance margin between components is always reported",
          "[bounded-batching]")
{
    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), batching());

    REQUIRE(plan.metrics.clearance_margin_mm.has_value());
    CHECK(*plan.metrics.clearance_margin_mm == 80.);
}

TEST_CASE("A print too large to reason about is not batched", "[bounded-batching]")
{
    BatchingOptions options = batching();
    options.max_operations  = 5;

    const BatchPlan plan = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), options);

    CHECK(plan.verdict == BatchVerdict::Blocked);
    CHECK(mentions(plan.notes, "batching is not offered"));
}

TEST_CASE("Nothing to schedule, and a component with no layers, are both errors",
          "[bounded-batching]")
{
    CHECK_FALSE(
        plan_bounded_batches({}, proven({ 0, 1 }), batching()).error.empty());

    BatchComponent empty;
    empty.id = 1;

    const BatchPlan plan =
        plan_bounded_batches({ tower(0, 0, 0.), empty }, proven({ 0, 1 }), batching());
    CHECK_FALSE(plan.error.empty());
    CHECK(plan.error.find("silently leave it out") != std::string::npos);
}
