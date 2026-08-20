// The Stage A gate: the cross-phase acceptance criteria, run as one set.
//
// Two things happen here. The criteria are judged as a list, so that "the gate
// passed" is something a program answers rather than something a document
// asserts; and the criteria whose own scenario can be run in this environment
// are run here, end to end, across the phases they span.
//
// The criteria that are covered only by their parts being tested separately
// say exactly that, and the gate refuses to report better than Partial while
// any of them remain. Nothing here has met a printer.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <sstream>

#include "libslic3r/BoundedBatchScheduler.hpp"
#include "libslic3r/DescentProof.hpp"
#include "libslic3r/GcodeTwin.hpp"
#include "libslic3r/PrintManifest.hpp"
#include "libslic3r/ProfileVersioning.hpp"
#include "libslic3r/SliceCache.hpp"
#include "libslic3r/StageAcceptance.hpp"

using namespace Slic3r;

namespace {

constexpr int64_t T0 = 1'700'000'000;

// --- the gate itself --------------------------------------------------------

CriterionCoverage covered(AcceptanceCriterion criterion)
{
    return { criterion, CoverageStatus::CoveredByScenario, { "[stage-a]" }, "shown", false };
}

std::vector<CriterionCoverage> everything_covered()
{
    std::vector<CriterionCoverage> coverage;
    for (AcceptanceCriterion criterion : all_criteria())
        coverage.push_back(covered(criterion));
    return coverage;
}

// --- the batching fixtures, matching 25F's ---------------------------------

BatchComponent tower(size_t id, int tool, double x0)
{
    BatchComponent value;
    value.id    = id;
    value.min_x = x0;
    value.max_x = x0 + 20.;
    value.min_y = 0.;
    value.max_y = 20.;
    for (size_t index = 0; index < 10; ++index) {
        BatchLayer layer;
        layer.z_bottom_mm  = double(index) * 0.2;
        layer.z_top_mm     = double(index + 1) * 0.2;
        layer.tool         = tool;
        layer.print_time_s = 30.;
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

BatchingOptions batching(bool enabled = true)
{
    BatchingOptions options;
    options.enabled                     = enabled;
    options.policy                      = BatchPolicy::Aggressive;
    options.max_layers_per_batch        = 8;
    options.max_z_lead_mm               = 1.0;
    options.max_z_lead_layers           = 20;
    options.min_component_separation_mm = 30.;
    options.max_idle_s                  = 600.;
    options.allow_downward_z            = enabled;
    return options;
}

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

// --- the twin --------------------------------------------------------------

// Turn a proven descent's motion segments into the g-code a machine would
// actually be sent, so the twin checks the moves rather than a description of
// them. Written here rather than in the library because the real emitter is
// the g-code writer's job; what is being joined is the move set to the
// verifier.
std::string gcode_for(const std::vector<DescentPlan> &descents)
{
    std::ostringstream out;
    out << "G21\nG90\nM82\n";
    out << "M109 T0 S210\n";
    out << "T0\n";
    // Something on the bed, so the safe-travel rule has a reason to apply.
    out << "G1 F1200 X10 Y10 Z0.2\n";
    out << "G1 F900 X15 Y10 Z0.2 E1.0\n";

    double e = 1.0;
    for (const DescentPlan &plan : descents)
        for (const MotionSegment &segment : plan.segments) {
            out.precision(4);
            out << std::fixed;
            if (segment.kind == MotionKind::Print) {
                e += 0.5;
                out << "G1 F900 X" << segment.x1 << " Y" << segment.y1 << " Z" << segment.z1
                    << " E" << e << "\n";
            } else {
                out << "G0 F3000 X" << segment.x1 << " Y" << segment.y1 << " Z" << segment.z1
                    << "\n";
            }
        }
    return out.str();
}

TwinMachine machine(double safe_travel_z_mm)
{
    TwinMachine value;
    TwinTool t0;
    t0.index               = 0;
    t0.material            = "PLA";
    t0.minimum_extrusion_c = 180.;
    t0.nozzle_mm           = 0.4;
    t0.offset_x_mm         = 0.;
    t0.offset_y_mm         = 0.;
    t0.offset_z_mm         = 0.;
    value.tools            = { t0 };
    value.min_x_mm = -5.;
    value.max_x_mm = 300.;
    value.min_y_mm = -5.;
    value.max_y_mm = 300.;
    value.min_z_mm = 0.;
    value.max_z_mm = 250.;
    // G21 is not a command the twin interprets, and it is right not to guess.
    // Declaring what it does is the mechanism provided for that, and using it
    // here is the point: an undeclared command stays a finding.
    TwinMacroContract millimetres;
    millimetres.name        = "G21";
    millimetres.description = "sets millimetre units and changes nothing else";
    value.macros            = { millimetres };

    value.safe_travel_z_mm     = safe_travel_z_mm;
    value.max_layer_height_mm  = 0.3;
    value.filament_diameter_mm = 1.75;
    return value;
}

bool has_finding(const TwinResult &result, TwinIssue issue)
{
    return result.has(issue);
}

} // namespace

// --- the gate ---------------------------------------------------------------

TEST_CASE("Stage A: the criterion list is the whole list", "[stage-a]")
{
    CHECK(all_criteria().size() == 21);

    // Every criterion has a name of its own; none falls through to the
    // placeholder.
    for (AcceptanceCriterion criterion : all_criteria())
        CHECK(std::string(acceptance_criterion_name(criterion)) != "unknown criterion");
}

TEST_CASE("Stage A: the gate refuses to pass on an incomplete report", "[stage-a]")
{
    SECTION("a criterion nobody reported on")
    {
        std::vector<CriterionCoverage> coverage = everything_covered();
        coverage.pop_back();
        const GateReport report = judge_stage_a(coverage);
        CHECK(report.verdict == GateVerdict::Failed);
        CHECK(report.missing.size() == 1);
        CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("were not reported on"));
    }

    SECTION("a criterion reported twice makes the report ambiguous, not thorough")
    {
        std::vector<CriterionCoverage> coverage = everything_covered();
        coverage.push_back(covered(AcceptanceCriterion::SupportKinds));
        const GateReport report = judge_stage_a(coverage);
        CHECK(report.verdict == GateVerdict::Failed);
        CHECK(report.duplicated == std::vector<AcceptanceCriterion> {
                                       AcceptanceCriterion::SupportKinds });
    }

    SECTION("a claim of coverage with no evidence behind it")
    {
        std::vector<CriterionCoverage> coverage = everything_covered();
        coverage[3].tags.clear();
        coverage[5].note.clear();
        const GateReport report = judge_stage_a(coverage);
        CHECK(report.verdict == GateVerdict::Failed);
        CHECK(report.unevidenced.size() == 2);
        CHECK_THAT(report.summary,
                   Catch::Matchers::ContainsSubstring("without naming the evidence"));
    }

    SECTION("an unexamined criterion is never a passing one")
    {
        std::vector<CriterionCoverage> coverage = everything_covered();
        coverage[0].status = CoverageStatus::Unexamined;
        const GateReport report = judge_stage_a(coverage);
        CHECK(report.verdict == GateVerdict::Failed);
        CHECK(report.unexamined == 1);
    }

    SECTION("everything covered by its own scenario passes")
    {
        const GateReport report = judge_stage_a(everything_covered());
        CHECK(report.verdict == GateVerdict::Passed);
        CHECK(report.covered_by_scenario == 21);
    }

    SECTION("a blocked criterion is not claimed to be met, and is named")
    {
        std::vector<CriterionCoverage> coverage = everything_covered();
        coverage[2].status = CoverageStatus::Blocked;
        coverage[2].note   = "the GUI cannot be launched under Smart App Control";
        const GateReport report = judge_stage_a(coverage);
        CHECK(report.verdict == GateVerdict::PassedWithBlockers);
        REQUIRE(report.blockers.size() == 1);
        CHECK_THAT(report.blockers[0],
                   Catch::Matchers::ContainsSubstring("Smart App Control"));
    }

    SECTION("parts tested separately is not the scenario having been run")
    {
        std::vector<CriterionCoverage> coverage = everything_covered();
        coverage[1].status = CoverageStatus::CoveredByComponentTests;
        const GateReport report = judge_stage_a(coverage);
        CHECK(report.verdict == GateVerdict::Partial);
        CHECK(report.covered_by_component_tests == 1);
        CHECK_THAT(report.summary,
                   Catch::Matchers::ContainsSubstring("rest on their parts being tested "
                                                      "separately"));
    }

    SECTION("no report ever claims a printer was involved")
    {
        const GateReport report = judge_stage_a(everything_covered());
        CHECK_FALSE(report.any_hardware_verified);
        CHECK_THAT(report.summary,
                   Catch::Matchers::ContainsSubstring("verified against a physical printer"));
    }
}

TEST_CASE("Stage A: what this repository actually has", "[stage-a]")
{
    const std::vector<CriterionCoverage> coverage = recorded_stage_a_coverage();
    const GateReport                     report   = judge_stage_a(coverage);

    // Every criterion is accounted for, and every claim names its evidence.
    CHECK(report.missing.empty());
    CHECK(report.duplicated.empty());
    CHECK(report.unevidenced.empty());
    CHECK(report.unexamined == 0);
    CHECK(coverage.size() == all_criteria().size());

    // All twenty-one criteria have now had their own scenario run. The gate
    // passes on everything that can be shown in this environment - and says,
    // in the same breath, that none of it has met a physical printer.
    CHECK(report.verdict == GateVerdict::Passed);
    CHECK(report.covered_by_scenario == 21);
    CHECK(report.covered_by_component_tests == 0);
    CHECK(report.blocked == 0);
    CHECK(report.covered_by_scenario + report.covered_by_component_tests ==
          all_criteria().size());
    CHECK_FALSE(report.any_hardware_verified);

    // Every entry, whatever its status, says something about what was shown.
    for (const CriterionCoverage &entry : coverage) {
        CHECK_FALSE(entry.note.empty());
        CHECK_FALSE(entry.tags.empty());
        CHECK_FALSE(entry.hardware_verified);
    }
}

// --- the scenarios ----------------------------------------------------------

TEST_CASE("Stage A/13: every downward move batching produces is verified by the twin",
          "[stage-a]")
{
    // The join 25F/14 left open: the move set the acceptance suite produces is
    // now turned into g-code and interpreted, rather than only counted.
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };
    const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    std::vector<DescentRequest> requests;
    for (const ScheduledOperation &operation : plan.operations)
        if (operation.downward_move)
            requests.push_back(descent());
    REQUIRE(requests.size() == plan.metrics.downward_moves);
    REQUIRE(requests.size() == 3);

