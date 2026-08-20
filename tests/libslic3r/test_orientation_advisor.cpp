// Phase 24: tool-aware object orientation. Two rules carry this phase. Nothing
// is ever rotated here - the advisor proposes, and a separate acceptance step
// is the only thing that produces a transform. And domination is only claimed
// between two orientations that were measured on every objective: an
// orientation nobody finished evaluating is not quietly dropped, and it is not
// quietly promoted either - it lands on the front marked unproven, because
// nothing could be shown to beat it rather than because it was shown to be
// good.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/OrientationAdvisor.hpp"

using namespace Slic3r;

namespace {

// A fully measured orientation, where a higher quality is better on every
// single objective at once.
OrientationCandidate full(size_t id, double quality)
{
    OrientationCandidate value;
    value.id     = id;
    value.rx_deg = double(id) * 90.;

    const double worse = 1. - quality;
    value.support_volume_mm3          = 1000. * worse;
    value.support_interface_area_mm2  = 500. * worse;
    value.print_time_s                = 600. + 3600. * worse;
    value.waste_mm3                   = 100. * worse;
    value.toolchanges                 = size_t(20. * worse);
    value.unreachable_detail_regions  = size_t(4. * worse);
    value.color_boundaries_crossed    = size_t(6. * worse);
    value.unanchored_bridges          = size_t(3. * worse);
    value.visible_surface_quality     = quality;
    value.strength_alignment          = quality;
    return value;
}

const OrientationAssessment *assessment_of(const OrientationAdvice &advice, size_t id)
{
    for (const OrientationAssessment &assessment : advice.assessments)
        if (assessment.candidate == id)
            return &assessment;
    return nullptr;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

bool contains(const std::vector<size_t> &values, size_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

TEST_CASE("An orientation better on every count dominates one that is worse",
          "[orientation]")
{
    const OrientationAdvice advice =
        advise_orientation({ full(0, 0.9), full(1, 0.2) }, OrientationPolicy {});

    REQUIRE(advice.recommended.size() == 1);
    CHECK(advice.recommended.front() == 0);
    CHECK(assessment_of(advice, 0)->pareto_rank == 1);
    CHECK(assessment_of(advice, 1)->pareto_rank == 2);
    CHECK(contains(assessment_of(advice, 1)->dominated_by, 0));
}

TEST_CASE("An unmeasured objective blocks domination in both directions",
          "[orientation]")
{
    OrientationCandidate incomplete = full(1, 0.2);
    incomplete.print_time_s.reset();

    const OrientationAdvice advice =
        advise_orientation({ full(0, 0.9), incomplete }, OrientationPolicy {});

    // The worse orientation is not dropped, because it was never shown to lose.
    CHECK(advice.recommended.size() == 2);
    const OrientationAssessment *unfinished = assessment_of(advice, 1);
    REQUIRE(unfinished != nullptr);
    CHECK(unfinished->verdict == OrientationVerdict::Unproven);
    CHECK(unfinished->dominated_by.empty());
    CHECK(mentions(unfinished->unknowns, "print time"));
    CHECK(mentions(unfinished->reasons, "cannot be shown to beat or lose"));
    CHECK(mentions(advice.notes, "not because it was shown to be good"));
}

TEST_CASE("A genuine trade-off leaves both orientations on the front",
          "[orientation]")
{
    OrientationCandidate quick = full(1, 0.2);
    quick.print_time_s         = 100.; // worse everywhere else, but fastest

    const OrientationAdvice advice =
        advise_orientation({ full(0, 0.9), quick }, OrientationPolicy {});

    CHECK(advice.recommended.size() == 2);
    CHECK(mentions(advice.notes, "judgement call"));
    CHECK(mentions(advice.notes, "left to you"));
}

TEST_CASE("Strengths and weaknesses are named against the field", "[orientation]")
{
    const OrientationAdvice advice =
        advise_orientation({ full(0, 0.9), full(1, 0.2) }, OrientationPolicy {});

    CHECK(mentions(assessment_of(advice, 0)->strengths, "least support material"));
    CHECK(mentions(assessment_of(advice, 0)->strengths, "lines the layers up best"));
    CHECK(mentions(assessment_of(advice, 1)->weaknesses, "most support material"));
    CHECK(mentions(assessment_of(advice, 1)->weaknesses, "prints slowest"));
}

TEST_CASE("An orientation that collides is excluded and says why", "[orientation]")
{
    OrientationCandidate blocked = full(1, 0.99);
    blocked.collides             = true;
    blocked.collision_reason     = "the parked tool sweeps the model";

    const OrientationAdvice advice =
        advise_orientation({ full(0, 0.2), blocked }, OrientationPolicy {});

    const OrientationAssessment *excluded = assessment_of(advice, 1);
    REQUIRE(excluded != nullptr);
    CHECK(excluded->verdict == OrientationVerdict::Excluded);
    CHECK(excluded->pareto_rank == 0);
    CHECK_FALSE(contains(advice.recommended, 1));
    CHECK(mentions(excluded->reasons, "the parked tool sweeps the model"));
}

TEST_CASE("An unchecked collision is unproven, not clear", "[orientation]")
{
    OrientationCandidate unchecked  = full(0, 0.9);
    unchecked.collision_checked     = false;

    const OrientationAdvice strict =
        advise_orientation({ unchecked, full(1, 0.2) }, OrientationPolicy {});
    CHECK(assessment_of(strict, 0)->verdict == OrientationVerdict::Unproven);
    CHECK(mentions(assessment_of(strict, 0)->reasons, "never checked against the toolhead"));

    OrientationPolicy relaxed;
    relaxed.require_collision_check = false;

    const OrientationAdvice lenient =
        advise_orientation({ unchecked, full(1, 0.2) }, relaxed);
    CHECK(assessment_of(lenient, 0)->verdict == OrientationVerdict::Ranked);
}

TEST_CASE("Advice never rotates anything", "[orientation]")
{
    const OrientationAdvice advice =
        advise_orientation({ full(0, 0.9), full(1, 0.2) }, OrientationPolicy {});

    CHECK_FALSE(advice.applied);
    CHECK(mentions(advice.notes, "nothing has been rotated"));
}

TEST_CASE("An orientation is applied only when the user accepts it", "[orientation]")
{
    const std::vector<OrientationCandidate> candidates { full(0, 0.9), full(1, 0.2) };
    const OrientationAdvice advice = advise_orientation(candidates, OrientationPolicy {});

    AcceptedOrientation accepted;
    std::string         error;

    SECTION("refused without acceptance") {
        CHECK_FALSE(accept_orientation(advice, candidates, 0, false, &accepted, &error));
        CHECK(error.find("never rotates on its own") != std::string::npos);
    }

    SECTION("refused for an orientation that does not exist") {
        CHECK_FALSE(accept_orientation(advice, candidates, 7, true, &accepted, &error));
        CHECK(error.find("no orientation 7") != std::string::npos);
    }

    SECTION("applied when accepted") {
        REQUIRE(accept_orientation(advice, candidates, 1, true, &accepted, &error));
        CHECK(accepted.candidate == 1);
        CHECK(accepted.rx_deg == Catch::Approx(90.));
    }
}

TEST_CASE("An excluded orientation is not applied even when asked for by number",
          "[orientation]")
{
    OrientationCandidate blocked = full(1, 0.99);
    blocked.collides             = true;

    const std::vector<OrientationCandidate> candidates { full(0, 0.5), blocked };
    const OrientationAdvice advice = advise_orientation(candidates, OrientationPolicy {});

    AcceptedOrientation accepted;
    std::string         error;
    CHECK_FALSE(accept_orientation(advice, candidates, 1, true, &accepted, &error));
    CHECK(error.find("cannot be printed") != std::string::npos);
}

TEST_CASE("An analysis is only reused for the same model, machine and objectives",
          "[orientation]")
{
    const OrientationPolicy policy;

    const std::string base = orientation_cache_key("abc123", { 0, 1, 2 }, policy);
    CHECK(base == orientation_cache_key("abc123", { 2, 1, 0 }, policy));
    CHECK(base != orientation_cache_key("abc124", { 0, 1, 2 }, policy));
    // A different machine must never be handed the same analysis.
    CHECK(base != orientation_cache_key("abc123", { 0, 1 }, policy));

    OrientationPolicy without_strength   = policy;
    without_strength.consider_strength   = false;
    CHECK(base != orientation_cache_key("abc123", { 0, 1, 2 }, without_strength));
}

TEST_CASE("A capped analysis says it stopped rather than looking complete",
          "[orientation]")
{
    OrientationPolicy policy;
    policy.max_candidates = 2;

    const OrientationAdvice advice = advise_orientation(
        { full(0, 0.9), full(1, 0.7), full(2, 0.5), full(3, 0.2) }, policy);

    CHECK(advice.assessments.size() == 2);
    CHECK(mentions(advice.notes, "were not ranked at all"));
}

TEST_CASE("When everything collides, nothing is recommended", "[orientation]")
{
    OrientationCandidate one = full(0, 0.9);
    OrientationCandidate two = full(1, 0.2);
    one.collides             = true;
    two.collides             = true;

    const OrientationAdvice advice = advise_orientation({ one, two }, OrientationPolicy {});
    CHECK(advice.recommended.empty());
    CHECK(mentions(advice.notes, "every orientation examined collides"));
}

TEST_CASE("Nothing to rank, and nothing to rank by, are both errors", "[orientation]")
{
    CHECK_FALSE(advise_orientation({}, OrientationPolicy {}).error.empty());

    OrientationPolicy nothing;
    nothing.consider_support          = false;
    nothing.consider_time             = false;
    nothing.consider_waste            = false;
    nothing.consider_finish           = false;
    nothing.consider_strength         = false;
    nothing.consider_toolchanges      = false;
    nothing.consider_detail_access    = false;
    nothing.consider_color_boundaries = false;
    nothing.consider_bridges          = false;

    CHECK_FALSE(advise_orientation({ full(0, 0.5) }, nothing).error.empty());
}
