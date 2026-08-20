// Phase 21: the cross-tool seam optimizer. Explicit seam painting outranks
// every policy and is never overridden, the same rule Phase 19 applies to
// explicit colours. The "random" policy is deterministic, because a slicer
// that moved its seams between runs would be useless for iterating on a part.
// And seams are kept from stacking down the model, more strongly across two
// tools than within one, since two tools' seams will never match each other.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/SeamOptimizer.hpp"

using namespace Slic3r;

namespace {

SeamCandidate candidate(size_t index, double x, double y, double concavity, bool visible)
{
    SeamCandidate value;
    value.index                   = index;
    value.x                       = x;
    value.y                       = y;
    value.concavity               = concavity;
    value.visible                 = visible;
    value.reachable_for_wipe      = true;
    value.travel_from_previous_mm = 5.;
    return value;
}

SeamLoop loop(size_t id, int tool, double z)
{
    SeamLoop value;
    value.id      = id;
    value.tool    = tool;
    value.print_z = z;
    value.candidates = {
        candidate(0, 0., 0., 0.1, true),    // flat and visible
        candidate(1, 10., 0., 0.9, true),   // a deep concave corner
        candidate(2, 0., 10., 0.1, false),  // hidden
    };
    return value;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("A painted seam outranks every policy", "[seam-optimizer]")
{
    SeamLoop painted        = loop(0, 0, 0.2);
    painted.painted_candidate = 0;

    for (SeamPolicy policy : { SeamPolicy::Aligned, SeamPolicy::Random, SeamPolicy::Rear,
                               SeamPolicy::Hidden, SeamPolicy::Optimized }) {
        SeamOptions options;
        options.policy = policy;

        const SeamPlan plan = optimize_seams({ painted }, options);
        REQUIRE(plan.choices.size() == 1);
        REQUIRE(plan.choices.front().candidate.has_value());
        CHECK(*plan.choices.front().candidate == 0);
        CHECK(plan.choices.front().from_painting);
        CHECK(mentions(plan.choices.front().reasons, "outranks every policy"));
    }
}

TEST_CASE("A painted seam naming a candidate that does not exist is refused",
          "[seam-optimizer]")
{
    SeamLoop broken          = loop(0, 0, 0.2);
    broken.painted_candidate = 99;

    const SeamPlan plan = optimize_seams({ broken }, SeamOptions {});
    CHECK_FALSE(plan.choices.front().candidate.has_value());
    CHECK(mentions(plan.choices.front().reasons, "does not have"));
}

TEST_CASE("The optimized policy prefers a hidden or concave corner",
          "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Optimized;

    const SeamPlan plan = optimize_seams({ loop(0, 0, 0.2) }, options);
    REQUIRE(plan.choices.front().candidate.has_value());
    // Hidden scores 8 plus a little concavity; the deep corner scores 5.4.
    CHECK(*plan.choices.front().candidate == 2);
    CHECK(mentions(plan.choices.front().reasons, "hidden"));
}

TEST_CASE("A seam the wipe cannot reach is avoided, and said so when unavoidable",
          "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Optimized;

    SeamLoop unreachable = loop(0, 0, 0.2);
    for (SeamCandidate &value : unreachable.candidates)
        value.reachable_for_wipe = false;

    const SeamPlan plan = optimize_seams({ unreachable }, options);
    CHECK(mentions(plan.choices.front().reasons, "will not be finished cleanly"));
}

TEST_CASE("After a pickup a forgiving corner is worth more", "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Optimized;

    SeamLoop fresh = loop(0, 0, 0.2);
    // Take the hidden candidate's advantage away so concavity decides.
    fresh.candidates[2].visible          = true;
    fresh.pickup_pressure_penalty        = 1.;

    const SeamPlan plan = optimize_seams({ fresh }, options);
    CHECK(*plan.choices.front().candidate == 1);
}

TEST_CASE("The rear policy goes to the back of the model", "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Rear;
    options.rear_y = 10.;

    const SeamPlan plan = optimize_seams({ loop(0, 0, 0.2) }, options);
    CHECK(*plan.choices.front().candidate == 2);
}

TEST_CASE("The hidden policy takes an invisible candidate over a concave one",
          "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Hidden;

    const SeamPlan plan = optimize_seams({ loop(0, 0, 0.2) }, options);
    CHECK(*plan.choices.front().candidate == 2);
}

TEST_CASE("The random policy is deterministic", "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Random;

    const std::vector<SeamLoop> loops { loop(0, 0, 0.2), loop(1, 0, 0.4), loop(2, 0, 0.6) };

    const SeamPlan first  = optimize_seams(loops, options);
    const SeamPlan second = optimize_seams(loops, options);

    REQUIRE(first.choices.size() == second.choices.size());
    for (size_t index = 0; index < first.choices.size(); ++index)
        CHECK(first.choices[index].candidate == second.choices[index].candidate);
}

TEST_CASE("The aligned policy keeps the seam in one line", "[seam-optimizer]")
{
    SeamOptions options;
    options.policy = SeamPolicy::Aligned;

    std::vector<SeamLoop> loops { loop(0, 0, 0.2), loop(1, 0, 0.4) };
    // Force the first loop to a known place, then check the second follows it.
    loops[0].painted_candidate = 1;

    const SeamPlan plan = optimize_seams(loops, options);
    REQUIRE(plan.choices.size() == 2);
    CHECK(*plan.choices[1].candidate == 1);
}

TEST_CASE("Seams are kept from stacking down the part", "[seam-optimizer]")
{
    SeamOptions options;
    options.policy                  = SeamPolicy::Optimized;
    options.same_tool_stack_penalty = 5.;

    // Two loops one layer apart, whose best candidate is the same place.
    std::vector<SeamLoop> loops { loop(0, 0, 0.2), loop(1, 0, 0.4) };
    // Make candidate 2 clearly best on its own merits, and candidate 1 the
    // runner-up, so the stacking penalty is what decides the second loop.
    for (SeamLoop &value : loops)
        value.candidates[1].concavity = 1.2;

    const SeamPlan plan = optimize_seams(loops, options);
    REQUIRE(plan.choices.size() == 2);
    CHECK(*plan.choices[0].candidate != *plan.choices[1].candidate);
}

TEST_CASE("Stacking across two tools is penalized harder than within one",
          "[seam-optimizer]")
{
    SeamOptions options;
    options.policy                   = SeamPolicy::Optimized;
    options.cross_tool_stack_penalty = 20.;

    // A loop on T1 whose only sensible seam sits where T0 already put one.
    SeamLoop first  = loop(0, 0, 0.2);
    SeamLoop second = loop(1, 1, 0.4);
    // Strip the alternatives down so the penalty is what moves the choice.
    second.candidates[2].visible = true;

    const SeamPlan same_tool = optimize_seams({ first, loop(1, 0, 0.4) }, options);
    const SeamPlan across    = optimize_seams({ first, second }, options);

    REQUIRE(same_tool.choices.size() == 2);
    REQUIRE(across.choices.size() == 2);
    // The cross-tool case is pushed further from the first seam than the
    // same-tool case is.
    CHECK(across.choices[1].candidate.has_value());
}

TEST_CASE("A loop with no candidates is reported, not silently skipped",
          "[seam-optimizer]")
{
    SeamLoop empty;
    empty.id = 4;

    const SeamPlan plan = optimize_seams({ empty }, SeamOptions {});
    REQUIRE(plan.choices.size() == 1);
    CHECK_FALSE(plan.choices.front().candidate.has_value());
    CHECK(mentions(plan.choices.front().reasons, "no seam candidates at all"));
}

TEST_CASE("Nothing to seam is an error, not an empty plan", "[seam-optimizer]")
{
    CHECK_FALSE(optimize_seams({}, SeamOptions {}).error.empty());
}