    const BatchDescentReview review = review_descents(requests, descent_context());
    REQUIRE(review.all_proven);
    REQUIRE(review.descents.size() == 3);

    const std::string gcode = gcode_for(review.descents);

    SECTION("the proven descents survive the twin")
    {
        const TwinResult result = interpret_gcode(gcode, machine(0.5), TwinPolicy {});
        CHECK(result.error.empty());
        CHECK_FALSE(has_finding(result, TwinIssue::UnsafeZ));
        CHECK_FALSE(has_finding(result, TwinIssue::WrongTool));
        CHECK_FALSE(has_finding(result, TwinIssue::ColdExtrusion));
        CHECK_FALSE(has_finding(result, TwinIssue::ZeroFeedrate));
        CHECK_FALSE(has_finding(result, TwinIssue::BadMode));
        CHECK_FALSE(has_finding(result, TwinIssue::OutOfRange));
        CHECK(result.acceptable());
        // Nine moves: three descents of three segments each.
        CHECK(result.moves >= 9);
    }

    SECTION("raise the declared safe height and the same moves are reported")
    {
        // The descents end at z = 1.0. Told that nothing may travel below 1.5,
        // the twin reports every one of them - which is what makes the check
        // above worth having.
        const TwinResult result = interpret_gcode(gcode, machine(1.5), TwinPolicy {});
        CHECK(has_finding(result, TwinIssue::UnsafeZ));
    }
}

