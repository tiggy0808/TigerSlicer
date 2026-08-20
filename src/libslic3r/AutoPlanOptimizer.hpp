///|/ TigerSlicer Phase AUTO0/AUTO1: the shared deterministic Auto Prepare &
///|/ Slice optimizer, and the contracts it plans over.
///|/
///|/ Technology-agnostic optimization infrastructure: hard capability
///|/ constraints, deterministic candidate elimination and scoring, a Pareto
///|/ front, an explanation graph, and a deadline fallback. The FDM and MSLA
///|/ planners (AUTO2/AUTO3) supply their own candidates, constraints and
///|/ objectives - they share this machinery but never share print physics. No
///|/ runtime AI, model, embedding, cloud or network: a local deterministic
///|/ optimizer, testable in libslic3r.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_AutoPlanOptimizer_hpp_
#define slic3r_AutoPlanOptimizer_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// A soft objective the optimizer scores on. A slider moves the *weights* of
// these; it never touches a hard constraint. Direction is declared, not
// inferred from the sign of a score.
enum class ObjectiveDirection : uint8_t
{
    HigherIsBetter,
    LowerIsBetter,
};

const char *objective_direction_name(ObjectiveDirection direction);

struct AutoObjective
{
    std::string        key;
    ObjectiveDirection direction { ObjectiveDirection::HigherIsBetter };
    // The weight from the quality-to-speed slider, >= 0. Zero means the
    // objective is reported but does not influence the winner.
    double weight { 1. };
};

// The objective vector for one plan: the soft objectives and their weights.
// Nothing here is a hard constraint.
struct ObjectiveVector
{
    std::vector<AutoObjective> objectives;
};

// Why a candidate failed a hard constraint. A hard constraint is a verified
// machine / material / motion / thermal / flow / exposure / format limit or a
// user lock - never a soft preference.
enum class ConstraintOutcome : uint8_t
{
    Satisfied,
    Violated,      // a verified limit is exceeded; the candidate is eliminated
    // Data the constraint needs was not supplied. This does not make the
    // candidate low-scoring; it blocks it, and surfaces a requirement.
    Unknown,
};

const char *constraint_outcome_name(ConstraintOutcome outcome);

struct ConstraintResult
{
    std::string       constraint;   // what was checked
    ConstraintOutcome outcome { ConstraintOutcome::Unknown };
    std::string       detail;
    // When Unknown: the datum that must be supplied before this can be checked,
    // for the requirements panel.
    std::string required_datum;
};

// One candidate plan the technology planner generated. It arrives already
// evaluated: its hard-constraint results and its raw objective values. The
// optimizer does not compute physics; it eliminates, scores and ranks.
struct AutoPlanCandidate
{
    // Stable within one optimization. Ties break on it, so results are
    // reproducible.
    std::string id;
    std::string summary;

    std::vector<ConstraintResult> constraints;
    // Raw objective values by key, matching the ObjectiveVector.
    std::map<std::string, double> objectives;

    // True when this candidate honours every active user lock. A candidate
    // that would change a locked choice is not eligible, whatever it scores.
    bool respects_locks { true };
    // The locked choices this candidate would have changed, for the
    // explanation of why a better-scoring plan was not taken.
    std::vector<std::string> would_break_locks;

    bool eliminated() const;
    bool blocked_unknown() const;
};

// Why the optimizer settled where it did, kept as a graph the UI can show:
// the winner, the objectives that decided it, and the notable candidates it
// rejected with the reason.
struct RejectedCandidate
{
    std::string id;
    std::string reason;   // "violated: max layer height", "blocked: no flow curve", "locked: ..."
};

struct PlanExplanation
{
    std::string winner_id;
    // The weighted objective contributions that decided the winner, by key.
    std::map<std::string, double> winning_contributions;
    // Constraints that held the winner back from a better score, for honesty
    // about what the locks/limits cost.
    std::vector<std::string> limiting_constraints;
    std::vector<RejectedCandidate> rejected;
    std::vector<std::string>       notes;
};

// The datum a blocked candidate needs. Aggregated across candidates so the
// requirements panel is one short list, not one per candidate.
struct RequirementGap
{
    std::string datum;
    std::string why;
};

enum class AutoPlanVerdict : uint8_t
{
    // A valid winner was chosen.
    Planned,
    // No candidate satisfied every hard constraint. Auto slicing is blocked.
    NoValidCandidate,
    // Every remaining candidate is blocked on missing required data. The
    // requirements panel lists what to supply.
    BlockedOnRequirements,
    // Nothing was offered to plan.
    NoCandidates,
};

const char *auto_plan_verdict_name(AutoPlanVerdict verdict);

struct AutoPlanResult
{
    AutoPlanVerdict verdict { AutoPlanVerdict::NoCandidates };

    std::optional<std::string> winner_id;
    // The Pareto-non-dominated valid candidates, sorted by id.
    std::vector<std::string> pareto_front;

    PlanExplanation             explanation;
    std::vector<RequirementGap> requirements;

    // True when the optimizer stopped at its deadline before evaluating every
    // candidate and fell back to the best *fully evaluated* valid candidate. A
    // partially validated plan is never returned.
    bool deadline_fallback { false };
    // How many candidates were considered before the deadline.
    size_t evaluated { 0 };

    std::string summary;

    bool ok() const { return verdict == AutoPlanVerdict::Planned; }
};

struct AutoPlanBudget
{
    // The most candidates the optimizer will evaluate. 0 means no limit.
    size_t max_candidates { 0 };
};

// The request the optimizer plans over. The candidates and objective vector
// come from the technology planner; the budget bounds the work.
struct AutoPlanRequest
{
    std::vector<AutoPlanCandidate> candidates;
    ObjectiveVector                objectives;
    AutoPlanBudget                 budget;
    // A seed carried through for reproducibility; the optimizer itself is
    // deterministic and does not consume randomness, but the seed is recorded
    // so the same request is recognisably the same run.
    uint64_t seed { 0 };
};

// Score, eliminate and rank candidates into a plan.
//
// Invalid candidates - any hard constraint Violated, or a user lock broken -
// are eliminated before scoring, so the winner can never be an unsafe plan
// that merely scored well. Candidates blocked on Unknown required data are set
// aside and their gaps aggregated; if no valid candidate remains but some are
// only blocked on data, the verdict is BlockedOnRequirements rather than a
// silent failure. The winner is the highest weighted score among valid
// candidates, ties broken by id for reproducibility. On reaching the budget,
// the optimizer returns the best *fully evaluated* valid candidate and says it
// fell back - never a partially validated one.
AutoPlanResult optimize_plan(const AutoPlanRequest &request);

// The weighted score of one candidate against an objective vector, exposed so
// a caller can rank or display candidates consistently with the optimizer.
// Only meaningful for a candidate that is not eliminated.
double score_candidate(const AutoPlanCandidate &candidate, const ObjectiveVector &objectives);

// Whether candidate A Pareto-dominates candidate B across the objective vector:
// at least as good on every objective and strictly better on one. Used to
// retain the front. Only valid candidates are compared.
bool dominates(const AutoPlanCandidate &a, const AutoPlanCandidate &b,
               const ObjectiveVector &objectives);

} // namespace Slic3r

#endif // slic3r_AutoPlanOptimizer_hpp_
