///|/ TigerSlicer Phase AUTO0/AUTO1: the shared deterministic Auto Prepare &
///|/ Slice optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AutoPlanOptimizer.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r {

const char *objective_direction_name(ObjectiveDirection direction)
{
    switch (direction) {
    case ObjectiveDirection::HigherIsBetter: return "higher is better";
    case ObjectiveDirection::LowerIsBetter:  return "lower is better";
    }
    return "higher is better";
}

const char *constraint_outcome_name(ConstraintOutcome outcome)
{
    switch (outcome) {
    case ConstraintOutcome::Satisfied: return "satisfied";
    case ConstraintOutcome::Violated:  return "violated";
    case ConstraintOutcome::Unknown:   return "unknown";
    }
    return "unknown";
}

const char *auto_plan_verdict_name(AutoPlanVerdict verdict)
{
    switch (verdict) {
    case AutoPlanVerdict::Planned:               return "planned";
    case AutoPlanVerdict::NoValidCandidate:      return "no valid candidate";
    case AutoPlanVerdict::BlockedOnRequirements: return "blocked on requirements";
    case AutoPlanVerdict::NoCandidates:          return "no candidates";
    }
    return "no candidates";
}

bool AutoPlanCandidate::eliminated() const
{
    if (! respects_locks)
        return true;
    for (const ConstraintResult &constraint : constraints)
        if (constraint.outcome == ConstraintOutcome::Violated)
            return true;
    return false;
}

bool AutoPlanCandidate::blocked_unknown() const
{
    // Only "blocked" when it is not otherwise eliminated: a candidate that
    // violates a hard limit is eliminated regardless of what else is unknown.
    if (eliminated())
        return false;
    for (const ConstraintResult &constraint : constraints)
        if (constraint.outcome == ConstraintOutcome::Unknown)
            return true;
    return false;
}

namespace {

double objective_value(const AutoPlanCandidate &candidate, const std::string &key)
{
    auto it = candidate.objectives.find(key);
    return it == candidate.objectives.end() ? 0. : it->second;
}

// A per-objective contribution normalized so higher is always better, then
// weighted. Lower-is-better objectives are negated so the sum is maximized.
double contribution(const AutoPlanCandidate &candidate, const AutoObjective &objective)
{
    const double raw = objective_value(candidate, objective.key);
    const double oriented =
        objective.direction == ObjectiveDirection::HigherIsBetter ? raw : -raw;
    return oriented * std::abs(objective.weight);
}

} // namespace

double score_candidate(const AutoPlanCandidate &candidate, const ObjectiveVector &objectives)
{
    double total = 0.;
    for (const AutoObjective &objective : objectives.objectives)
        total += contribution(candidate, objective);
    return total;
}

bool dominates(const AutoPlanCandidate &a, const AutoPlanCandidate &b,
               const ObjectiveVector &objectives)
{
    bool strictly_better_somewhere = false;
    for (const AutoObjective &objective : objectives.objectives) {
        const double va = objective_value(a, objective.key);
        const double vb = objective_value(b, objective.key);
        const double da = objective.direction == ObjectiveDirection::HigherIsBetter ? va : -va;
        const double db = objective.direction == ObjectiveDirection::HigherIsBetter ? vb : -vb;
        if (da < db - 1e-12)
            return false; // worse on this objective: cannot dominate
        if (da > db + 1e-12)
            strictly_better_somewhere = true;
    }
    return strictly_better_somewhere;
}

