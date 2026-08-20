///|/ TigerSlicer Phase 28: quality/time/waste/risk Pareto explorer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PlanExplorer.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r {

const char *plan_objective_name(PlanObjective objective)
{
    switch (objective) {
    case PlanObjective::Fastest:            return "fastest";
    case PlanObjective::Balanced:           return "balanced";
    case PlanObjective::Finest:             return "finest";
    case PlanObjective::FewestChanges:      return "fewest changes";
    case PlanObjective::LeastWaste:         return "least waste";
    case PlanObjective::LowestRisk:         return "lowest risk";
    case PlanObjective::StrongestHeuristic: return "strongest heuristic";
    }
    return "balanced";
}

const char *assumption_basis_name(AssumptionBasis basis)
{
    switch (basis) {
    case AssumptionBasis::Measured:   return "measured";
    case AssumptionBasis::Configured: return "configured";
    case AssumptionBasis::Estimated:  return "estimated";
    case AssumptionBasis::Assumed:    return "assumed";
    case AssumptionBasis::Unknown:    return "unknown";
    }
    return "unknown";
}

const char *dominance_verdict_name(DominanceVerdict verdict)
{
    switch (verdict) {
    case DominanceVerdict::Dominates:       return "dominates";
    case DominanceVerdict::DoesNotDominate: return "does not dominate";
    case DominanceVerdict::Unprovable:      return "unprovable";
    }
    return "unprovable";
}

const char *selection_verdict_name(SelectionVerdict verdict)
{
    switch (verdict) {
    case SelectionVerdict::Accepted:             return "accepted";
    case SelectionVerdict::Infeasible:           return "infeasible";
    case SelectionVerdict::UnknownPlan:          return "unknown plan";
    case SelectionVerdict::NeedsAcknowledgement: return "needs acknowledgement";
    }
    return "unknown plan";
}

namespace {

struct MetricAccess
{
    const char *name;
    std::optional<double> PlanMetrics::*field;
    PlanMetricDirection direction;
    double ExplorerPolicy::*epsilon;
};

// The one fixed order. Everything that walks metrics walks this.
const MetricAccess ACCESS[] = {
    { "time", &PlanMetrics::time_s, PlanMetricDirection::LowerIsBetter,
      &ExplorerPolicy::time_s_epsilon },
    { "toolchanges", &PlanMetrics::toolchanges, PlanMetricDirection::LowerIsBetter,
      &ExplorerPolicy::toolchange_epsilon },
    { "heating", &PlanMetrics::heating_s, PlanMetricDirection::LowerIsBetter,
      &ExplorerPolicy::heating_s_epsilon },
    { "purge waste", &PlanMetrics::purge_waste_mm3, PlanMetricDirection::LowerIsBetter,
      &ExplorerPolicy::waste_mm3_epsilon },
    { "support waste", &PlanMetrics::support_waste_mm3, PlanMetricDirection::LowerIsBetter,
      &ExplorerPolicy::waste_mm3_epsilon },
    { "flow utilization", &PlanMetrics::flow_utilization, PlanMetricDirection::HigherIsBetter,
      &ExplorerPolicy::flow_epsilon },
    { "detail", &PlanMetrics::detail, PlanMetricDirection::HigherIsBetter,
      &ExplorerPolicy::detail_epsilon },
    { "calibration margin", &PlanMetrics::calibration_margin,
      PlanMetricDirection::HigherIsBetter, &ExplorerPolicy::margin_epsilon },
    { "collision margin", &PlanMetrics::collision_margin_mm, PlanMetricDirection::HigherIsBetter,
      &ExplorerPolicy::collision_mm_epsilon },
};

constexpr size_t METRIC_COUNT = sizeof(ACCESS) / sizeof(ACCESS[0]);

// The metric each objective is actually judged on. Balanced and the strongest
// heuristic are deliberately absent: there is no single number that decides
// them, and inventing one would be the hidden compromise this phase exists to
// prevent.
const std::optional<size_t> objective_metric(PlanObjective objective)
{
    switch (objective) {
    case PlanObjective::Fastest:       return size_t(0); // time
    case PlanObjective::FewestChanges: return size_t(1); // toolchanges
    case PlanObjective::Finest:        return size_t(6); // detail
    case PlanObjective::LowestRisk:    return size_t(8); // collision margin
    case PlanObjective::LeastWaste:    return std::nullopt; // purge + support, handled apart
    case PlanObjective::Balanced:
    case PlanObjective::StrongestHeuristic: return std::nullopt;
    }
    return std::nullopt;
}

// Total waste, only when both halves were measured. Half an accounting is not
// a total.
std::optional<double> total_waste(const PlanMetrics &m)
{
    if (! m.purge_waste_mm3.has_value() || ! m.support_waste_mm3.has_value())
        return std::nullopt;
    return *m.purge_waste_mm3 + *m.support_waste_mm3;
}

const PlanCandidate *by_id(const std::vector<PlanCandidate> &candidates, size_t id)
{
    for (const PlanCandidate &c : candidates)
        if (c.id == id)
            return &c;
    return nullptr;
}

// Better by more than epsilon, in the metric's own direction.
bool strictly_better(double a, double b, PlanMetricDirection direction, double epsilon)
{
    return direction == PlanMetricDirection::LowerIsBetter ? a < b - epsilon : a > b + epsilon;
}

} // namespace

