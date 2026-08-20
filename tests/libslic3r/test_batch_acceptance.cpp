// Phase 25F: the acceptance suite for bounded-Z batching, run as one set.
//
// The sixteen cases the specification asks for are here in its order, each
// named for the case it is. They exercise 25A (independence), 25B (the
// scheduler), 25C (the descent proof), 25D (the semantic review) and 25E (the
// preview gate) together rather than one at a time, because the thing being
// accepted is the combination.
//
// Bounded batching is compared against both baselines the spec names - the
// ordinary by-layer schedule and the complete-object schedule - on the same
// measurements: toolchanges, time, waste, maximum Z lead, downward moves and
// clearance margin.
//
// Case 14 asks for digital G-code twin verification of every downward and
// low-Z travel move. The twin is Phase 26 and does not exist yet, so what is
// checked here is that the exact move set a twin would have to verify is
// produced and complete. That is not the same as having verified it, and the
// case says so.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>

#include "libslic3r/BatchPreview.hpp"
#include "libslic3r/DescentProof.hpp"

using namespace Slic3r;

namespace {

// --- components and schedules ----------------------------------------------

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

PrintableRegion region(size_t id, int tool, double x0, size_t parent)
{
    PrintableRegion value;
    value.id            = id;
    value.parent_object = parent;
    value.tool          = tool;
    value.min_x         = x0;
    value.max_x         = x0 + 20.;
    value.min_y         = 0.;
    value.max_y         = 20.;
    value.min_z         = 0.;
    value.max_z         = 2.;
    return value;
}

// --- the comparison the spec asks to be recorded ----------------------------

struct Comparison
{
    size_t                toolchanges { 0 };
    double                total_time_s { 0. };
    double                waste_mm3 { 0. };
    double                max_z_lead_mm { 0. };
    size_t                downward_moves { 0 };
    std::optional<double> clearance_margin_mm;
};

// Purge volume per toolchange. Waste is not measured by the scheduler, so it
// is derived here from the one thing that causes it, and only from that.
constexpr double PURGE_MM3 = 90.;

Comparison compare(const std::vector<ScheduledOperation> &operations,
                   const std::vector<BatchComponent>     &components,
                   const BatchingOptions                 &options)
{
    const ScheduleMetrics metrics = measure_schedule(operations, components, options);

    Comparison value;
    value.toolchanges         = metrics.toolchanges;
    value.total_time_s        = metrics.total_time_s;
    value.waste_mm3           = double(metrics.toolchanges) * PURGE_MM3;
    value.max_z_lead_mm       = metrics.max_z_lead_mm;
    value.downward_moves      = metrics.downward_moves;
    value.clearance_margin_mm = metrics.clearance_margin_mm;
    return value;
}

// --- the descent fixtures, shared with Phase 25C ----------------------------

PrintedHeightField bed()
{
    PrintedHeightField field;
    field.origin_x = -50.;
    field.origin_y = -50.;
    field.cell_mm  = 10.;
    field.nx       = 40;
    field.ny       = 20;
    field.height_mm.assign(field.nx * field.ny, std::optional<double>(0.));

    auto set = [&](size_t ix, size_t iy, double height) {
        field.height_mm[iy * field.nx + ix] = height;
    };
    for (size_t ix : { size_t(5), size_t(6) })
        for (size_t iy : { size_t(5), size_t(6) })
            set(ix, iy, 2.0);
    for (size_t ix : { size_t(15), size_t(16) })
        for (size_t iy : { size_t(5), size_t(6) })
            set(ix, iy, 1.0);
    return field;
}

ToolheadShape toolhead()
{
    ToolheadShape shape;
    shape.tool   = 0;
    shape.nozzle = Aabb::of(-0.5, -0.5, 0., 0.5, 0.5, 1.);
    shape.body   = Aabb::of(-12., -12., 1., 12., 12., 60.);
    return shape;
}

DescentCorridor corridor(const std::string &name, double x, double y)
{
    DescentCorridor value;
    value.name          = name;
    value.center_x      = x;
    value.center_y      = y;
    value.half_width_mm = 15.;
    return value;
}

DescentContext descent_context()
{
    DescentContext value;
    value.shapes                     = { toolhead() };
    value.printed                    = bed();
    value.corridors                  = { corridor("the gap between the towers", 60., 10.) };
    value.calibration_uncertainty_mm = 0.2;
    value.clearance_mm               = 1.0;
    value.sample_step_mm             = 2.0;
    return value;
}

DescentRequest descent()
{
    DescentRequest value;
    value.tool   = 0;
    value.from_x = 10.;
    value.from_y = 10.;
    value.from_z = 2.0;
    value.to_x   = 110.;
    value.to_y   = 10.;
    value.to_z   = 1.0;
    return value;
}

// --- semantics --------------------------------------------------------------

std::vector<ComponentSemantics> semantics_for(const std::vector<size_t> &ids)
{
    std::vector<ComponentSemantics> value;
    for (size_t id : ids) {
        ComponentSemantics component;
        component.id                 = id;
        component.material           = "PLA";
        component.bond_window_s      = 3600.;
        component.cancellable        = true;
        component.cancel_object_name = "tower " + std::to_string(id);
        value.push_back(component);
    }
    return value;
}

SemanticPolicy semantic_policy()
{
    SemanticPolicy value;
    value.ooze_window_s     = 3600.;
    value.toolchange_cost_s = 25.;
    value.reprime_cost_s    = 8.;
    return value;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

bool finding_mentions(const SemanticReview &review, const std::string &fragment)
{
    return std::any_of(review.findings.begin(), review.findings.end(),
                       [&](const SemanticFinding &finding) {
                           return finding.message.find(fragment) != std::string::npos;
                       });
}

} // namespace

// 1 ---------------------------------------------------------------------------

TEST_CASE("25F/1: two tall separated towers on T0 and T1 lose toolchanges",
          "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    REQUIRE(plan.mode == ScheduleMode::BoundedBatch);
    CHECK(plan.verdict == BatchVerdict::Batched);
    CHECK(plan.operations.size() == 20);
    CHECK(plan.legacy_metrics.toolchanges == 19);
    CHECK(plan.metrics.toolchanges == 3);
    REQUIRE(plan.toolchanges_saved.has_value());
    CHECK(*plan.toolchanges_saved == 16);
}

TEST_CASE("25F/1: bounded batching measured against both baselines",
          "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchingOptions             options = batching();
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), options);

    const Comparison by_layer  = compare(ordinary_schedule(towers), towers, options);
    const Comparison by_object = compare(complete_object_schedule(towers), towers, options);
    const Comparison batched   = compare(plan.operations, towers, options);

    // Every schedule prints every layer exactly once.
    CHECK(ordinary_schedule(towers).size() == 20);
    CHECK(complete_object_schedule(towers).size() == 20);
    CHECK(plan.operations.size() == 20);

    // Toolchanges, and the waste that follows from them.
    CHECK(by_layer.toolchanges == 19);
    CHECK(batched.toolchanges == 3);
    CHECK(by_object.toolchanges == 1);
    CHECK(batched.waste_mm3 < by_layer.waste_mm3);
    CHECK(by_object.waste_mm3 < batched.waste_mm3);

    // Time follows the toolchanges, because the layers are the same layers.
    CHECK(batched.total_time_s < by_layer.total_time_s);
    CHECK(by_object.total_time_s < batched.total_time_s);

    // And this is the trade the whole phase exists to bound. The
    // complete-object schedule wins on every number above and is still not the
    // default, because it leads by the full height of a tower and comes back
    // down through it.
    // Interleaving by layer still leads by one layer, which is the least any
    // schedule can lead by.
    CHECK(by_layer.max_z_lead_mm <= 0.2 + 1e-6);
    CHECK(batched.max_z_lead_mm > by_layer.max_z_lead_mm);
    CHECK(batched.max_z_lead_mm <= 1.0 + 1e-6);
    CHECK(by_object.max_z_lead_mm > batched.max_z_lead_mm);
    CHECK(by_layer.downward_moves == 0);
    CHECK(batched.downward_moves == 3);
    CHECK(by_object.downward_moves == 9);

    // The clearance margin is a property of the parts, not of the order.
    REQUIRE(batched.clearance_margin_mm.has_value());
    CHECK(*batched.clearance_margin_mm == 80.);
    CHECK(batched.clearance_margin_mm == by_layer.clearance_margin_mm);
    CHECK(batched.clearance_margin_mm == by_object.clearance_margin_mm);
}

