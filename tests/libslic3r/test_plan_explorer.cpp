///|/ TigerSlicer Phase 28: quality/time/waste/risk Pareto explorer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/PlanExplorer.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

// A plan with every metric measured, so a fixture only has to say what makes
// it different. Nothing in the library fills these in; a metric left alone
// here is deliberately absent.
PlanCandidate full_plan(size_t id, PlanObjective objective, double time_s, double toolchanges,
                        double heating_s, double purge, double support, double flow,
                        double detail, double calibration, double collision)
{
    PlanCandidate plan;
    plan.id                          = id;
    plan.objective                   = objective;
    plan.label                       = plan_objective_name(objective);
    plan.metrics.time_s              = time_s;
    plan.metrics.toolchanges         = toolchanges;
    plan.metrics.heating_s           = heating_s;
    plan.metrics.purge_waste_mm3     = purge;
    plan.metrics.support_waste_mm3   = support;
    plan.metrics.flow_utilization    = flow;
    plan.metrics.detail              = detail;
    plan.metrics.calibration_margin  = calibration;
    plan.metrics.collision_margin_mm = collision;
    plan.metrics.filament_mm3_by_tool = { { 0, 1000. }, { 1, 500. } };
    plan.metrics.filament_complete    = true;
    return plan;
}

// Two plans that each win something: the first is faster, the second finer.
std::vector<PlanCandidate> tradeoff_pair()
{
    return { full_plan(1, PlanObjective::Fastest, 600., 3., 60., 270., 100., 0.80, 0.50, 0.90,
                       2.0),
             full_plan(2, PlanObjective::Finest, 900., 19., 240., 1710., 100., 0.60, 0.95, 0.90,
                       2.0) };
}

const ObjectiveOutcome *outcome_for(const PlanExploration &exploration, PlanObjective objective)
{
    for (const ObjectiveOutcome &o : exploration.objectives)
        if (o.objective == objective)
            return &o;
    return nullptr;
}

bool contains(const std::vector<size_t> &v, size_t id)
{
    return std::find(v.begin(), v.end(), id) != v.end();
}

const Compromise *compromise_for(const std::vector<Compromise> &list, const std::string &metric)
{
    for (const Compromise &c : list)
        if (c.metric == metric)
            return &c;
    return nullptr;
}

} // namespace

TEST_CASE("28: the nine reported metrics have declared directions", "[plan-explorer]")
{
    const std::vector<PlanMetricSpec> &specs = plan_metric_specs();
    REQUIRE(specs.size() == 9);
    CHECK(std::string(specs[0].name) == "time");
    CHECK(specs[0].direction == PlanMetricDirection::LowerIsBetter);
    CHECK(std::string(specs[5].name) == "flow utilization");
    CHECK(specs[5].direction == PlanMetricDirection::HigherIsBetter);
    CHECK(std::string(specs[8].name) == "collision margin");
    CHECK(specs[8].direction == PlanMetricDirection::HigherIsBetter);
}

TEST_CASE("28: dominance is unprovable when a metric was never measured", "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();

    SECTION("a plan better at everything dominates")
    {
        PlanCandidate better = full_plan(3, PlanObjective::Balanced, 500., 2., 50., 200., 90.,
                                         0.85, 0.99, 0.95, 3.0);
        DominanceResult result = dominance(better, plans[1]);
        CHECK(result.verdict == DominanceVerdict::Dominates);
        CHECK(result.worse.empty());
        CHECK(result.better.size() == 9);
    }

    SECTION("a plan better at one thing and worse at another does not dominate")
    {
        DominanceResult result = dominance(plans[0], plans[1]);
        CHECK(result.verdict == DominanceVerdict::DoesNotDominate);
        // It wins on time and waste, and loses on detail. Both lists are kept,
        // so nothing is presented as an unqualified win.
        CHECK_FALSE(result.better.empty());
        CHECK_FALSE(result.worse.empty());
    }

    SECTION("one unmeasured metric makes the whole comparison unprovable")
    {
        PlanCandidate incomplete = full_plan(3, PlanObjective::Balanced, 500., 2., 50., 200., 90.,
                                             0.85, 0.96, 0.95, 3.0);
        incomplete.metrics.detail.reset();
        DominanceResult result = dominance(incomplete, plans[1]);
        CHECK(result.verdict == DominanceVerdict::Unprovable);
        REQUIRE(result.unmeasured.size() == 1);
        CHECK_THAT(result.unmeasured[0],
                   Catch::Matchers::ContainsSubstring("the first plan did not measure it"));
    }

    SECTION("an identical plan does not dominate its twin")
    {
        DominanceResult result = dominance(plans[0], plans[0]);
        CHECK(result.verdict == DominanceVerdict::DoesNotDominate);
        CHECK(result.better.empty());
    }
}