const std::vector<PlanMetricSpec> &plan_metric_specs()
{
    static const std::vector<PlanMetricSpec> specs = [] {
        std::vector<PlanMetricSpec> out;
        for (const MetricAccess &a : ACCESS)
            out.push_back({ a.name, a.direction });
        return out;
    }();
    return specs;
}

DominanceResult dominance(const PlanCandidate &a, const PlanCandidate &b)
{
    // Dominance is decided with the default epsilons; explore_plans uses the
    // policy's. Kept as one implementation so the two never drift.
    static const ExplorerPolicy DEFAULT_POLICY {};
    const ExplorerPolicy       &policy = DEFAULT_POLICY;

    DominanceResult result;

    for (size_t i = 0; i < METRIC_COUNT; ++i) {
        const MetricAccess &m  = ACCESS[i];
        const auto         &va = a.metrics.*(m.field);
        const auto         &vb = b.metrics.*(m.field);

        if (! va.has_value() || ! vb.has_value()) {
            result.unmeasured.push_back(
                std::string(m.name) + ": " +
                (! va.has_value() && ! vb.has_value()
                     ? "neither plan measured it"
                     : (va.has_value() ? "the second plan did not measure it"
                                       : "the first plan did not measure it")));
            continue;
        }

        const double epsilon = std::abs(policy.*(m.epsilon));
        if (strictly_better(*va, *vb, m.direction, epsilon))
            result.better.push_back(m.name);
        else if (strictly_better(*vb, *va, m.direction, epsilon))
            result.worse.push_back(m.name);
    }

    if (! result.unmeasured.empty()) {
        // A plan cannot be shown to be at least as good on a metric nobody
        // measured, so no dominance claim is available. Saying "does not
        // dominate" here would quietly promote an unmeasured plan onto the
        // frontier as if it had been compared.
        result.verdict = DominanceVerdict::Unprovable;
        return result;
    }

    result.verdict = (result.worse.empty() && ! result.better.empty())
                         ? DominanceVerdict::Dominates
                         : DominanceVerdict::DoesNotDominate;
    return result;
}

namespace {

// The same comparison as dominance(), against the caller's policy.
DominanceVerdict dominance_with(const PlanCandidate &a, const PlanCandidate &b,
                                const ExplorerPolicy &policy, bool &unprovable)
{
    bool any_better = false;
    bool any_worse  = false;
    unprovable      = false;

    for (size_t i = 0; i < METRIC_COUNT; ++i) {
        const MetricAccess &m  = ACCESS[i];
        const auto         &va = a.metrics.*(m.field);
        const auto         &vb = b.metrics.*(m.field);
        if (! va.has_value() || ! vb.has_value()) {
            unprovable = true;
            return DominanceVerdict::Unprovable;
        }
        const double epsilon = std::abs(policy.*(m.epsilon));
        if (strictly_better(*va, *vb, m.direction, epsilon))
            any_better = true;
        else if (strictly_better(*vb, *va, m.direction, epsilon))
            any_worse = true;
    }

    return (! any_worse && any_better) ? DominanceVerdict::Dominates
                                       : DominanceVerdict::DoesNotDominate;
}

} // namespace