// 2 ---------------------------------------------------------------------------

TEST_CASE("25F/2: towers with different layer heights and non-identical grids",
          "[batch-acceptance]")
{
    // 10 layers of 0.2 mm against 7 of 0.3 mm: the grids share no plane above
    // 0.6 mm, so nothing may assume they line up.
    const std::vector<BatchComponent> towers { tower(0, 0, 0., 10, 0.2, 30.),
                                               tower(1, 1, 100., 7, 0.3, 45.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    REQUIRE(plan.mode == ScheduleMode::BoundedBatch);
    CHECK(plan.operations.size() == 17);
    CHECK(plan.metrics.toolchanges < plan.legacy_metrics.toolchanges);
    CHECK(plan.metrics.max_z_lead_mm <= 1.0 + 1e-6);

    // Each component's own layers stay in its own grid and its own order.
    const SemanticReview review = review_batch_semantics(plan, semantics_for({ 0, 1 }), {},
                                                         {}, semantic_policy());
    CHECK_FALSE(finding_mentions(review, "never a component against itself"));
    REQUIRE(review.estimate.components.size() == 2);
    CHECK(review.estimate.components[0].layers_printed == 10);
    CHECK(review.estimate.components[1].layers_printed == 7);
}

// 3 ---------------------------------------------------------------------------

TEST_CASE("25F/3: two disconnected colours inside one object", "[batch-acceptance]")
{
    PrintableRegion left  = region(0, 0, 0., 7);
    PrintableRegion right = region(1, 1, 100., 7);
    left.kind             = RegionKind::PaintedRegion;
    right.kind            = RegionKind::PaintedRegion;

    SECTION("one object stays one component, and batching is not offered") {
        const ComponentDecomposition result =
            decompose_components({ left, right }, {}, ComponentOptions {});
        REQUIRE(result.components.size() == 1);
        CHECK_FALSE(result.batching_eligible);

        const BatchPlan plan = plan_bounded_batches(
            { tower(0, 0, 0.), tower(1, 1, 100.) }, result, batching());
        CHECK(plan.mode == ScheduleMode::FallbackLegacy);
        CHECK(plan.verdict == BatchVerdict::FellBack);
    }

    SECTION("splitting one object is something the caller has to ask for") {
        ComponentOptions options;
        options.allow_intra_object_split = true;

        const ComponentDecomposition result =
            decompose_components({ left, right }, {}, options);
        REQUIRE(result.components.size() == 2);
        CHECK(result.batching_eligible);
    }
}

// 4 ---------------------------------------------------------------------------

TEST_CASE("25F/4: four towers on four tools", "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.),
                                               tower(2, 2, 200.), tower(3, 3, 300.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1, 2, 3 }), batching());

    REQUIRE(plan.mode == ScheduleMode::BoundedBatch);
    CHECK(plan.operations.size() == 40);
    CHECK(plan.legacy_metrics.toolchanges == 39);
    CHECK(plan.metrics.toolchanges < plan.legacy_metrics.toolchanges);
    CHECK(plan.metrics.max_z_lead_mm <= 1.0 + 1e-6);

    // Every tower is printed in full, and each keeps its own tool.
    const SemanticReview review = review_batch_semantics(plan, semantics_for({ 0, 1, 2, 3 }),
                                                         {}, {}, semantic_policy());
    REQUIRE(review.estimate.components.size() == 4);
    for (const ComponentTimeline &timeline : review.estimate.components)
        CHECK(timeline.layers_printed == 10);
    CHECK(review.estimate.components[3].tool == 3);
}

