///|/ TigerSlicer Phase AUTO0/AUTO1: the shared deterministic optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/AutoPlanOptimizer.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

ObjectiveVector quality_and_speed()
{
    ObjectiveVector v;
    v.objectives = {
        { "quality", ObjectiveDirection::HigherIsBetter, 1. },
        { "time_s", ObjectiveDirection::LowerIsBetter, 1. },
    };
    return v;
}

ConstraintResult satisfied(const std::string &name)
{
    return { name, ConstraintOutcome::Satisfied, "", "" };
}

ConstraintResult violated(const std::string &name)
{
    return { name, ConstraintOutcome::Violated, "exceeds a verified limit", "" };
}

ConstraintResult unknown(const std::string &name, const std::string &datum)
{
    return { name, ConstraintOutcome::Unknown, "needs " + datum, datum };
}

AutoPlanCandidate candidate(const std::string &id, double quality, double time_s,
                            std::vector<ConstraintResult> constraints = {})
{
    AutoPlanCandidate c;
    c.id          = id;
    c.summary     = id;
    c.objectives  = { { "quality", quality }, { "time_s", time_s } };
    c.constraints = std::move(constraints);
    if (c.constraints.empty())
        c.constraints = { satisfied("all limits") };
    return c;
}

bool contains(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("AUTO1: the winner is the best-scoring valid candidate, ties broken by id",
          "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    request.candidates = {
        candidate("fast", 0.5, 100.),
        candidate("fine", 0.9, 400.),
    };
    // Equal weights: quality - time. fast: 0.5-100, fine: 0.9-400. fast wins.
    const AutoPlanResult result = optimize_plan(request);
    CHECK(result.ok());
    CHECK(result.winner_id.value() == "fast");

    SECTION("weighting toward quality changes the winner")
    {
        request.objectives.objectives[0].weight = 1000.; // quality dominates
        const AutoPlanResult quality_first = optimize_plan(request);
        CHECK(quality_first.winner_id.value() == "fine");
    }

    SECTION("a genuine score tie is broken by id, deterministically")
    {
        AutoPlanRequest tie;
        tie.objectives = quality_and_speed();
        tie.candidates = { candidate("b", 0.5, 100.), candidate("a", 0.5, 100.) };
        const AutoPlanResult first  = optimize_plan(tie);
        std::reverse(tie.candidates.begin(), tie.candidates.end());
        const AutoPlanResult second = optimize_plan(tie);
        CHECK(first.winner_id.value() == "a");
        CHECK(second.winner_id.value() == "a");
    }
}

TEST_CASE("AUTO1: safety is never inferred from a score - invalid candidates are eliminated first",
          "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    // The highest-scoring candidate violates a hard limit. It must not win.
    request.candidates = {
        candidate("unsafe-but-great", 10.0, 1.,
                  { violated("max layer height for the tool") }),
        candidate("safe", 0.6, 200.),
    };
    const AutoPlanResult result = optimize_plan(request);
    CHECK(result.ok());
    CHECK(result.winner_id.value() == "safe");
    // The eliminated one is explained, not silently dropped.
    bool named = false;
    for (const RejectedCandidate &r : result.explanation.rejected)
        if (r.id == "unsafe-but-great") {
            named = true;
            CHECK_THAT(r.reason, Catch::Matchers::ContainsSubstring("violated"));
        }
    CHECK(named);
}

TEST_CASE("AUTO1: no valid candidate blocks automatic slicing", "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    request.candidates = {
        candidate("a", 0.9, 100., { violated("build volume") }),
        candidate("b", 0.8, 120., { violated("flow ceiling") }),
    };
    const AutoPlanResult result = optimize_plan(request);
    CHECK(result.verdict == AutoPlanVerdict::NoValidCandidate);
    CHECK_FALSE(result.winner_id.has_value());
    CHECK_THAT(result.summary, Catch::Matchers::ContainsSubstring("blocked"));
}