TEST_CASE("28: a trade-off is never resolved into a recommendation", "[plan-explorer]")
{
    ExplorerPolicy  policy;
    PlanExploration exploration = explore_plans(tradeoff_pair(), policy);

    CHECK(exploration.feasible.size() == 2);
    CHECK(exploration.frontier.size() == 2);
    CHECK(exploration.dominated.empty());
    CHECK_FALSE(exploration.recommended.has_value());
    CHECK_THAT(exploration.no_recommendation_reason,
               Catch::Matchers::ContainsSubstring("not a decision this can make"));
}

TEST_CASE("28: a plan that is worse at everything is off the frontier and says who beat it",
          "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    // Worse than plan 1 on every axis, and not better than it on any.
    plans.push_back(full_plan(3, PlanObjective::Balanced, 1200., 25., 300., 2000., 200., 0.40,
                              0.30, 0.50, 1.0));

    PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});

    CHECK(exploration.frontier.size() == 2);
    CHECK(contains(exploration.frontier, 1));
    CHECK(contains(exploration.frontier, 2));
    REQUIRE(exploration.dominated.size() == 1);
    CHECK(exploration.dominated[0].dominated_id == 3);
    // Both of the others beat it; every dominator is listed, not just one.
    CHECK(exploration.dominated[0].dominated_by == std::vector<size_t> { 1, 2 });
    // A dominated plan is still in the list of everything offered.
    CHECK(exploration.all.size() == 3);
    CHECK(contains(exploration.all, 3));
}

TEST_CASE("28: an unorderable pair keeps the frontier from being claimed complete",
          "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    PlanCandidate partial = full_plan(3, PlanObjective::LowestRisk, 700., 5., 80., 300., 100.,
                                      0.70, 0.80, 0.90, 5.0);
    partial.metrics.calibration_margin.reset();
    plans.push_back(partial);

    PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});

    // Plan 3 could not be compared to either of the others.
    CHECK(exploration.unprovable_pairs.size() == 2);
    CHECK(exploration.frontier.size() == 3);
    CHECK_FALSE(exploration.recommended.has_value());
    CHECK_THAT(exploration.no_recommendation_reason,
               Catch::Matchers::ContainsSubstring("could not be compared on every metric"));
    bool warned = false;
    for (const std::string &note : exploration.notes)
        if (note.find("not proven to be the whole frontier") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("28: a blocked or ungenerated plan is excluded by name, not dropped",
          "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();

    PlanCandidate blocked = full_plan(3, PlanObjective::LeastWaste, 400., 1., 30., 90., 50., 0.90,
                                      0.99, 0.99, 4.0);
    blocked.blocking.push_back("the descent corridor could not be proven empty");
    blocked.blocking.push_back("tool 2 has no calibration");
    plans.push_back(blocked);

    PlanCandidate never_made;
    never_made.id                 = 4;
    never_made.objective          = PlanObjective::LowestRisk;
    never_made.generated          = false;
    never_made.ungenerated_reason = "no arrangement kept every toolhead clear";
    plans.push_back(never_made);

    PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});

    CHECK(exploration.all.size() == 4);
    CHECK(exploration.feasible.size() == 2);
    REQUIRE(exploration.excluded.count(3) == 1);
    CHECK_THAT(exploration.excluded.at(3),
               Catch::Matchers::ContainsSubstring("descent corridor"));
    CHECK_THAT(exploration.excluded.at(3), Catch::Matchers::ContainsSubstring("1 more"));
    REQUIRE(exploration.excluded.count(4) == 1);
    CHECK_THAT(exploration.excluded.at(4),
               Catch::Matchers::ContainsSubstring("no arrangement kept every toolhead clear"));

    // The fastest plan on paper was the blocked one; it wins nothing.
    const ObjectiveOutcome *fastest = outcome_for(exploration, PlanObjective::Fastest);
    REQUIRE(fastest != nullptr);
    CHECK(fastest->best == std::vector<size_t> { 1 });
}