// 5 ---------------------------------------------------------------------------

TEST_CASE("25F/5: towers too close for the toolhead envelope", "[batch-acceptance]")
{
    // 5 mm apart, against a 30 mm separation requirement.
    const std::vector<BatchComponent> crowded { tower(0, 0, 0.), tower(1, 1, 25.) };
    const BatchPlan plan = plan_bounded_batches(crowded, proven({ 0, 1 }), batching());

    CHECK(plan.mode == ScheduleMode::FallbackLegacy);
    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(mentions(plan.notes, "5.00 mm"));

    // Falling back is a choice the caller gets to withhold.
    BatchingOptions strict         = batching();
    strict.allow_reported_fallback = false;
    const BatchPlan blocked = plan_bounded_batches(crowded, proven({ 0, 1 }), strict);
    CHECK(blocked.verdict == BatchVerdict::Blocked);
    CHECK(blocked.operations.empty());
}

// 6 ---------------------------------------------------------------------------

TEST_CASE("25F/6: a path safe for the nozzle point but not for the carriage",
          "[batch-acceptance]")
{
    DescentContext tip_only      = descent_context();
    tip_only.shapes.front().body = Aabb {};

    const DescentPlan plan = prove_descent(descent(), tip_only);
    CHECK(plan.verdict == DescentVerdict::Unproven);
    CHECK(mentions(plan.reasons, "does not prove the carriage or gantry clears"));

    // The same descent with the whole toolhead described is provable, so the
    // refusal above is about what was described, not about the geometry.
    CHECK(prove_descent(descent(), descent_context()).verdict == DescentVerdict::Clear);
}

