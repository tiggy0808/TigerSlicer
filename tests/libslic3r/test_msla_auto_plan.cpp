///|/ TigerSlicer Phase AUTO3: the MSLA planner's mapping into the optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/MslaAutoPlan.hpp"

using namespace Slic3r;

namespace {

ResinAnalysis clean_analysis()
{
    return build_analysis({}, AnalysisState::Complete, "hash-1");
}

ResinAnalysis analysis_with_island()
{
    ResinIssue issue;
    issue.kind                 = ResinIssueKind::Island;
    issue.location.layer_index = 5;
    return build_analysis({ issue }, AnalysisState::Complete, "hash-1");
}

MslaCandidatePlan candidate(const std::string &id, MslaExposureSource source,
                            ResinAnalysis analysis, double quality = 0.8, double time_s = 3000.)
{
    MslaCandidatePlan c;
    c.id                       = id;
    c.summary                 = id;
    c.analysis                = std::move(analysis);
    c.exposure_source         = source;
    c.resin                   = "Standard Grey";
    c.printer                 = "Saturn";
    c.layer_height_supported  = true;
    c.trapped_resin           = ValidationState3::Passed;
    c.predicted_quality       = quality;
    c.predicted_support_burden = 0.3;
    c.predicted_time_s        = time_s;
    c.predicted_peel_risk     = 0.2;
    return c;
}

const ConstraintResult *constraint_named(const AutoPlanCandidate &c, const std::string &name)
{
    for (const ConstraintResult &r : c.constraints)
        if (r.constraint == name)
            return &r;
    return nullptr;
}

} // namespace

TEST_CASE("AUTO3: exposure comes only from an audited preset", "[msla-auto-plan]")
{
    SECTION("an audited preset is safe")
    {
        const AutoPlanCandidate c =
            map_msla_candidate(candidate("ok", MslaExposureSource::AuditedPreset, clean_analysis()));
        CHECK_FALSE(c.eliminated());
        CHECK_FALSE(c.blocked_unknown());
    }

    SECTION("no audited preset stops before slicing and requests it - Unknown, not a guess")
    {
        const AutoPlanCandidate c =
            map_msla_candidate(candidate("no-preset", MslaExposureSource::None, clean_analysis()));
        CHECK_FALSE(c.eliminated());
        CHECK(c.blocked_unknown());
        const ConstraintResult *r = constraint_named(c, "exposure preset");
        REQUIRE(r != nullptr);
        CHECK(r->outcome == ConstraintOutcome::Unknown);
        CHECK_THAT(r->required_datum,
                   Catch::Matchers::ContainsSubstring("audited exposure preset"));
    }

    SECTION("a resin substituted by similarity is forbidden - Violated")
    {
        const AutoPlanCandidate c = map_msla_candidate(
            candidate("similar", MslaExposureSource::SubstitutedBySimilarity, clean_analysis()));
        CHECK(c.eliminated());
        const ConstraintResult *r = constraint_named(c, "exposure preset");
        CHECK_THAT(r->detail, Catch::Matchers::ContainsSubstring("similar resin"));
    }

    SECTION("exposure derived from geometry is forbidden - Violated")
    {
        const AutoPlanCandidate c = map_msla_candidate(
            candidate("geometry", MslaExposureSource::DerivedFromGeometry, clean_analysis()));
        CHECK(c.eliminated());
        const ConstraintResult *r = constraint_named(c, "exposure preset");
        CHECK_THAT(r->detail, Catch::Matchers::ContainsSubstring("derived from geometry"));
    }
}

TEST_CASE("AUTO3: a blocking resin issue eliminates the candidate", "[msla-auto-plan]")
{
    SECTION("an island (or trap/suction) is a violation")
    {
        const AutoPlanCandidate c = map_msla_candidate(
            candidate("islanded", MslaExposureSource::AuditedPreset, analysis_with_island()));
        CHECK(c.eliminated());
        const ConstraintResult *r = constraint_named(c, "resin analysis");
        CHECK(r->outcome == ConstraintOutcome::Violated);
    }

    SECTION("an incomplete analysis has not established safety - Unknown")
    {
        const AutoPlanCandidate c = map_msla_candidate(candidate(
            "capped", MslaExposureSource::AuditedPreset,
            build_analysis({}, AnalysisState::Capped, "hash-1")));
        CHECK(c.blocked_unknown());
        CHECK(constraint_named(c, "resin analysis")->outcome == ConstraintOutcome::Unknown);
    }
}

TEST_CASE("AUTO3: layer height and trapped resin are hard-gated", "[msla-auto-plan]")
{
    SECTION("an unsupported layer height is a violation")
    {
        MslaCandidatePlan plan =
            candidate("bad-height", MslaExposureSource::AuditedPreset, clean_analysis());
        plan.layer_height_supported = false;
        CHECK(map_msla_candidate(plan).eliminated());
    }

    SECTION("a part that would trap resin is a violation")
    {
        MslaCandidatePlan plan =
            candidate("trap", MslaExposureSource::AuditedPreset, clean_analysis());
        plan.trapped_resin = ValidationState3::Failed;
        CHECK(map_msla_candidate(plan).eliminated());
    }

    SECTION("un-validated drainage is Unknown, not a silent pass")
    {
        MslaCandidatePlan plan =
            candidate("unchecked-drain", MslaExposureSource::AuditedPreset, clean_analysis());
        plan.trapped_resin = ValidationState3::NotValidated;
        CHECK(map_msla_candidate(plan).blocked_unknown());
    }
}

TEST_CASE("AUTO3: the planner drives the whole plan through AUTO1 with its own objectives",
          "[msla-auto-plan]")
{
    const MslaCandidatePlan fine =
        candidate("fine", MslaExposureSource::AuditedPreset, clean_analysis(), 0.95, 5000.);
    const MslaCandidatePlan fast =
        candidate("fast", MslaExposureSource::AuditedPreset, clean_analysis(), 0.6, 1500.);

    SECTION("finest prefers the fine plan, fastest the fast one")
    {
        CHECK(optimize_plan(build_msla_request({ fine, fast },
                                               msla_objectives_for_slider(1.0), 1))
                  .winner_id.value() == "fine");
        CHECK(optimize_plan(build_msla_request({ fine, fast },
                                               msla_objectives_for_slider(0.0), 1))
                  .winner_id.value() == "fast");
    }

    SECTION("a whole plate with no audited preset is blocked on requirements, not sliced")
    {
        const MslaCandidatePlan a = candidate("a", MslaExposureSource::None, clean_analysis());
        const MslaCandidatePlan b = candidate("b", MslaExposureSource::None, clean_analysis());
        const AutoPlanResult result =
            optimize_plan(build_msla_request({ a, b }, msla_objectives_for_slider(0.5), 1));
        CHECK(result.verdict == AutoPlanVerdict::BlockedOnRequirements);
        CHECK_FALSE(result.requirements.empty());
    }

    SECTION("a fast plan with a blocking issue never wins")
    {
        const MslaCandidatePlan unsafe = candidate(
            "unsafe-fast", MslaExposureSource::AuditedPreset, analysis_with_island(), 0.9, 500.);
        const AutoPlanResult result = optimize_plan(
            build_msla_request({ fine, unsafe }, msla_objectives_for_slider(0.0), 1));
        CHECK(result.ok());
        CHECK(result.winner_id.value() == "fine");
    }
}