TEST_CASE("28: every objective is reported, winner or not", "[plan-explorer]")
{
    PlanExploration exploration = explore_plans(tradeoff_pair(), ExplorerPolicy {});
    REQUIRE(exploration.objectives.size() == 7);

    CHECK(outcome_for(exploration, PlanObjective::Fastest)->best == std::vector<size_t> { 1 });
    CHECK(outcome_for(exploration, PlanObjective::FewestChanges)->best ==
          std::vector<size_t> { 1 });
    CHECK(outcome_for(exploration, PlanObjective::Finest)->best == std::vector<size_t> { 2 });
    // Least waste is judged on purge and support together.
    CHECK(outcome_for(exploration, PlanObjective::LeastWaste)->best == std::vector<size_t> { 1 });

    SECTION("a tie on the deciding metric is reported, not broken")
    {
        const ObjectiveOutcome *risk = outcome_for(exploration, PlanObjective::LowestRisk);
        REQUIRE(risk != nullptr);
        // Both plans have a 2.0 mm collision margin.
        CHECK(risk->tied);
        CHECK(risk->best == std::vector<size_t> { 1, 2 });
    }

    SECTION("balanced and the strongest heuristic are not decided by one number")
    {
        for (PlanObjective objective :
             { PlanObjective::Balanced, PlanObjective::StrongestHeuristic }) {
            const ObjectiveOutcome *outcome = outcome_for(exploration, objective);
            REQUIRE(outcome != nullptr);
            CHECK(outcome->best.empty());
            CHECK_THAT(outcome->unavailable_reason,
                       Catch::Matchers::ContainsSubstring("no feasible plan was generated"));
        }
    }
}

TEST_CASE("28: an objective whose metric nobody measured has no winner", "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    for (PlanCandidate &plan : plans)
        plan.metrics.detail.reset();

    PlanExploration         exploration = explore_plans(plans, ExplorerPolicy {});
    const ObjectiveOutcome *finest      = outcome_for(exploration, PlanObjective::Finest);
    REQUIRE(finest != nullptr);
    CHECK_FALSE(finest->available());
    CHECK_THAT(finest->unavailable_reason,
               Catch::Matchers::ContainsSubstring("no feasible plan measured"));
}

TEST_CASE("28: half a waste accounting is not a waste total", "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    plans[0].metrics.support_waste_mm3.reset();

    PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});
    const ObjectiveOutcome *least = outcome_for(exploration, PlanObjective::LeastWaste);
    REQUIRE(least != nullptr);
    // Plan 1 purges far less, but its support waste is unknown, so it cannot
    // be shown to waste less overall. Plan 2 wins by being the only one that
    // measured both, and the omission is stated.
    CHECK(least->best == std::vector<size_t> { 2 });
    CHECK_THAT(least->unavailable_reason,
               Catch::Matchers::ContainsSubstring("did not measure it"));
}