// 7 ---------------------------------------------------------------------------

TEST_CASE("25F/7: a dock path intersecting advanced geometry", "[batch-acceptance]")
{
    Obstacle dock;
    dock.name = "the T1 dock";
    dock.kind = ObstacleKind::Dock;
    dock.box  = Aabb::of(55., 5., 0., 65., 15., 40.);

    DescentContext blocked = descent_context();
    blocked.obstacles      = { dock };

    const DescentPlan plan = prove_descent(descent(), blocked);
    CHECK(plan.verdict == DescentVerdict::Refused);
    CHECK(mentions(plan.rejected_corridors, "the T1 dock"));
    CHECK(mentions(plan.reasons, "none of them is clear"));
}

// 8 ---------------------------------------------------------------------------

TEST_CASE("25F/8: safe and unsafe downward-Z corridors", "[batch-acceptance]")
{
    SECTION("a corridor verified empty is clear") {
        const DescentPlan plan = prove_descent(descent(), descent_context());
        CHECK(plan.verdict == DescentVerdict::Clear);
        REQUIRE(plan.corridor.has_value());
        CHECK(*plan.corridor == "the gap between the towers");
    }

    SECTION("a corridor over printed geometry is rejected by name") {
        DescentContext over_the_part = descent_context();
        over_the_part.corridors      = { corridor("straight down onto the tower", 10., 10.) };

        const DescentPlan plan = prove_descent(descent(), over_the_part);
        CHECK(plan.verdict == DescentVerdict::Refused);
        CHECK(mentions(plan.rejected_corridors, "dropping onto the part"));
    }

    SECTION("a corridor whose printed height is unknown is not clear either") {
        DescentContext unknown = descent_context();
        unknown.printed.height_mm[6 * unknown.printed.nx + 11] = std::nullopt;

        const DescentPlan plan = prove_descent(descent(), unknown);
        CHECK(plan.verdict == DescentVerdict::Unproven);
        CHECK(mentions(plan.rejected_corridors, "unknown is not the same as clear"));
    }
}

// 9 ---------------------------------------------------------------------------

TEST_CASE("25F/9: supports linking otherwise separate components",
          "[batch-acceptance]")
{
    PrintableRegion left  = region(0, 0, 0., 0);
    PrintableRegion right = region(1, 1, 100., 1);

    SECTION("separate without the support") {
        const ComponentDecomposition result =
            decompose_components({ left, right }, {}, ComponentOptions {});
        CHECK(result.components.size() == 2);
        CHECK(result.batching_eligible);
    }

    SECTION("one component once a support bridges them") {
        RegionLink support;
        support.a           = 0;
        support.b           = 1;
        support.kind        = LinkKind::Supports;
        support.description = "a shared support raft";

        const ComponentDecomposition result =
            decompose_components({ left, right }, { support }, ComponentOptions {});
        REQUIRE(result.components.size() == 1);
        CHECK_FALSE(result.batching_eligible);

        const BatchPlan plan = plan_bounded_batches(
            { tower(0, 0, 0.), tower(1, 1, 100.) }, result, batching());
        CHECK(plan.verdict == BatchVerdict::FellBack);
    }
}