TEST_CASE("Stage A/20: invalid feedrate and wrong-tool g-code is caught", "[stage-a]")
{
    const std::string prologue = "G21\nG90\nM82\nM109 T0 S210\nT0\n"
                                 "G1 F1200 X10 Y10 Z0.2\n";

    SECTION("the same file without the faults is acceptable")
    {
        const TwinResult result =
            interpret_gcode(prologue + "G1 F900 X20 Y10 Z0.2 E1.0\n", machine(0.1),
                            TwinPolicy {});
        CHECK(result.acceptable());
        CHECK(result.findings.empty());
    }

    SECTION("a feedrate of zero")
    {
        const TwinResult result =
            interpret_gcode(prologue + "G1 F0 X20 Y10 Z0.2 E1.0\n", machine(0.1), TwinPolicy {});
        CHECK(has_finding(result, TwinIssue::ZeroFeedrate));
        CHECK_FALSE(result.acceptable());
    }

    SECTION("extrusion on a tool that was never selected")
    {
        const TwinResult result =
            interpret_gcode("G21\nG90\nM82\nM109 T0 S210\nG1 F1200 X10 Y10 Z0.2\n"
                            "G1 F900 X20 Y10 Z0.2 E1.0\n",
                            machine(0.1), TwinPolicy {});
        CHECK(has_finding(result, TwinIssue::WrongTool));
        CHECK_FALSE(result.acceptable());
    }

    SECTION("selecting a tool the machine does not have")
    {
        const TwinResult result =
            interpret_gcode("G21\nG90\nM82\nT7\nG1 F1200 X10 Y10 Z0.2\n", machine(0.1),
                            TwinPolicy {});
        CHECK(has_finding(result, TwinIssue::WrongTool));
    }
}

