///|/ TigerSlicer Phase AUTO2: the FDM planner's mapping into the optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/FdmAutoPlan.hpp"

using namespace Slic3r;

namespace {

ToolProposal assigned(AssignmentOutcome outcome, int tool = 0)
{
    ToolProposal p;
    p.outcome       = outcome;
    p.proposed_tool = tool;
    p.current_tool  = tool;
    return p;
}

FlowResult flow_ok()
{
    FlowResult f;
    f.limit         = FlowLimit::CalibratedCurve;
    f.exceeds_limit = false;
    return f;
}

FlowResult flow_over()
{
    FlowResult f;
    f.limit         = FlowLimit::CalibratedCurve;
    f.exceeds_limit = true;
    return f;
}

FlowResult flow_uncalibrated()
{
    FlowResult f;
    f.limit         = FlowLimit::NoCalibrationForTool;
    f.exceeds_limit = false;
    return f;
}

FdmFeaturePlan feature(const std::string &name, ToolProposal assignment, FlowResult flow)
{
    FdmFeaturePlan f;
    f.feature    = 0;
    f.name       = name;
    f.assignment = assignment;
    f.flow       = flow;
    return f;
}

FdmCandidatePlan candidate(const std::string &id, std::vector<FdmFeaturePlan> features,
                           double quality = 0.7, double time_s = 200.)
{
    FdmCandidatePlan c;
    c.id                    = id;
    c.summary              = id;
    c.features             = std::move(features);
    c.predicted_quality    = quality;
    c.predicted_time_s     = time_s;
    c.predicted_toolchanges = 2.;
    c.predicted_waste_mm3   = 90.;
    return c;
}

bool has_violation(const AutoPlanCandidate &c)
{
    for (const ConstraintResult &r : c.constraints)
        if (r.outcome == ConstraintOutcome::Violated)
            return true;
    return false;
}

const ConstraintResult *constraint_named(const AutoPlanCandidate &c, const std::string &fragment)
{
    for (const ConstraintResult &r : c.constraints)
        if (r.constraint.find(fragment) != std::string::npos)
            return &r;
    return nullptr;
}

} // namespace

TEST_CASE("AUTO2: existing-engine verdicts map to hard constraints", "[fdm-auto-plan]")
{
    SECTION("a clean feature is fully satisfied and not eliminated")
    {
        const AutoPlanCandidate c = map_fdm_candidate(candidate(
            "clean", { feature("walls", assigned(AssignmentOutcome::Proposed, 0), flow_ok()) }));
        CHECK_FALSE(c.eliminated());
        CHECK_FALSE(c.blocked_unknown());
    }

    SECTION("no compatible tool is a violation - the candidate is eliminated")
    {
        const AutoPlanCandidate c = map_fdm_candidate(candidate(
            "no-tool",
            { feature("text", assigned(AssignmentOutcome::NoCompatibleTool), flow_ok()) }));
        CHECK(c.eliminated());
        CHECK(has_violation(c));
    }

    SECTION("needs-measurement is Unknown - the candidate is blocked, with the datum")
    {
        const AutoPlanCandidate c = map_fdm_candidate(candidate(
            "measure",
            { feature("slope", assigned(AssignmentOutcome::NeedsMeasurement), flow_ok()) }));
        CHECK_FALSE(c.eliminated());
        CHECK(c.blocked_unknown());
        const ConstraintResult *r = constraint_named(c, "tool for");
        REQUIRE(r != nullptr);
        CHECK(r->outcome == ConstraintOutcome::Unknown);
        CHECK_THAT(r->required_datum, Catch::Matchers::ContainsSubstring("measurement"));
    }

    SECTION("flow over the verified limit is a violation")
    {
        const AutoPlanCandidate c = map_fdm_candidate(candidate(
            "overflow",
            { feature("infill", assigned(AssignmentOutcome::Proposed, 3), flow_over()) }));
        CHECK(c.eliminated());
    }

    SECTION("no verified flow curve is Unknown, not a violation")
    {
        const AutoPlanCandidate c = map_fdm_candidate(candidate(
            "no-curve", { feature("infill", assigned(AssignmentOutcome::Proposed, 3),
                                  flow_uncalibrated()) }));
        CHECK_FALSE(c.eliminated());
        CHECK(c.blocked_unknown());
    }

    SECTION("an ambiguous assignment is a note, not a block")
    {
        const AutoPlanCandidate c = map_fdm_candidate(candidate(
            "ambig",
            { feature("wall", assigned(AssignmentOutcome::Ambiguous), flow_ok()) }));
        CHECK_FALSE(c.eliminated());
        CHECK_FALSE(c.blocked_unknown());
    }
}

