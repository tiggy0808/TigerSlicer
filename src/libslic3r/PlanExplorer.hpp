///|/ TigerSlicer Phase 28: quality/time/waste/risk Pareto explorer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PlanExplorer_hpp_
#define slic3r_PlanExplorer_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// What a candidate plan was generated to be best at. A plan is labelled with
// the objective it was built for; whether it actually wins that objective is
// measured, not assumed.
enum class PlanObjective : uint8_t
{
    Fastest,
    Balanced,
    Finest,
    FewestChanges,
    LeastWaste,
    LowestRisk,
    // The best the heuristic can do without committing to one axis.
    StrongestHeuristic,
};

const char *plan_objective_name(PlanObjective objective);

// How far a number can be leant on. Nothing here is a default: a value with no
// stated basis is Unknown and is reported as such.
enum class AssumptionBasis : uint8_t
{
    Measured,
    Configured,
    Estimated,
    Assumed,
    Unknown,
};

const char *assumption_basis_name(AssumptionBasis basis);

// Something a plan rests on that is not itself part of the plan. Every plan
// carries its own; they are shown with it and never summarised away.
struct PlanAssumption
{
    std::string     what;
    std::string     why;
    AssumptionBasis basis { AssumptionBasis::Unknown };

    // True when the plan would be different if this turned out to be wrong.
    bool load_bearing { false };
};

// Which axis a metric improves along. Nothing infers this from the sign of a
// number.
enum class PlanMetricDirection : uint8_t
{
    LowerIsBetter,
    HigherIsBetter,
};

// The nine reported quantities, every one optional so that "never measured"
// stays structurally distinct from "measured as zero".
struct PlanMetrics
{
    std::optional<double> time_s;
    std::optional<double> toolchanges;
    std::optional<double> heating_s;
    std::optional<double> purge_waste_mm3;
    std::optional<double> support_waste_mm3;
    std::optional<double> flow_utilization;
    std::optional<double> detail;
    std::optional<double> calibration_margin;
    std::optional<double> collision_margin_mm;

    // Filament per tool. Reported separately from the totals because a plan
    // that moves material between tools has not saved any.
    std::map<int, double> filament_mm3_by_tool;
    // False when some tool's usage could not be attributed. The map is then
    // partial and is never presented as a complete accounting.
    bool filament_complete { false };
};

// The names and directions of the metrics, in the fixed order every report
// uses. Exposed so callers render the same list the comparison used.
struct PlanMetricSpec
{
    const char         *name;
    PlanMetricDirection direction;
};

const std::vector<PlanMetricSpec> &plan_metric_specs();

struct PlanCandidate
{
    // Stable within one exploration. Reports are ordered by it, so two runs
    // over the same candidates read alike.
    size_t        id { 0 };
    PlanObjective objective { PlanObjective::Balanced };
    std::string   label;

    PlanMetrics metrics;

    std::vector<PlanAssumption> assumptions;

    // A plan with any blocking finding is not offered, whatever its numbers.
    std::vector<std::string> blocking;
    std::vector<std::string> warnings;

    // False when the generator could not produce a valid plan for this
    // objective at all. The reason is shown in place of the plan; the
    // objective is never silently dropped from the list.
    bool        generated { true };
    std::string ungenerated_reason;
};

// What one candidate gives up relative to the best any feasible candidate
// achieved on that metric.
struct Compromise
{
    std::string metric;
    double      best { 0. };
    double      this_plan { 0. };
    // Always positive: how much worse this plan is on this metric.
    double amount { 0. };
    // Absent when the best value is zero, so no ratio is invented.
    std::optional<double> relative;
    // Who achieves the best value. More than one when they tie.
    std::vector<size_t> achieved_by;
};

// Why one candidate is not on the frontier.
struct Domination
{
    size_t dominated_id { 0 };
    // Every candidate that dominates it, sorted. Not just the first one found.
    std::vector<size_t> dominated_by;
};

// Whether one candidate dominates another, and why not when it does not.
enum class DominanceVerdict : uint8_t
{
    Dominates,
    DoesNotDominate,
    // At least one metric was not measured on both sides, so no claim about
    // dominance can be made at all. Not the same as not dominating.
    Unprovable,
};