TEST_CASE("Stage A/9: no layer is printed above a tool's maximum height", "[stage-a]")
{
    // The machine declares 0.3 mm. A file that steps 0.5 mm between extrusions
    // is reported, and one that steps 0.2 mm is not.
    const std::string prologue = "G21\nG90\nM82\nM109 T0 S210\nT0\n";

    const TwinResult within =
        interpret_gcode(prologue + "G1 F1200 X10 Y10 Z0.2\nG1 F900 X20 Y10 E1\n"
                                   "G0 F3000 X10 Y10 Z0.4\nG1 F900 X20 Y10 E2\n",
                        machine(0.1), TwinPolicy {});
    CHECK_FALSE(has_finding(within, TwinIssue::HeightViolation));

    const TwinResult beyond =
        interpret_gcode(prologue + "G1 F1200 X10 Y10 Z0.2\nG1 F900 X20 Y10 E1\n"
                                   "G0 F3000 X10 Y10 Z0.7\nG1 F900 X20 Y10 E2\n",
                        machine(0.1), TwinPolicy {});
    CHECK(has_finding(beyond, TwinIssue::HeightViolation));
}

TEST_CASE("Stage A/14: the same inputs schedule the same way every time", "[stage-a]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };

    const BatchPlan first  = plan_bounded_batches(towers, proven({ 0, 1 }), batching());
    const BatchPlan second = plan_bounded_batches(towers, proven({ 0, 1 }), batching());

    REQUIRE(first.operations.size() == second.operations.size());
    for (size_t i = 0; i < first.operations.size(); ++i) {
        CHECK(first.operations[i].component == second.operations[i].component);
        CHECK(first.operations[i].tool == second.operations[i].tool);
        CHECK(first.operations[i].downward_move == second.operations[i].downward_move);
    }
    CHECK(first.metrics.toolchanges == second.metrics.toolchanges);
    CHECK(first.metrics.downward_moves == second.metrics.downward_moves);

    // And the cache graph over the same stages resolves identically however it
    // is presented.
    std::vector<CacheNode> nodes = { CacheNode { 1, "slice", {}, {} },
                                     CacheNode { 2, "perimeters", { 1 }, {} },
                                     CacheNode { 3, "infill", { 1 }, {} },
                                     CacheNode { 4, "gcode", { 2, 3 }, {} } };
    const GraphResult forwards = resolve_graph(nodes);
    std::reverse(nodes.begin(), nodes.end());
    const GraphResult backwards = resolve_graph(nodes);
    REQUIRE(forwards.ok());
    REQUIRE(backwards.ok());
    CHECK(forwards.order == backwards.order);
    CHECK(forwards.keys.at(4) == backwards.keys.at(4));
}