TEST_CASE("28: every frontier plan carries what it gives up", "[plan-explorer]")
{
    PlanExploration exploration = explore_plans(tradeoff_pair(), ExplorerPolicy {});

    REQUIRE(exploration.compromises.count(1) == 1);
    REQUIRE(exploration.compromises.count(2) == 1);

    const std::vector<Compromise> &fast = exploration.compromises.at(1);
    const std::vector<Compromise> &fine = exploration.compromises.at(2);

    SECTION("the fast plan admits it is coarser")
    {
        const Compromise *detail = compromise_for(fast, "detail");
        REQUIRE(detail != nullptr);
        CHECK(detail->best == 0.95);
        CHECK(detail->this_plan == 0.50);
        CHECK(detail->amount == Catch::Approx(0.45));
        CHECK(detail->achieved_by == std::vector<size_t> { 2 });
        REQUIRE(detail->relative.has_value());
        // Nothing it is already best at appears in its list.
        CHECK(compromise_for(fast, "time") == nullptr);
    }

    SECTION("the fine plan admits it is slower and wastes more")
    {
        REQUIRE(compromise_for(fine, "time") != nullptr);
        CHECK(compromise_for(fine, "time")->amount == 300.);
        REQUIRE(compromise_for(fine, "purge waste") != nullptr);
        CHECK(compromise_for(fine, "purge waste")->amount == 1440.);
        REQUIRE(compromise_for(fine, "toolchanges") != nullptr);
        CHECK(compromise_for(fine, "detail") == nullptr);
    }

    SECTION("a metric where they tie is a compromise for neither")
    {
        CHECK(compromise_for(fast, "collision margin") == nullptr);
        CHECK(compromise_for(fine, "collision margin") == nullptr);
        CHECK(compromise_for(fast, "support waste") == nullptr);
    }
}

TEST_CASE("28: incomplete filament accounting is reported and never presented as a total",
          "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    plans[0].metrics.filament_complete = false;

    SECTION("by default the plan stays, with the gap stated")
    {
        PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});
        CHECK(exploration.feasible.size() == 2);
        bool noted = false;
        for (const std::string &note : exploration.notes)
            if (note.find("could not attribute all filament") != std::string::npos)
                noted = true;
        CHECK(noted);
    }

    SECTION("a policy that requires a complete accounting excludes it and says why")
    {
        ExplorerPolicy strict;
        strict.allow_incomplete_filament = false;
        PlanExploration exploration      = explore_plans(plans, strict);
        CHECK(exploration.feasible == std::vector<size_t> { 2 });
        REQUIRE(exploration.excluded.count(1) == 1);
        CHECK_THAT(exploration.excluded.at(1),
                   Catch::Matchers::ContainsSubstring("material figures are partial"));
    }
}

TEST_CASE("28: one plan that beats every other is recommended; anything less is not",
          "[plan-explorer]")
{
    SECTION("a single dominating plan is recommended")
    {
        std::vector<PlanCandidate> plans = tradeoff_pair();
        plans.push_back(full_plan(3, PlanObjective::StrongestHeuristic, 500., 2., 50., 200., 90.,
                                  0.90, 0.99, 0.95, 3.0));
        PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});
        CHECK(exploration.frontier == std::vector<size_t> { 3 });
        REQUIRE(exploration.recommended.has_value());
        CHECK(exploration.recommended.value() == 3);
    }

    SECTION("one feasible plan is not a choice, and says so")
    {
        std::vector<PlanCandidate> plans = { tradeoff_pair()[0] };
        PlanExploration exploration      = explore_plans(plans, ExplorerPolicy {});
        REQUIRE(exploration.recommended.has_value());
        CHECK(exploration.recommended.value() == 1);
        bool noted = false;
        for (const std::string &note : exploration.notes)
            if (note.find("no choice to make") != std::string::npos)
                noted = true;
        CHECK(noted);
    }

    SECTION("no feasible plan means no recommendation and no silent empty result")
    {
        std::vector<PlanCandidate> plans = tradeoff_pair();
        for (PlanCandidate &plan : plans)
            plan.blocking.push_back("unsafe descent");
        PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});
        CHECK(exploration.feasible.empty());
        CHECK(exploration.frontier.empty());
        CHECK_FALSE(exploration.recommended.has_value());
        CHECK_THAT(exploration.no_recommendation_reason,
                   Catch::Matchers::ContainsSubstring("nothing to choose between"));
        // Every objective is still listed as having produced nothing.
        CHECK(exploration.objectives.size() == 7);
        for (const ObjectiveOutcome &outcome : exploration.objectives)
            CHECK(outcome.unavailable_reason == "no feasible plan");
    }
}