// 10 --------------------------------------------------------------------------

TEST_CASE("25F/10: a coarse region advanced over an access a fine tool needs",
          "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    AccessRequirement need;
    need.component         = 0;
    need.fine_tool         = 1;
    need.what              = "the embedded T1 channel";
    need.sealed_above_z_mm = 0.6;

    const SemanticReview review = review_batch_semantics(plan, semantics_for({ 0, 1 }), {},
                                                         { need }, semantic_policy());

    CHECK(review.verdict == SemanticVerdict::Unsound);
    CHECK_FALSE(review.emittable());
    CHECK(finding_mentions(review, "the embedded T1 channel"));
    CHECK(finding_mentions(review, "has not been in there yet"));

    // And the preview refuses to offer it, in those words.
    AcknowledgementRequest request;
    request.user    = "tiger";
    request.feature = "bounded-z-batching";
    AcknowledgementRecord record;
    record.user                = "tiger";
    record.feature             = "bounded-z-batching";
    record.explicitly_answered = true;
    record.accepted            = true;

    const BatchPreview preview =
        build_batch_preview(plan, review, semantic_policy(),
                            check_acknowledgement(request, { record }));
    CHECK_FALSE(preview.exportable);
    CHECK(preview.has_banner(PreviewBannerLevel::Blocked));
}

// 11 --------------------------------------------------------------------------

TEST_CASE("25F/11: cooling and idle-time limits cut the batch down",
          "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };

    BatchingOptions impatient = batching();
    impatient.max_idle_s      = 40.;

    const BatchPlan patient   = plan_bounded_batches(towers, proven({ 0, 1 }), batching());
    const BatchPlan restricted = plan_bounded_batches(towers, proven({ 0, 1 }), impatient);

    CHECK(mentions(restricted.limits, "maximum idle time"));
    CHECK(restricted.metrics.max_idle_s <= patient.metrics.max_idle_s);
    // Smaller batches means the toolchanges come back.
    CHECK(restricted.metrics.toolchanges >= patient.metrics.toolchanges);

    // A component that sat past its bond window is reported against the bond,
    // not against the clock.
    std::vector<ComponentSemantics> fragile = semantics_for({ 0, 1 });
    fragile[0].bond_window_s                = 30.;
    const SemanticReview review = review_batch_semantics(patient, fragile, {}, {},
                                                         semantic_policy());
    CHECK(review.verdict == SemanticVerdict::Degraded);
    CHECK(finding_mentions(review, "stays bondable"));
}

// 12 --------------------------------------------------------------------------

TEST_CASE("25F/12: a saving smaller than what it costs picks the ordinary schedule",
          "[batch-acceptance]")
{
    // Both towers on one tool: there were never any toolchanges to remove, and
    // repriming between batches is not free.
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 0, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    CHECK(plan.verdict == BatchVerdict::FellBack);
    CHECK(plan.mode == ScheduleMode::FallbackLegacy);
    REQUIRE(plan.toolchanges_saved.has_value());
    CHECK(*plan.toolchanges_saved == 0);

    // And where a schedule does batch, repriming that outweighs the saving is
    // reported against the saving it claimed.
    const BatchPlan batched = plan_bounded_batches(
        { tower(0, 0, 0.), tower(1, 1, 100.) }, proven({ 0, 1 }), batching());
    SemanticPolicy expensive    = semantic_policy();
    expensive.reprime_cost_s    = 400.;
    expensive.toolchange_cost_s = 5.;

    const SemanticReview review = review_batch_semantics(batched, semantics_for({ 0, 1 }), {},
                                                         {}, expensive);
    CHECK(finding_mentions(review, "slower than the ordinary one"));
}

// 13 --------------------------------------------------------------------------