PlanExploration explore_plans(const std::vector<PlanCandidate> &candidates,
                              const ExplorerPolicy             &policy)
{
    PlanExploration exploration;

    std::vector<const PlanCandidate *> sorted;
    for (const PlanCandidate &c : candidates)
        sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(),
              [](const PlanCandidate *a, const PlanCandidate *b) { return a->id < b->id; });

    std::vector<const PlanCandidate *> feasible;
    for (const PlanCandidate *c : sorted) {
        exploration.all.push_back(c->id);

        if (! c->generated) {
            exploration.excluded[c->id] =
                std::string("no plan was generated for the ") +
                plan_objective_name(c->objective) + " objective" +
                (c->ungenerated_reason.empty() ? std::string() : ": " + c->ungenerated_reason);
            continue;
        }
        if (! c->blocking.empty()) {
            std::string reason = "blocked: " + c->blocking.front();
            if (c->blocking.size() > 1)
                reason += " (and " + std::to_string(c->blocking.size() - 1) + " more)";
            exploration.excluded[c->id] = reason;
            continue;
        }
        if (! c->metrics.filament_complete) {
            const std::string note =
                "plan " + std::to_string(c->id) +
                " could not attribute all filament to a tool, so its material figures are "
                "partial";
            if (policy.allow_incomplete_filament)
                exploration.notes.push_back(note);
            else {
                exploration.excluded[c->id] = note;
                continue;
            }
        }

        exploration.feasible.push_back(c->id);
        feasible.push_back(c);
    }

    if (feasible.empty()) {
        exploration.no_recommendation_reason =
            "no plan was both generated and free of blocking findings, so there is nothing to "
            "choose between";
        // The objectives are still listed, so the caller can see which ones
        // produced nothing rather than finding them missing.
        for (PlanObjective objective :
             { PlanObjective::Fastest, PlanObjective::Balanced, PlanObjective::Finest,
               PlanObjective::FewestChanges, PlanObjective::LeastWaste, PlanObjective::LowestRisk,
               PlanObjective::StrongestHeuristic }) {
            ObjectiveOutcome outcome;
            outcome.objective          = objective;
            outcome.unavailable_reason = "no feasible plan";
            exploration.objectives.push_back(outcome);
        }
        return exploration;
    }

    // --- frontier ---------------------------------------------------------
    std::set<std::pair<size_t, size_t>> unprovable;
    for (const PlanCandidate *a : feasible) {
        Domination domination;
        domination.dominated_id = a->id;

        for (const PlanCandidate *b : feasible) {
            if (a == b)
                continue;
            bool                   unprovable_here = false;
            const DominanceVerdict verdict = dominance_with(*b, *a, policy, unprovable_here);
            if (unprovable_here) {
                unprovable.insert({ std::min(a->id, b->id), std::max(a->id, b->id) });
                continue;
            }
            if (verdict == DominanceVerdict::Dominates)
                domination.dominated_by.push_back(b->id);
        }

        if (domination.dominated_by.empty()) {
            exploration.frontier.push_back(a->id);
        } else {
            std::sort(domination.dominated_by.begin(), domination.dominated_by.end());
            exploration.dominated.push_back(domination);
        }
    }
    exploration.unprovable_pairs.assign(unprovable.begin(), unprovable.end());
    if (! exploration.unprovable_pairs.empty())
        exploration.notes.push_back(
            std::to_string(exploration.unprovable_pairs.size()) +
            " pair(s) of plans could not be ordered because a metric was missing on one side; "
            "the frontier is therefore not proven to be the whole frontier");

    // --- objective winners ------------------------------------------------
    for (PlanObjective objective :
         { PlanObjective::Fastest, PlanObjective::Balanced, PlanObjective::Finest,
           PlanObjective::FewestChanges, PlanObjective::LeastWaste, PlanObjective::LowestRisk,
           PlanObjective::StrongestHeuristic }) {
        ObjectiveOutcome outcome;
        outcome.objective = objective;

        if (objective == PlanObjective::Balanced ||
            objective == PlanObjective::StrongestHeuristic) {
            // These are not a single number. The candidate generated for them
            // is offered as itself; nothing here declares it the winner of a
            // contest that was never defined.
            bool present = false;
            for (const PlanCandidate *c : feasible)
                if (c->objective == objective) {
                    outcome.best.push_back(c->id);
                    present = true;
                }
            std::sort(outcome.best.begin(), outcome.best.end());
            outcome.tied = outcome.best.size() > 1;
            if (! present)
                outcome.unavailable_reason =
                    std::string("no feasible plan was generated for the ") +
                    plan_objective_name(objective) + " objective";
            else
                outcome.unavailable_reason =
                    std::string("the ") + plan_objective_name(objective) +
                    " objective is not decided by a single metric; this is the plan built for "
                    "it, not the proven best on any axis";
            exploration.objectives.push_back(outcome);
            continue;
        }

        // Least waste is judged on the two waste figures together.
        const std::optional<size_t> index = objective_metric(objective);

        // Direction and epsilon are fixed for this objective; only the value
        // varies per candidate.
        PlanMetricDirection direction = PlanMetricDirection::LowerIsBetter;
        double              epsilon   = std::abs(policy.waste_mm3_epsilon);
        if (index.has_value()) {
            direction = ACCESS[*index].direction;
            epsilon   = std::abs(policy.*(ACCESS[*index].epsilon));
        }
        auto value_of = [&](const PlanCandidate *c) -> std::optional<double> {
            return index.has_value() ? c->metrics.*(ACCESS[*index].field)
                                     : total_waste(c->metrics);
        };

        // Two passes so the reported tie set is consistent with the final
        // optimum rather than order-dependent. A single reset-on-strictly-better
        // pass drops a plan that ties the FINAL best but was seen before it (the
        // epsilon tie relation is non-transitive).
        std::optional<double> best_value;
        size_t                unmeasured = 0;
        // Pass 1: the true optimum, ignoring epsilon, so pass 2's band is
        // measured against the real best.
        for (const PlanCandidate *c : feasible) {
            const std::optional<double> value = value_of(c);
            if (! value.has_value()) {
                ++unmeasured;
                continue;
            }
            if (! best_value.has_value() || strictly_better(*value, *best_value, direction, 0.))
                best_value = value;
        }
        // Pass 2: every measured plan within epsilon (or equal) of the optimum.
        std::vector<size_t> best_ids;
        if (best_value.has_value())
            for (const PlanCandidate *c : feasible) {
                const std::optional<double> value = value_of(c);
                if (! value.has_value())
                    continue;
                if (! strictly_better(*best_value, *value, direction, epsilon))
                    best_ids.push_back(c->id);
            }

        std::sort(best_ids.begin(), best_ids.end());
        outcome.best = best_ids;
        outcome.tied = best_ids.size() > 1;
        if (best_ids.empty())
            outcome.unavailable_reason =
                std::string("no feasible plan measured what the ") +
                plan_objective_name(objective) + " objective is judged on";
        else if (unmeasured > 0)
            outcome.unavailable_reason =
                std::to_string(unmeasured) +
                " feasible plan(s) did not measure it and could not be considered";

        exploration.objectives.push_back(outcome);
    }

    // --- compromises ------------------------------------------------------
    // Compute for every feasible plan, not only frontier plans, so a caller who
    // deliberately selects a dominated plan is still shown the trade-offs it
    // makes (an empty list would otherwise hide them entirely).
    for (size_t id : exploration.feasible) {
        const PlanCandidate *plan = nullptr;
        for (const PlanCandidate *c : feasible)
            if (c->id == id)
                plan = c;
        if (plan == nullptr)
            continue;

        std::vector<Compromise> list;
        for (size_t i = 0; i < METRIC_COUNT; ++i) {
            const MetricAccess &m    = ACCESS[i];
            const auto         &mine = plan->metrics.*(m.field);
            if (! mine.has_value())
                continue;

            const double epsilon = std::abs(policy.*(m.epsilon));
            // Pass 1: the true optimum on this metric (epsilon 0), so the holder
            // set below is order-independent and includes every epsilon-tie.
            std::optional<double> best;
            for (const PlanCandidate *other : feasible) {
                const auto &value = other->metrics.*(m.field);
                if (! value.has_value())
                    continue;
                if (! best.has_value() || strictly_better(*value, *best, m.direction, 0.))
                    best = value;
            }
            if (! best.has_value())
                continue;
            if (! strictly_better(*best, *mine, m.direction, epsilon))
                continue; // this plan is among the best on this metric
            // Pass 2: every plan within epsilon of that optimum holds it.
            std::vector<size_t> holders;
            for (const PlanCandidate *other : feasible) {
                const auto &value = other->metrics.*(m.field);
                if (value.has_value() && ! strictly_better(*best, *value, m.direction, epsilon))
                    holders.push_back(other->id);
            }

            Compromise compromise;
            compromise.metric    = m.name;
            compromise.best      = *best;
            compromise.this_plan = *mine;
            compromise.amount    = std::abs(*mine - *best);
            if (std::abs(*best) > 1e-12)
                compromise.relative = compromise.amount / std::abs(*best);
            std::sort(holders.begin(), holders.end());
            compromise.achieved_by = holders;
            list.push_back(compromise);
        }
        // Present even when empty, so a caller reading the map never has to
        // decide whether a missing entry means "no compromises" or "not
        // computed".
        exploration.compromises[id] = list;
    }

    // --- recommendation ---------------------------------------------------
    if (exploration.frontier.size() == 1 && exploration.unprovable_pairs.empty() &&
        feasible.size() > 1) {
        exploration.recommended = exploration.frontier.front();
    } else if (feasible.size() == 1) {
        exploration.recommended = feasible.front()->id;
        exploration.notes.push_back(
            "only one plan was feasible, so there was no choice to make");
    } else if (! exploration.unprovable_pairs.empty()) {
        exploration.no_recommendation_reason =
            "some plans could not be compared on every metric, so no plan can be shown to be "
            "best";
    } else {
        exploration.no_recommendation_reason =
            "more than one plan is on the frontier; each is better than the others at something, "
            "and which trade-off to take is not a decision this can make";
    }

    return exploration;
}