TEST_CASE("Stage A/16: with the feature disabled, nothing new happens", "[stage-a]")
{
    const std::vector<BatchComponent> towers { tower(0, 0, 0.), tower(1, 1, 100.) };

    SECTION("batching off gives the ordinary schedule and no downward motion")
    {
        const BatchPlan plan = plan_bounded_batches(towers, proven({ 0, 1 }), batching(false));
        // No downward motion at all - that is what batching adds and what
        // turning it off must take away. The lead is not zero, and never was:
        // one layer of it is inherent to printing two objects at once, which
        // the ordinary by-layer schedule already does.
        CHECK(plan.metrics.downward_moves == 0);
        CHECK(plan.metrics.max_z_lead_mm <= 0.2 + 1e-9);

        const BatchPlan batched = plan_bounded_batches(towers, proven({ 0, 1 }), batching(true));
        CHECK(batched.metrics.downward_moves > 0);
        CHECK(batched.metrics.max_z_lead_mm > plan.metrics.max_z_lead_mm);
    }

    SECTION("a disabled cache misses everything rather than becoming a smaller one")
    {
        CachePolicy policy;
        policy.enabled  = false;
        policy.build_id = "b";
        SliceCacheStore store(policy);

        CacheEntry entry;
        entry.key.digest    = "aaaa";
        entry.stage         = "gcode";
        entry.size_bytes    = 10;
        entry.checksum      = "s";
        entry.build_id      = "b";
        entry.compatibility = CacheCompatibility::Proven;

        std::string refusal;
        CHECK_FALSE(store.store(entry, refusal));
        CHECK(store.size() == 0);
        CHECK(store.lookup(entry.key, std::string("s"), T0).verdict == LookupVerdict::Disabled);
    }
}

TEST_CASE("Stage A/19: a calibration versions and rolls back like anything else", "[stage-a]")
{
    ProfileArchive archive;

    ProfileVersionRecord first;
    first.key         = { ProfileKind::Calibration, "T0-flow", 1 };
    first.fields      = { { "multiplier", "0.98" } };
    first.reason      = "measured on the bench";
    first.source      = ProfileSource::Calibration;
    first.timestamp_s = T0;
    first.validation  = ValidationState::Valid;
    REQUIRE(archive.add(first).ok());

    ProfileVersionRecord second = first;
    second.key                  = { ProfileKind::Calibration, "T0-flow", 2 };
    second.parent_revision      = 1;
    second.fields               = { { "multiplier", "1.06" } };
    second.reason               = "re-measured after the nozzle change";
    second.timestamp_s          = T0 + 100;
    REQUIRE(archive.add(second).ok());

    const RollbackResult plan = plan_rollback(archive, ProfileKind::Calibration, "T0-flow", 1,
                                              "1.06 over-extruded", T0 + 200);
    REQUIRE(plan.ok());
    REQUIRE(plan.record.has_value());
    CHECK(plan.record->key.revision == 3);
    CHECK(plan.record->restores_revision.value() == 1);
    CHECK(plan.record->fields.at("multiplier") == "0.98");
    // Restored values have not been validated in their new context.
    CHECK(plan.record->validation == ValidationState::Unvalidated);
    REQUIRE(archive.add(*plan.record).ok());

    // Nothing was erased: all three revisions are still there and still say
    // what they said.
    CHECK(archive.history(ProfileKind::Calibration, "T0-flow").size() == 3);
    CHECK(archive.find({ ProfileKind::Calibration, "T0-flow", 2 })->fields.at("multiplier") ==
          "1.06");
}

TEST_CASE("Stage A/15: a print reloads as the print it was", "[stage-a]")
{
    ProfileArchive archive;
    ProfileVersionRecord v1;
    v1.key         = { ProfileKind::Print, "fine", 1 };
    v1.fields      = { { "layer_height", "0.20" } };
    v1.reason      = "as shipped";
    v1.source      = ProfileSource::Vendor;
    v1.timestamp_s = T0;
    v1.validation  = ValidationState::Valid;
    REQUIRE(archive.add(v1).ok());

    ProfileVersionRecord v2 = v1;
    v2.key                  = { ProfileKind::Print, "fine", 2 };
    v2.parent_revision      = 1;
    v2.fields               = { { "layer_height", "0.15" } };
    v2.timestamp_s          = T0 + 10;
    REQUIRE(archive.add(v2).ok());

    // An older project referencing revision 1 still means revision 1.
    const ProjectResolution resolution =
        resolve_project_references(archive, { { ProfileKind::Print, "fine", 1 } });
    CHECK(resolution.complete);
    CHECK(resolution.references[0].status == ReferenceStatus::Superseded);
    CHECK(resolution.references[0].usable());
    CHECK(resolution.references[0].record->fields.at("layer_height") == "0.20");

    // And a project that only names the profile does not describe a print.
    const ProjectResolution vague =
        resolve_project_references(archive, { { ProfileKind::Print, "fine", 0 } });
    CHECK_FALSE(vague.complete);
    CHECK(vague.name_only == 1);
}