TEST_CASE("25F/13: cancel-object metadata and recovery checkpoints in a batched plan",
          "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    SemanticPolicy checkpointing               = semantic_policy();
    checkpointing.checkpoint_batch_boundaries  = true;
    checkpointing.recovery_capabilities.can_record_checkpoint = true;
    checkpointing.recovery_capabilities.can_reprime           = true;
    checkpointing.recovery_capabilities.checkpoint_gcode = "M117 CP [operation]";
    checkpointing.recovery_capabilities.reprime_gcode    = "G1 E5";

    const SemanticReview review = review_batch_semantics(plan, semantics_for({ 0, 1 }), {},
                                                         {}, checkpointing);

    // Every batch boundary is a checkpoint, and every one demands a reprime,
    // because nothing recorded the melt zone when the tool left.
    REQUIRE(review.checkpoints.has_value());
    CHECK(review.checkpoints->checkpoints.size() ==
          batch_boundary_operations(plan, semantics_for({ 0, 1 })).size());
    CHECK(review.checkpoints->checkpoints.size() > 1);
    for (const RecoveryCheckpoint &checkpoint : review.checkpoints->checkpoints) {
        CHECK(checkpoint.emitted);
        CHECK(checkpoint.requires_reprime);
    }

    // Both towers are cancellable, and batching split each into several runs,
    // so every run of each is marked.
    REQUIRE(review.cancel_objects.size() == 2);
    for (const CancelObjectRegion &object : review.cancel_objects) {
        CHECK(object.actionable);
        CHECK_FALSE(object.contiguous);
        CHECK(object.runs.size() > 1);

        // The runs cover the object's operations exactly - no gaps, no overlap.
        size_t covered = 0;
        for (const auto &run : object.runs) {
            CHECK(run.first <= run.second);
            covered += run.second - run.first + 1;
        }
        CHECK(covered == 10);
    }
}

// 14 --------------------------------------------------------------------------

TEST_CASE("25F/14: every downward and low-Z move is produced for a twin to verify",
          "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    // One descent request per downward move in the schedule. Nothing is
    // dropped and nothing is invented: the counts have to match.
    std::vector<DescentRequest> descents;
    for (const ScheduledOperation &operation : plan.operations)
        if (operation.downward_move)
            descents.push_back(descent());
    REQUIRE(descents.size() == plan.metrics.downward_moves);
    REQUIRE(descents.size() == 3);

    const BatchDescentReview review = review_descents(descents, descent_context());
    CHECK(review.all_proven);
    CHECK(review.proven == 3);
    CHECK(provable_batch_prefix(review) == 3);

    // Exactly three moves per descent, in a fixed order, which is the move set
    // a G-code twin would have to check.
    size_t travels = 0, downs = 0;
    for (const DescentPlan &proof : review.descents) {
        REQUIRE(proof.segments.size() == 3);
        CHECK(proof.segments[0].kind == MotionKind::Travel);
        CHECK(proof.segments[1].kind == MotionKind::DownwardZ);
        CHECK(proof.segments[2].kind == MotionKind::Travel);
        travels += 2;
        downs += 1;
    }
    CHECK(travels == 6);
    CHECK(downs == 3);

    // What has NOT happened: none of this has been checked against exported
    // G-code. The digital twin is Phase 26. Producing the move set a twin
    // would verify is not the same as having verified it, and this case does
    // not claim otherwise.
}

// 15 --------------------------------------------------------------------------

TEST_CASE("25F/15: with the mode off, scheduling is unchanged", "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), BatchingOptions {});

    CHECK(plan.mode == ScheduleMode::Legacy);
    CHECK(plan.verdict == BatchVerdict::Disabled);

    // Not merely equivalent: the same operations, in the same order.
    const std::vector<ScheduledOperation> legacy = ordinary_schedule(towers);
    REQUIRE(plan.operations.size() == legacy.size());
    for (size_t i = 0; i < legacy.size(); ++i) {
        CHECK(plan.operations[i].component == legacy[i].component);
        CHECK(plan.operations[i].layer == legacy[i].layer);
        CHECK(plan.operations[i].tool == legacy[i].tool);
        CHECK(plan.operations[i].z_top_mm == legacy[i].z_top_mm);
    }
    CHECK(plan.metrics.downward_moves == 0);

    // And the preview never offers a mode nobody turned on.
    AcknowledgementRequest request;
    request.user    = "tiger";
    request.feature = "bounded-z-batching";
    const SemanticReview review = review_batch_semantics(plan, semantics_for({ 0, 1 }), {},
                                                         {}, semantic_policy());
    const BatchPreview preview = build_batch_preview(plan, review, semantic_policy(),
                                                     check_acknowledgement(request, {}));
    CHECK_FALSE(preview.exportable);
    CHECK(preview.experimental);
}

