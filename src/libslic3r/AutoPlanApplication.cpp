///|/ TigerSlicer Phase AUTO5: the independent apply-time preflight and the
///|/ plan reproducibility rule.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AutoPlanApplication.hpp"

#include <algorithm>

namespace Slic3r {

const char *preflight_verdict_name(PreflightVerdict verdict)
{
    switch (verdict) {
    case PreflightVerdict::Clear:    return "clear";
    case PreflightVerdict::Blocked:  return "blocked";
    case PreflightVerdict::Unproven: return "unproven";
    }
    return "unproven";
}

const char *reproduction_decision_name(ReproductionDecision decision)
{
    switch (decision) {
    case ReproductionDecision::Reproduce:        return "reproduce";
    case ReproductionDecision::InputsChanged:    return "inputs changed";
    case ReproductionDecision::MigrationRequired: return "migration required";
    }
    return "migration required";
}

PreflightResult independent_preflight(const AutoPlanCandidate &winner)
{
    PreflightResult result;

    for (const ConstraintResult &constraint : winner.constraints) {
        if (constraint.outcome == ConstraintOutcome::Violated)
            result.violations.push_back(constraint.constraint +
                                        (constraint.detail.empty() ? std::string()
                                                                   : ": " + constraint.detail));
        else if (constraint.outcome == ConstraintOutcome::Unknown)
            result.unknowns.push_back(constraint.constraint +
                                      (constraint.required_datum.empty()
                                           ? std::string()
                                           : " (needs " + constraint.required_datum + ")"));
    }

    if (! winner.respects_locks)
        result.broken_locks = winner.would_break_locks;

    // A violated constraint or a broken lock blocks; an unknown one leaves the
    // plan unproven. Violations and broken locks are the harder failure and win
    // the verdict.
    if (! result.violations.empty() || ! result.broken_locks.empty()) {
        result.verdict = PreflightVerdict::Blocked;
        result.message = "the plan cannot be applied: " +
                         std::to_string(result.violations.size()) + " violation(s) and " +
                         std::to_string(result.broken_locks.size()) + " broken lock(s) on a "
                         "fresh check";
    } else if (! result.unknowns.empty()) {
        result.verdict = PreflightVerdict::Unproven;
        result.message = "the plan is not proven safe to apply: " +
                         std::to_string(result.unknowns.size()) +
                         " constraint(s) could not be re-verified";
    } else {
        result.verdict = PreflightVerdict::Clear;
        result.message = "the plan re-verified clear and may be applied";
    }
    return result;
}

PreflightResult preflight_result(const AutoPlanResult                &result,
                                 const std::vector<AutoPlanCandidate> &candidates)
{
    PreflightResult preflight;

    if (! result.winner_id.has_value()) {
        preflight.verdict = PreflightVerdict::Blocked;
        preflight.message = "there is no winning plan to apply";
        return preflight;
    }

    const AutoPlanCandidate *winner = nullptr;
    for (const AutoPlanCandidate &candidate : candidates)
        if (candidate.id == *result.winner_id)
            winner = &candidate;

    if (winner == nullptr) {
        preflight.verdict = PreflightVerdict::Blocked;
        preflight.message = "the winning plan \"" + *result.winner_id +
                            "\" is not among the candidates supplied; it cannot be verified for "
                            "application";
        return preflight;
    }

    return independent_preflight(*winner);
}

ReproductionResult decide_reproduction(
    const PlanProvenance &stored, const std::string &current_planner_version,
    const std::string &current_objective_version,
    const std::map<std::string, std::string> &current_input_hashes)
{
    ReproductionResult result;

    // Inputs first: a changed input means the stored plan no longer describes
    // this project, whatever the versions are. Every stored input must still
    // match, and no new input the plan did not account for may have appeared
    // among the ones it recorded.
    for (const auto &entry : stored.input_hashes) {
        auto current = current_input_hashes.find(entry.first);
        if (current == current_input_hashes.end() || current->second != entry.second)
            result.changed_inputs.push_back(entry.first);
    }
    // A required input the stored plan recorded that is now absent is also a
    // change (handled above). An entirely new input the stored plan never saw
    // is a change too, when it is one the plan would have depended on; we treat
    // any difference in the recorded set as a change and leave brand-new keys to
    // the caller's hashing discipline.
    std::sort(result.changed_inputs.begin(), result.changed_inputs.end());

    if (! result.changed_inputs.empty()) {
        result.decision = ReproductionDecision::InputsChanged;
        result.message  = std::to_string(result.changed_inputs.size()) +
                         " input(s) changed; the stored plan is recomputed, not kept";
        return result;
    }

    // Versions next. A different planner or objective-weight model must not be
    // used to replay the stored slider position - that would silently
    // reinterpret the user's intent under new weights. Migration is requested
    // instead.
    if (stored.planner_version != current_planner_version ||
        stored.objective_version != current_objective_version) {
        result.decision = ReproductionDecision::MigrationRequired;
        result.message =
            "the planner or objective-weight model changed since this plan was made; migration "
            "is requested rather than reinterpreting the stored slider position under new "
            "weights";
        return result;
    }

    result.decision = ReproductionDecision::Reproduce;
    result.message  = "the planner, objectives and inputs all match; the stored plan is "
                     "reproduced exactly";
    return result;
}

} // namespace Slic3r