TEST_CASE("AUTO1: unknown required data opens a requirements panel, not a low score",
          "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    request.candidates = {
        candidate("needs-flow", 0.9, 100.,
                  { satisfied("volume"), unknown("flow curve for T3", "T3 flow curve") }),
        candidate("needs-material", 0.8, 120.,
                  { unknown("loaded material for T1", "T1 loaded material") }),
    };
    const AutoPlanResult result = optimize_plan(request);
    CHECK(result.verdict == AutoPlanVerdict::BlockedOnRequirements);
    CHECK_FALSE(result.winner_id.has_value());
    // Both required data are surfaced, de-duplicated, for one short panel.
    REQUIRE(result.requirements.size() == 2);
    std::vector<std::string> data;
    for (const RequirementGap &gap : result.requirements)
        data.push_back(gap.datum);
    CHECK(contains(data, "T3 flow curve"));
    CHECK(contains(data, "T1 loaded material"));

    SECTION("a valid candidate outweighs a blocked one: the plan is made, not blocked")
    {
        request.candidates.push_back(candidate("ready", 0.7, 150.));
        const AutoPlanResult with_ready = optimize_plan(request);
        CHECK(with_ready.ok());
        CHECK(with_ready.winner_id.value() == "ready");
    }

    SECTION("a violated candidate is eliminated, not treated as merely blocked")
    {
        // 'both' has an unknown AND a violation; the violation wins - it is
        // eliminated, never surfaced as a requirement to fix.
        AutoPlanRequest mixed;
        mixed.objectives = quality_and_speed();
        mixed.candidates = { candidate("both", 0.9, 100.,
                                       { violated("z height"),
                                         unknown("flow", "flow curve") }) };
        const AutoPlanResult result = optimize_plan(mixed);
        CHECK(result.verdict == AutoPlanVerdict::NoValidCandidate);
        CHECK(result.requirements.empty());
    }
}

TEST_CASE("AUTO1: a user lock is honoured over a better score, and explained", "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();

    AutoPlanCandidate breaks_lock = candidate("faster-remap", 0.9, 80.);
    breaks_lock.respects_locks    = false;
    breaks_lock.would_break_locks = { "T0 colour lock (red)" };

    request.candidates = { breaks_lock, candidate("keeps-lock", 0.6, 200.) };

    const AutoPlanResult result = optimize_plan(request);
    CHECK(result.ok());
    CHECK(result.winner_id.value() == "keeps-lock");
    bool explained = false;
    for (const RejectedCandidate &r : result.explanation.rejected)
        if (r.id == "faster-remap") {
            explained = true;
            CHECK_THAT(r.reason, Catch::Matchers::ContainsSubstring("T0 colour lock"));
        }
    CHECK(explained);
}

TEST_CASE("AUTO1: the Pareto front retains the non-dominated valid candidates", "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    request.candidates = {
        candidate("fast", 0.5, 100.),  // best time
        candidate("fine", 0.9, 400.),  // best quality
        candidate("dominated", 0.4, 500.), // worse than fast on both
    };
    const AutoPlanResult result = optimize_plan(request);
    CHECK(contains(result.pareto_front, "fast"));
    CHECK(contains(result.pareto_front, "fine"));
    CHECK_FALSE(contains(result.pareto_front, "dominated"));

    // dominates() is directly checkable.
    CHECK(dominates(candidate("fast", 0.5, 100.), candidate("dominated", 0.4, 500.),
                    request.objectives));
    CHECK_FALSE(dominates(candidate("fast", 0.5, 100.), candidate("fine", 0.9, 400.),
                          request.objectives));
}

TEST_CASE("AUTO1: on the deadline it falls back to the best fully-evaluated valid candidate",
          "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives      = quality_and_speed();
    request.budget.max_candidates = 2;
    // Sorted by id: "a", "b", "z-best". The budget of 2 stops before "z-best",
    // even though it would have scored highest.
    request.candidates = {
        candidate("a", 0.5, 100.),
        candidate("b", 0.6, 90.),
        candidate("z-best", 5.0, 1.),
    };
    const AutoPlanResult result = optimize_plan(request);
    CHECK(result.ok());
    CHECK(result.deadline_fallback);
    CHECK(result.evaluated == 2);
    // The winner is the best of the two evaluated, never the un-evaluated
    // "z-best" - a partially validated plan is not returned.
    CHECK(result.winner_id.value() != "z-best");
    CHECK_THAT(result.summary, Catch::Matchers::ContainsSubstring("before the deadline"));
}

TEST_CASE("AUTO1: the same request plans identically every time", "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    request.seed       = 42;
    request.candidates = { candidate("a", 0.5, 100.), candidate("b", 0.9, 400.),
                           candidate("c", 0.7, 200.) };

    const AutoPlanResult first = optimize_plan(request);
    std::reverse(request.candidates.begin(), request.candidates.end());
    const AutoPlanResult second = optimize_plan(request);

    CHECK(first.winner_id == second.winner_id);
    CHECK(first.pareto_front == second.pareto_front);
    CHECK(first.verdict == second.verdict);
}

TEST_CASE("AUTO1: nothing to plan is reported, not a crash", "[auto-plan]")
{
    AutoPlanRequest request;
    request.objectives = quality_and_speed();
    CHECK(optimize_plan(request).verdict == AutoPlanVerdict::NoCandidates);
}