SelectionResult select_plan(const PlanExploration            &exploration,
                            const std::vector<PlanCandidate> &candidates, size_t id,
                            bool acknowledged_dominated)
{
    SelectionResult result;

    const PlanCandidate *plan = by_id(candidates, id);
    if (plan == nullptr) {
        result.verdict = SelectionVerdict::UnknownPlan;
        result.message = "plan " + std::to_string(id) + " is not one of the plans offered";
        return result;
    }

    auto excluded = exploration.excluded.find(id);
    if (excluded != exploration.excluded.end()) {
        result.verdict = SelectionVerdict::Infeasible;
        result.message = "plan " + std::to_string(id) + " cannot be selected - " +
                         excluded->second;
        return result;
    }

    const bool on_frontier = std::find(exploration.frontier.begin(), exploration.frontier.end(),
                                       id) != exploration.frontier.end();
    if (! on_frontier && ! acknowledged_dominated) {
        std::vector<size_t> dominators;
        for (const Domination &d : exploration.dominated)
            if (d.dominated_id == id)
                dominators = d.dominated_by;

        result.verdict = SelectionVerdict::NeedsAcknowledgement;
        result.message = "plan " + std::to_string(id) + " is not as good as ";
        for (size_t i = 0; i < dominators.size(); ++i) {
            if (i > 0)
                result.message += ", ";
            result.message += std::to_string(dominators[i]);
        }
        result.message += " on every metric measured; selecting it anyway is allowed, but it "
                          "has to be a decision";
        return result;
    }

    result.verdict  = SelectionVerdict::Accepted;
    result.selected = id;
    auto compromises = exploration.compromises.find(id);
    if (compromises != exploration.compromises.end())
        result.compromises = compromises->second;
    for (const PlanAssumption &assumption : plan->assumptions)
        if (assumption.load_bearing)
            result.load_bearing.push_back(assumption);

    result.message = "plan " + std::to_string(id) + " selected";
    if (! result.compromises.empty())
        result.message += " with " + std::to_string(result.compromises.size()) +
                          " metric(s) it is not the best on";
    if (! plan->warnings.empty())
        result.message += "; " + std::to_string(plan->warnings.size()) + " warning(s) stand";

    return result;
}

} // namespace Slic3r
