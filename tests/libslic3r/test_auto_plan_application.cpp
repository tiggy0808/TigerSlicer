///|/ TigerSlicer Phase AUTO5: the apply-time preflight and reproducibility rule.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/AutoPlanApplication.hpp"

using namespace Slic3r;

namespace {

AutoPlanCandidate candidate(const std::string &id, std::vector<ConstraintResult> constraints)
{
    AutoPlanCandidate c;
    c.id          = id;
    c.constraints = std::move(constraints);
    return c;
}

ConstraintResult satisfied(const std::string &name)
{
    return { name, ConstraintOutcome::Satisfied, "", "" };
}

ConstraintResult violated(const std::string &name)
{
    return { name, ConstraintOutcome::Violated, "exceeds a limit", "" };
}

ConstraintResult unknown(const std::string &name, const std::string &datum)
{
    return { name, ConstraintOutcome::Unknown, "", datum };
}

PlanProvenance provenance()
{
    PlanProvenance p;
    p.planner_version   = "auto-1.0";
    p.objective_version = "obj-1.0";
    p.slider_position   = 0.7;
    p.seed              = 42;
    p.selected_plan_id  = "fine";
    p.input_hashes      = { { "geometry", "g1" }, { "print_profile", "p1" } };
    return p;
}

} // namespace

TEST_CASE("AUTO5: the preflight re-verifies the winner independently before apply",
          "[auto-apply]")
{
    SECTION("a clean plan re-verifies and may be applied")
    {
        const PreflightResult r =
            independent_preflight(candidate("ok", { satisfied("flow"), satisfied("volume") }));
        CHECK(r.may_apply());
        CHECK(r.verdict == PreflightVerdict::Clear);
    }

    SECTION("a violation on a fresh check blocks apply, even if the optimizer had chosen it")
    {
        const PreflightResult r = independent_preflight(
            candidate("bad", { satisfied("flow"), violated("max layer height") }));
        CHECK_FALSE(r.may_apply());
        CHECK(r.verdict == PreflightVerdict::Blocked);
        CHECK_FALSE(r.violations.empty());
    }

    SECTION("an unknown constraint leaves the plan unproven, distinct from blocked")
    {
        const PreflightResult r = independent_preflight(
            candidate("maybe", { satisfied("flow"), unknown("colour", "loaded colour") }));
        CHECK_FALSE(r.may_apply());
        CHECK(r.verdict == PreflightVerdict::Unproven);
        CHECK_FALSE(r.unknowns.empty());
    }

    SECTION("a broken lock blocks apply and is named")
    {
        AutoPlanCandidate c = candidate("remap", { satisfied("flow") });
        c.respects_locks    = false;
        c.would_break_locks = { "T0 colour lock" };
        const PreflightResult r = independent_preflight(c);
        CHECK(r.verdict == PreflightVerdict::Blocked);
        CHECK(r.broken_locks == std::vector<std::string> { "T0 colour lock" });
    }

    SECTION("a violation outweighs an unknown in the verdict")
    {
        const PreflightResult r = independent_preflight(
            candidate("both", { violated("z"), unknown("flow", "curve") }));
        CHECK(r.verdict == PreflightVerdict::Blocked);
    }
}

TEST_CASE("AUTO5: preflighting a result ties the winner to its candidate set", "[auto-apply]")
{
    std::vector<AutoPlanCandidate> candidates = {
        candidate("fine", { satisfied("all") }),
        candidate("fast", { satisfied("all") }),
    };

    SECTION("the named winner is found and re-verified")
    {
        AutoPlanResult result;
        result.winner_id = std::string("fine");
        CHECK(preflight_result(result, candidates).may_apply());
    }

    SECTION("a result with no winner cannot be applied")
    {
        AutoPlanResult result;
        CHECK(preflight_result(result, candidates).verdict == PreflightVerdict::Blocked);
    }

    SECTION("a winner not in the candidate set is refused, not applied blindly")
    {
        AutoPlanResult result;
        result.winner_id = std::string("from-another-run");
        const PreflightResult r = preflight_result(result, candidates);
        CHECK(r.verdict == PreflightVerdict::Blocked);
        CHECK_THAT(r.message, Catch::Matchers::ContainsSubstring("not among the candidates"));
    }
}

TEST_CASE("AUTO5: an old project reproduces its plan or requests migration, never reinterprets",
          "[auto-apply]")
{
    const PlanProvenance stored = provenance();

    SECTION("matching versions and inputs reproduce the plan exactly")
    {
        const ReproductionResult r = decide_reproduction(
            stored, "auto-1.0", "obj-1.0", { { "geometry", "g1" }, { "print_profile", "p1" } });
        CHECK(r.reproduces());
    }

    SECTION("a changed input recomputes the plan, it is not kept")
    {
        const ReproductionResult r = decide_reproduction(
            stored, "auto-1.0", "obj-1.0", { { "geometry", "g2" }, { "print_profile", "p1" } });
        CHECK(r.decision == ReproductionDecision::InputsChanged);
        CHECK(r.changed_inputs == std::vector<std::string> { "geometry" });
    }

    SECTION("a new planner version requests migration, never silent reinterpretation")
    {
        const ReproductionResult r = decide_reproduction(
            stored, "auto-2.0", "obj-1.0", { { "geometry", "g1" }, { "print_profile", "p1" } });
        CHECK(r.decision == ReproductionDecision::MigrationRequired);
        CHECK_THAT(r.message,
                   Catch::Matchers::ContainsSubstring("reinterpreting the stored slider"));
    }

    SECTION("a new objective-weight model likewise requests migration")
    {
        const ReproductionResult r = decide_reproduction(
            stored, "auto-1.0", "obj-2.0", { { "geometry", "g1" }, { "print_profile", "p1" } });
        CHECK(r.decision == ReproductionDecision::MigrationRequired);
    }

    SECTION("inputs are checked before versions: a changed input under a new planner recomputes")
    {
        // Both changed. Inputs-changed wins, because the plan does not describe
        // this project regardless of the planner.
        const ReproductionResult r = decide_reproduction(
            stored, "auto-2.0", "obj-1.0", { { "geometry", "g9" }, { "print_profile", "p1" } });
        CHECK(r.decision == ReproductionDecision::InputsChanged);
    }

    SECTION("a stored input the project no longer provides is a change")
    {
        const ReproductionResult r =
            decide_reproduction(stored, "auto-1.0", "obj-1.0", { { "geometry", "g1" } });
        CHECK(r.decision == ReproductionDecision::InputsChanged);
        CHECK(r.changed_inputs == std::vector<std::string> { "print_profile" });
    }
}