TEST_CASE("Stage A/21: an incremental slice equals a clean one", "[stage-a]")
{
    std::vector<CacheNode> nodes = { CacheNode { 1, "slice", {}, {} },
                                     CacheNode { 2, "perimeters", { 1 }, {} },
                                     CacheNode { 3, "infill", { 1 }, {} },
                                     CacheNode { 4, "gcode", { 2, 3 }, {} } };
    nodes[2].inputs.push_back({ CacheInputKind::PrintProfile, "infill", "20%" });

    const GraphResult before = resolve_graph(nodes);
    REQUIRE(before.ok());

    auto run = [&](const GraphResult &graph) {
        std::vector<StageOutput> out;
        for (size_t id : graph.order)
            for (const CacheNode &node : nodes)
                if (node.id == id)
                    out.push_back({ node.stage, "out(" + graph.keys.at(id).digest + ")" });
        return out;
    };
    const std::vector<StageOutput> first = run(before);

    nodes[2].inputs[0].value = "40%";
    const InvalidationResult invalidation = invalidate(nodes, { 3 });
    REQUIRE(invalidation.ok());
    CHECK(invalidation.invalidated == std::vector<size_t> { 3, 4 });
    CHECK(invalidation.retained == std::vector<size_t> { 1, 2 });

    const GraphResult after = resolve_graph(nodes);
    REQUIRE(after.ok());

    std::vector<StageOutput> incremental;
    for (const StageOutput &output : first)
        if (output.stage == "slice" || output.stage == "perimeters")
            incremental.push_back(output);
    for (size_t id : invalidation.invalidated)
        for (const CacheNode &node : nodes)
            if (node.id == id)
                incremental.push_back({ node.stage, "out(" + after.keys.at(id).digest + ")" });

    const EquivalenceReport report = compare_slice_outputs(incremental, run(after));
    CHECK(report.equivalent);
    CHECK(report.compared == 4);
}

TEST_CASE("Stage A: the manifest of an accepted print records what it rests on", "[stage-a]")
{
    // The last link: whatever Stage A accepted, the print that comes out of it
    // says what it was made from, and says what it could not attribute.
    PrintManifest manifest;
    manifest.tigerslicer_version = "2.9.4-tiger.30";
    manifest.tigerslicer_commit  = "e638a92";
    manifest.project_name        = "two towers";
    manifest.project_hash        = "proj-1";
    manifest.models              = { { "towers.stl", "mesh-1" } };
    manifest.profiles            = { { ProfileKind::Printer, "tiger-4t", 1 },
                                     { ProfileKind::Print, "fine", 2 } };

    ToolMapping t0;
    t0.index       = 0;
    t0.material    = "PLA";
    t0.nozzle_mm   = 0.4;
    t0.offset_x_mm = 0.;
    t0.offset_y_mm = 0.;
    t0.offset_z_mm = 0.;
    manifest.tools = { t0 };

    manifest.calibration           = { { "T0-flow", "0.98" } };
    manifest.transition            = { { "PLA->PLA", "none" } };
    manifest.grids                 = { { "towers", "grid-1" } };
    manifest.ownership             = { { "walls", 0, "painted" } };
    manifest.scheduler_policy      = { { "mode", "bounded-batch" } };
    manifest.cost_model            = { { "toolchange_s", "25" } };
    manifest.thermal_model         = { { "heat_rate_c_per_s", "2" } };
    manifest.validation            = { { "descent", "proven" } };
    manifest.verifier              = { { "gcode-twin", "acceptable" } };
    manifest.estimates             = { { "time_s", "315" } };
    manifest.firmware_capabilities = { { "klipper", "0.12" } };
    manifest.macro_contracts       = { { "TOOLCHANGE", "selects and purges", true } };

    // The tool has no calibration version recorded, which is exactly the kind
    // of thing the manifest exists to surface.
    const ManifestAudit audit = audit_manifest(manifest);
    CHECK(audit.verdict == ManifestVerdict::Qualified);
    bool named = false;
    for (const std::string &finding : audit.findings)
        if (finding.find("no calibration version recorded") != std::string::npos)
            named = true;
    CHECK(named);

    // With it recorded, the same print is reproducible.
    manifest.tools[0].calibration = ProfileVersionKey { ProfileKind::Calibration, "T0-flow", 1 };
    manifest.profiles.push_back({ ProfileKind::Calibration, "T0-flow", 1 });
    CHECK(audit_manifest(manifest).reproducible());
}