const char *dominance_verdict_name(DominanceVerdict verdict);

struct DominanceResult
{
    DominanceVerdict verdict { DominanceVerdict::Unprovable };
    // Metrics that could not be compared, and why.
    std::vector<std::string> unmeasured;
    // Metrics on which the first candidate is strictly better, and worse.
    std::vector<std::string> better;
    std::vector<std::string> worse;
};

DominanceResult dominance(const PlanCandidate &a, const PlanCandidate &b);

// The outcome for one objective: which candidate is actually best at it.
struct ObjectiveOutcome
{
    PlanObjective objective { PlanObjective::Balanced };

    // Sorted. More than one when they tie; a tie is reported, never broken by
    // index or by any second metric nobody asked for.
    std::vector<size_t> best;
    bool                tied { false };

    // Set when the objective has no winner: the metric it is judged on was
    // never measured on any feasible plan, or no plan was generated for it.
    std::string unavailable_reason;

    bool available() const { return ! best.empty(); }
};

struct ExplorerPolicy
{
    // Differences smaller than this are a tie, per metric.
    double time_s_epsilon { 1.0 };
    double toolchange_epsilon { 0.5 };
    double heating_s_epsilon { 1.0 };
    double waste_mm3_epsilon { 1.0 };
    double flow_epsilon { 0.01 };
    double detail_epsilon { 0.01 };
    double margin_epsilon { 0.01 };
    double collision_mm_epsilon { 0.01 };

    // When true, a plan whose filament accounting is incomplete may still sit
    // on the frontier, but the incompleteness is reported against it.
    bool allow_incomplete_filament { true };
};

struct PlanExploration
{
    // Every candidate that was offered, feasible or not, sorted by id. The
    // list is the whole list; nothing is filtered out of sight.
    std::vector<size_t> all;

    // Candidates with no blocking findings that were actually generated.
    std::vector<size_t> feasible;
    // Why each excluded candidate was excluded, by id.
    std::map<size_t, std::string> excluded;

    // Non-dominated feasible candidates, sorted.
    std::vector<size_t> frontier;
    std::vector<Domination> dominated;

    // Pairs that could not be ordered at all because a metric was missing on
    // one side. Recorded so a frontier is never read as "these are the only
    // good plans" when some comparison was never possible.
    std::vector<std::pair<size_t, size_t>> unprovable_pairs;

    std::vector<ObjectiveOutcome> objectives;

    // What each frontier plan gives up, by id. Every frontier plan has an
    // entry, empty only when it really is best at everything measured.
    std::map<size_t, std::vector<Compromise>> compromises;

    // Set only when one candidate dominates every other feasible one. A
    // frontier with more than one member is a decision, and the decision is
    // the user's.
    std::optional<size_t> recommended;
    std::string           no_recommendation_reason;

    std::vector<std::string> notes;
};

PlanExploration explore_plans(const std::vector<PlanCandidate> &candidates,
                              const ExplorerPolicy             &policy);

// --- selection -------------------------------------------------------------

enum class SelectionVerdict : uint8_t
{
    Accepted,
    // The plan exists but is not offerable: blocking findings, or it was never
    // generated.
    Infeasible,
    UnknownPlan,
    // The plan is dominated by another. Selectable, but only deliberately.
    NeedsAcknowledgement,
};

const char *selection_verdict_name(SelectionVerdict verdict);

struct SelectionResult
{
    SelectionVerdict verdict { SelectionVerdict::UnknownPlan };
    std::string      message;

    std::optional<size_t> selected;
    // What the selection gives up, carried with it so it is on screen at the
    // moment of choosing and not only in the explorer.
    std::vector<Compromise> compromises;
    // The load-bearing assumptions of the selected plan, likewise.
    std::vector<PlanAssumption> load_bearing;

    bool ok() const { return verdict == SelectionVerdict::Accepted; }
};

SelectionResult select_plan(const PlanExploration &exploration,
                            const std::vector<PlanCandidate> &candidates, size_t id,
                            bool acknowledged_dominated);

} // namespace Slic3r

#endif // slic3r_PlanExplorer_hpp_