TEST_CASE("AUTO2: a colour is never remapped to a visibly different filament automatically",
          "[fdm-auto-plan]")
{
    FdmFeaturePlan f = feature("logo", assigned(AssignmentOutcome::Proposed, 1), flow_ok());
    f.required_colour = "red";

    SECTION("the assigned tool loaded with the required colour is fine")
    {
        f.assigned_tool_colour = "red";
        const AutoPlanCandidate c = map_fdm_candidate(candidate("match", { f }));
        CHECK_FALSE(c.eliminated());
    }

    SECTION("a different loaded colour is a violation, not a low score")
    {
        f.assigned_tool_colour = "blue";
        const AutoPlanCandidate c = map_fdm_candidate(candidate("remap", { f }));
        CHECK(c.eliminated());
        const ConstraintResult *r = constraint_named(c, "colour for");
        REQUIRE(r != nullptr);
        CHECK_THAT(r->detail,
                   Catch::Matchers::ContainsSubstring("visibly different filament"));
    }

    SECTION("an unreported loaded colour is Unknown, so it blocks rather than assuming a match")
    {
        f.assigned_tool_colour.clear();
        const AutoPlanCandidate c = map_fdm_candidate(candidate("unknown-colour", { f }));
        CHECK(c.blocked_unknown());
    }
}

TEST_CASE("AUTO2: a candidate that would break a user lock does not respect locks",
          "[fdm-auto-plan]")
{
    SECTION("a colour lock the assignment departs from is recorded")
    {
        FdmFeaturePlan f     = feature("wall", assigned(AssignmentOutcome::Proposed, 2), flow_ok());
        f.assigned_tool_colour = "green";
        FdmFeatureLock lock;
        lock.kind  = FdmLockKind::Colour;
        lock.value = "red";
        f.lock     = lock;

        const AutoPlanCandidate c = map_fdm_candidate(candidate("breaks", { f }));
        CHECK_FALSE(c.respects_locks);
        REQUIRE_FALSE(c.would_break_locks.empty());
        CHECK_THAT(c.would_break_locks.front(),
                   Catch::Matchers::ContainsSubstring("colour lock red"));
    }

    SECTION("a tool lock the assignment honours is fine")
    {
        FdmFeaturePlan f = feature("wall", assigned(AssignmentOutcome::Proposed, 0), flow_ok());
        FdmFeatureLock lock;
        lock.kind  = FdmLockKind::Tool;
        lock.value = "0";
        f.lock     = lock;
        const AutoPlanCandidate c = map_fdm_candidate(candidate("keeps", { f }));
        CHECK(c.respects_locks);
    }
}

TEST_CASE("AUTO2: the slider moves only soft weights, and drives the whole plan through AUTO1",
          "[fdm-auto-plan]")
{
    // A fine, slow candidate and a coarse, fast one, both valid.
    const FdmCandidatePlan fine = candidate(
        "fine", { feature("walls", assigned(AssignmentOutcome::Proposed, 0), flow_ok()) },
        0.95, 400.);
    const FdmCandidatePlan fast = candidate(
        "fast", { feature("walls", assigned(AssignmentOutcome::Proposed, 3), flow_ok()) },
        0.55, 120.);

    SECTION("the finest slider position prefers the fine plan")
    {
        const AutoPlanRequest request =
            build_fdm_request({ fine, fast }, fdm_objectives_for_slider(1.0), 1);
        const AutoPlanResult result = optimize_plan(request);
        CHECK(result.ok());
        CHECK(result.winner_id.value() == "fine");
    }

    SECTION("the fastest slider position prefers the fast plan")
    {
        const AutoPlanRequest request =
            build_fdm_request({ fine, fast }, fdm_objectives_for_slider(0.0), 1);
        const AutoPlanResult result = optimize_plan(request);
        CHECK(result.ok());
        CHECK(result.winner_id.value() == "fast");
    }

    SECTION("an unsafe fast candidate never wins, whatever the slider")
    {
        const FdmCandidatePlan unsafe = candidate(
            "unsafe-fast",
            { feature("infill", assigned(AssignmentOutcome::Proposed, 3), flow_over()) },
            0.9, 10.);
        const AutoPlanRequest request =
            build_fdm_request({ fine, unsafe }, fdm_objectives_for_slider(0.0), 1);
        const AutoPlanResult result = optimize_plan(request);
        CHECK(result.ok());
        CHECK(result.winner_id.value() == "fine");
    }

    SECTION("the slider clamps out of range rather than misbehaving")
    {
        CHECK(fdm_objectives_for_slider(-5.).objectives.size() ==
              fdm_objectives_for_slider(0.5).objectives.size());
        // A quality weight is never negative.
        for (const AutoObjective &o : fdm_objectives_for_slider(-5.).objectives)
            CHECK(o.weight >= 0.);
    }
}