AutoPlanResult optimize_plan(const AutoPlanRequest &request)
{
    AutoPlanResult result;

    if (request.candidates.empty()) {
        result.verdict = AutoPlanVerdict::NoCandidates;
        result.summary = "nothing was offered to plan";
        return result;
    }

    // Evaluate in a stable order (by id), so a deadline cut is reproducible and
    // the fallback is well-defined.
    std::vector<const AutoPlanCandidate *> ordered;
    for (const AutoPlanCandidate &candidate : request.candidates)
        ordered.push_back(&candidate);
    std::sort(ordered.begin(), ordered.end(),
              [](const AutoPlanCandidate *a, const AutoPlanCandidate *b) {
                  return a->id < b->id;
              });

    const size_t limit =
        request.budget.max_candidates == 0 ? ordered.size()
                                           : std::min(request.budget.max_candidates,
                                                      ordered.size());
    result.deadline_fallback = limit < ordered.size();
    result.evaluated         = limit;

    std::vector<const AutoPlanCandidate *> valid;
    std::vector<const AutoPlanCandidate *> blocked;
    for (size_t i = 0; i < limit; ++i) {
        const AutoPlanCandidate *candidate = ordered[i];
        if (candidate->eliminated()) {
            // Record why, for the explanation.
            std::string reason;
            if (! candidate->respects_locks) {
                reason = "locked: ";
                for (size_t j = 0; j < candidate->would_break_locks.size(); ++j) {
                    if (j > 0)
                        reason += ", ";
                    reason += candidate->would_break_locks[j];
                }
            } else {
                for (const ConstraintResult &c : candidate->constraints)
                    if (c.outcome == ConstraintOutcome::Violated) {
                        reason = "violated: " + c.constraint;
                        break;
                    }
            }
            result.explanation.rejected.push_back({ candidate->id, reason });
        } else if (candidate->blocked_unknown()) {
            blocked.push_back(candidate);
        } else {
            valid.push_back(candidate);
        }
    }

    if (valid.empty()) {
        if (! blocked.empty()) {
            // Aggregate the required data across all blocked candidates.
            std::set<std::string> seen;
            for (const AutoPlanCandidate *candidate : blocked)
                for (const ConstraintResult &c : candidate->constraints)
                    if (c.outcome == ConstraintOutcome::Unknown &&
                        ! c.required_datum.empty() && ! seen.count(c.required_datum)) {
                        seen.insert(c.required_datum);
                        result.requirements.push_back({ c.required_datum, c.detail });
                    }
            result.verdict = AutoPlanVerdict::BlockedOnRequirements;
            result.summary = std::to_string(result.requirements.size()) +
                             " required datum(s) must be supplied before a plan can be made";
        } else {
            result.verdict = AutoPlanVerdict::NoValidCandidate;
            result.summary = "no candidate satisfied every hard constraint; automatic slicing "
                             "is blocked";
        }
        // A capped search cannot assert a definitive global conclusion: valid
        // candidates may exist past the deadline cut. Say so, so a caller keying
        // on the verdict does not treat a partial search as exhaustive.
        if (result.deadline_fallback)
            result.summary += " (only " + std::to_string(result.evaluated) + " of " +
                              std::to_string(request.candidates.size()) +
                              " candidates were evaluated before the deadline; others were "
                              "not checked)";
        return result;
    }

    // Winner: highest weighted score among valid candidates, ties by id.
    const AutoPlanCandidate *winner = valid.front();
    double                   best   = score_candidate(*winner, request.objectives);
    for (const AutoPlanCandidate *candidate : valid) {
        const double s = score_candidate(*candidate, request.objectives);
        if (s > best + 1e-12 || (std::abs(s - best) <= 1e-12 && candidate->id < winner->id)) {
            best   = s;
            winner = candidate;
        }
    }

    // Pareto front over the valid set.
    for (const AutoPlanCandidate *candidate : valid) {
        bool dominated = false;
        for (const AutoPlanCandidate *other : valid)
            if (other != candidate && dominates(*other, *candidate, request.objectives)) {
                dominated = true;
                break;
            }
        if (! dominated)
            result.pareto_front.push_back(candidate->id);
    }
    std::sort(result.pareto_front.begin(), result.pareto_front.end());

    result.verdict   = AutoPlanVerdict::Planned;
    result.winner_id = winner->id;

    // Explanation: the winning contributions, and the constraints that held the
    // winner back (satisfied ones that were tight are not modelled here; a
    // limiting constraint is one the winner reports as a note-worthy limit).
    result.explanation.winner_id = winner->id;
    for (const AutoObjective &objective : request.objectives.objectives)
        result.explanation.winning_contributions[objective.key] =
            contribution(*winner, objective);
    for (const ConstraintResult &c : winner->constraints)
        if (c.outcome == ConstraintOutcome::Satisfied && ! c.detail.empty())
            result.explanation.limiting_constraints.push_back(c.constraint + ": " + c.detail);

    result.summary = "planned \"" + winner->id + "\"" +
                     (result.deadline_fallback
                          ? " (best of " + std::to_string(result.evaluated) +
                                " evaluated before the deadline)"
                          : "");
    return result;
}

} // namespace Slic3r