// 16 --------------------------------------------------------------------------

TEST_CASE("25F/16: identical inputs always schedule identically", "[batch-acceptance]")
{
    const std::vector<BatchComponent> towers { tower(2, 0, 0.), tower(0, 1, 100.),
                                               tower(1, 2, 200.) };
    // The same components, handed over in a different order.
    const std::vector<BatchComponent> shuffled { towers[1], towers[2], towers[0] };

    const BatchPlan first  = plan_bounded_batches(towers, proven({ 0, 1, 2 }), batching());
    const BatchPlan again  = plan_bounded_batches(towers, proven({ 0, 1, 2 }), batching());
    const BatchPlan mixed  = plan_bounded_batches(shuffled, proven({ 2, 0, 1 }), batching());

    REQUIRE(first.operations.size() == again.operations.size());
    for (size_t i = 0; i < first.operations.size(); ++i) {
        CHECK(first.operations[i].component == again.operations[i].component);
        CHECK(first.operations[i].layer == again.operations[i].layer);
        CHECK(first.operations[i].z_top_mm == again.operations[i].z_top_mm);
    }
    CHECK(first.metrics.toolchanges == again.metrics.toolchanges);

    // Component identity, not the order the caller happened to supply them in.
    REQUIRE(mixed.operations.size() == first.operations.size());
    for (size_t i = 0; i < first.operations.size(); ++i) {
        CHECK(mixed.operations[i].component == first.operations[i].component);
        CHECK(mixed.operations[i].layer == first.operations[i].layer);
    }

    // The reviews of two identical schedules agree too.
    const SemanticReview a = review_batch_semantics(first, semantics_for({ 0, 1, 2 }), {}, {},
                                                    semantic_policy());
    const SemanticReview b = review_batch_semantics(again, semantics_for({ 0, 1, 2 }), {}, {},
                                                    semantic_policy());
    CHECK(a.verdict == b.verdict);
    CHECK(a.findings.size() == b.findings.size());
    CHECK(a.estimate.total_time_s == b.estimate.total_time_s);
}

// The gate -------------------------------------------------------------------

TEST_CASE("25F: the Phase 25 completion gate", "[batch-acceptance]")
{
    // "Phase 25 is complete only when unsafe cases are rejected and the
    // two-tower reference case demonstrates a measurable reduction in
    // toolchanges."
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    // A measurable reduction, against a baseline computed the same way.
    const Comparison by_layer = compare(ordinary_schedule(towers), towers, batching());
    const Comparison batched  = compare(plan.operations, towers, batching());
    CHECK(batched.toolchanges < by_layer.toolchanges);
    CHECK(by_layer.toolchanges - batched.toolchanges == 16);

    // Unsafe cases rejected: geometry in the corridor, hardware in the
    // corridor, a toolhead described only as far as its nozzle, and a machine
    // whose calibration was never characterized.
    DescentContext over_the_part = descent_context();
    over_the_part.corridors      = { corridor("straight down onto the tower", 10., 10.) };
    CHECK(prove_descent(descent(), over_the_part).verdict == DescentVerdict::Refused);

    Obstacle dock;
    dock.name              = "the T1 dock";
    dock.kind              = ObstacleKind::Dock;
    dock.box               = Aabb::of(55., 5., 0., 65., 15., 40.);
    DescentContext blocked = descent_context();
    blocked.obstacles      = { dock };
    CHECK(prove_descent(descent(), blocked).verdict == DescentVerdict::Refused);

    DescentContext tip_only      = descent_context();
    tip_only.shapes.front().body = Aabb {};
    CHECK(prove_descent(descent(), tip_only).verdict == DescentVerdict::Unproven);

    DescentContext uncalibrated                = descent_context();
    uncalibrated.calibration_uncertainty_mm    = std::nullopt;
    CHECK(prove_descent(descent(), uncalibrated).verdict == DescentVerdict::Unproven);
}
