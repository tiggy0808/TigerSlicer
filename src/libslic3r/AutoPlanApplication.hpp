///|/ TigerSlicer Phase AUTO5: the independent apply-time preflight and the
///|/ plan reproducibility rule.
///|/
///|/ Two safety layers over the AUTO1 optimizer's choice. The preflight
///|/ re-verifies the winning candidate independently before it may be applied -
///|/ the optimizer choosing a plan is not the same as the plan being safe to
///|/ apply, so apply is gated on a fresh check. The reproducibility rule
///|/ decides whether an older project's stored plan is replayed as-is or must
///|/ be migrated, and it never silently reinterprets a stored slider position
///|/ under new objective weights. Testable libslic3r logic.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_AutoPlanApplication_hpp_
#define slic3r_AutoPlanApplication_hpp_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "AutoPlanOptimizer.hpp"

namespace Slic3r {

enum class PreflightVerdict : uint8_t
{
    // Every hard constraint re-verified as satisfied and every lock respected.
    Clear,
    // A hard constraint is violated: the plan must not be applied.
    Blocked,
    // A hard constraint could not be re-verified (unknown data): the plan is
    // not proven safe to apply. Distinct from Blocked.
    Unproven,
};

const char *preflight_verdict_name(PreflightVerdict verdict);

struct PreflightResult
{
    PreflightVerdict verdict { PreflightVerdict::Unproven };
    std::vector<std::string> violations;   // constraints found violated now
    std::vector<std::string> unknowns;     // constraints not re-verifiable now
    std::vector<std::string> broken_locks; // locks the candidate would break
    std::string message;

    bool may_apply() const { return verdict == PreflightVerdict::Clear; }
};

// Independently re-verify the winning candidate before it is applied.
//
// This does not trust the optimizer's earlier verdict: it re-reads the
// candidate's hard-constraint results and lock status from scratch and passes
// only when every one is Satisfied and no lock is broken. A candidate the
// optimizer somehow returned as a winner but that carries a violation - a bug,
// or a candidate mutated after optimization - is caught here and refused.
PreflightResult independent_preflight(const AutoPlanCandidate &winner);

// Preflight the winner named by an AutoPlanResult against the candidate set it
// was chosen from. Refuses when the result has no winner, or the named winner
// is not in the set (the two came from different runs), rather than applying a
// plan that cannot be tied to its candidate.
PreflightResult preflight_result(const AutoPlanResult                &result,
                                 const std::vector<AutoPlanCandidate> &candidates);

// --- reproducibility (AUTO5.6 / AUTO5.7) -----------------------------------

// What a project stores so its plan can be reproduced or migrated. Hashes and
// versions are strings supplied by the caller; nothing here reads a clock.
struct PlanProvenance
{
    std::string planner_version;
    std::string objective_version;   // the version of the objective-weight model
    double      slider_position { 0. };
    uint64_t    seed { 0 };

    std::string selected_plan_id;
    std::string explanation_summary;

    // Input hashes that pin the plan to its inputs (geometry, profiles, etc.).
    std::map<std::string, std::string> input_hashes;
    // The locks in force when the plan was made.
    std::vector<std::string> locks;
};

enum class ReproductionDecision : uint8_t
{
    // Same planner and objective versions and matching inputs: replay the
    // stored plan exactly.
    Reproduce,
    // The inputs changed: the stored plan no longer describes this project and
    // must be recomputed (not silently kept).
    InputsChanged,
    // The planner or objective-weight model changed: the plan must be migrated
    // with the user's involvement. The stored slider position is NEVER
    // silently reinterpreted under the new weights.
    MigrationRequired,
};

const char *reproduction_decision_name(ReproductionDecision decision);

struct ReproductionResult
{
    ReproductionDecision decision { ReproductionDecision::MigrationRequired };
    std::string          message;
    // The input hashes that differ, when that is the reason.
    std::vector<std::string> changed_inputs;

    bool reproduces() const { return decision == ReproductionDecision::Reproduce; }
};

// Decide whether a stored plan can be reproduced against the current
// environment. Inputs first: if any input hash differs, the plan is recomputed
// (InputsChanged). Otherwise, if the planner or objective version differs, the
// plan must be migrated - never replayed under reinterpreted weights. Only when
// versions and inputs all match is it reproduced exactly.
ReproductionResult decide_reproduction(const PlanProvenance                     &stored,
                                       const std::string                        &current_planner_version,
                                       const std::string                        &current_objective_version,
                                       const std::map<std::string, std::string> &current_input_hashes);

} // namespace Slic3r

#endif // slic3r_AutoPlanApplication_hpp_