TEST_CASE("28: selecting a plan carries its compromises and assumptions with it",
          "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    plans[1].assumptions.push_back(
        { "tool 1 flow was fitted at 0.40 mm", "no measurement at 0.25 mm exists",
          AssumptionBasis::Estimated, true });
    plans[1].assumptions.push_back(
        { "ambient is 22 C", "the default", AssumptionBasis::Assumed, false });
    plans[1].warnings.push_back("the finest plan has not been printed");

    PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});

    SECTION("a frontier plan is accepted, with what it gives up")
    {
        SelectionResult result = select_plan(exploration, plans, 2, false);
        REQUIRE(result.ok());
        CHECK(result.selected.value() == 2);
        CHECK_FALSE(result.compromises.empty());
        // Only the load-bearing assumptions travel; the rest stay with the plan.
        REQUIRE(result.load_bearing.size() == 1);
        CHECK(result.load_bearing[0].basis == AssumptionBasis::Estimated);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("warning(s) stand"));
    }

    SECTION("an unknown plan is refused")
    {
        SelectionResult result = select_plan(exploration, plans, 99, false);
        CHECK(result.verdict == SelectionVerdict::UnknownPlan);
        CHECK_FALSE(result.selected.has_value());
    }
}

TEST_CASE("28: a blocked plan cannot be selected, and a dominated one takes a decision",
          "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    plans.push_back(full_plan(3, PlanObjective::Balanced, 1200., 25., 300., 2000., 200., 0.40,
                              0.30, 0.50, 1.0));
    PlanCandidate blocked = full_plan(4, PlanObjective::LeastWaste, 400., 1., 30., 90., 50., 0.90,
                                      0.99, 0.99, 4.0);
    blocked.blocking.push_back("tool 2 has no calibration");
    plans.push_back(blocked);

    PlanExploration exploration = explore_plans(plans, ExplorerPolicy {});

    SECTION("blocked stays blocked no matter how good its numbers are")
    {
        SelectionResult result = select_plan(exploration, plans, 4, true);
        CHECK(result.verdict == SelectionVerdict::Infeasible);
        CHECK_FALSE(result.selected.has_value());
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("no calibration"));
    }

    SECTION("a dominated plan needs an acknowledgement first, and names what beats it")
    {
        SelectionResult refused = select_plan(exploration, plans, 3, false);
        CHECK(refused.verdict == SelectionVerdict::NeedsAcknowledgement);
        CHECK_FALSE(refused.selected.has_value());
        CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("1, 2"));
        CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("has to be a decision"));

        SelectionResult accepted = select_plan(exploration, plans, 3, true);
        CHECK(accepted.ok());
        CHECK(accepted.selected.value() == 3);
    }
}

TEST_CASE("28: the same candidates explore the same way every time", "[plan-explorer]")
{
    std::vector<PlanCandidate> plans = tradeoff_pair();
    plans.push_back(full_plan(3, PlanObjective::LeastWaste, 700., 5., 80., 100., 60., 0.75, 0.70,
                              0.92, 2.5));

    PlanExploration first = explore_plans(plans, ExplorerPolicy {});

    // Offered in a different order: the report must not change.
    std::vector<PlanCandidate> shuffled = { plans[2], plans[0], plans[1] };
    PlanExploration            second   = explore_plans(shuffled, ExplorerPolicy {});

    CHECK(first.all == second.all);
    CHECK(first.feasible == second.feasible);
    CHECK(first.frontier == second.frontier);
    REQUIRE(first.objectives.size() == second.objectives.size());
    for (size_t i = 0; i < first.objectives.size(); ++i) {
        CHECK(first.objectives[i].objective == second.objectives[i].objective);
        CHECK(first.objectives[i].best == second.objectives[i].best);
    }
    for (size_t id : first.frontier) {
        REQUIRE(second.compromises.count(id) == 1);
        REQUIRE(first.compromises.at(id).size() == second.compromises.at(id).size());
        for (size_t i = 0; i < first.compromises.at(id).size(); ++i)
            CHECK(first.compromises.at(id)[i].metric == second.compromises.at(id)[i].metric);
    }
}
