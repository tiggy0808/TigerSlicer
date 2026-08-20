///|/ TigerSlicer Phase 4: toolchange-aware global schedule optimization.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ToolchangeOptimizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace Slic3r {

const char *optimization_policy_name(OptimizationPolicy policy)
{
    switch (policy) {
    case OptimizationPolicy::Fastest:                    return "fastest";
    case OptimizationPolicy::Balanced:                   return "balanced";
    case OptimizationPolicy::BestDetail:                 return "best detail";
    case OptimizationPolicy::FewestChanges:              return "fewest changes";
    case OptimizationPolicy::LeastWaste:                 return "least waste";
    case OptimizationPolicy::PreserveExplicitAssignment: return "preserve explicit assignment";
    }
    return "unknown";
}

const char *toolchange_necessity_name(ToolchangeNecessity necessity)
{
    switch (necessity) {
    case ToolchangeNecessity::DependencyForced: return "dependency forced";
    case ToolchangeNecessity::ToolExhausted:    return "tool exhausted";
    case ToolchangeNecessity::PolicyChosen:     return "policy chosen";
    }
    return "unknown";
}

double OptimizerInputs::purge_volume_for(int from_tool, int to_tool) const
{
    if (tool_count == 0 || purge_volume_mm3.size() != tool_count * tool_count)
        return 0.;
    if (from_tool < 0 || to_tool < 0 ||
        size_t(from_tool) >= tool_count || size_t(to_tool) >= tool_count)
        return 0.;
    return purge_volume_mm3[size_t(from_tool) * tool_count + size_t(to_tool)];
}

namespace {

// Beyond this many operations the full rollout search is skipped in favour of
// the cheap heuristic. The cap is reported, never applied silently.
constexpr size_t ROLLOUT_OPERATION_CAP = 2000;

const char *dependency_reason_name(MultiRateDependencyReason reason)
{
    switch (reason) {
    case MultiRateDependencyReason::MonotonicZ:         return "monotonic Z";
    case MultiRateDependencyReason::VerticalSupport:    return "vertical support";
    case MultiRateDependencyReason::MaterialInterface:  return "material interface";
    case MultiRateDependencyReason::Enclosure:          return "enclosure";
    case MultiRateDependencyReason::SupportContact:     return "support contact";
    case MultiRateDependencyReason::CollisionClearance: return "collision clearance";
    }
    return "unknown";
}

// Edges that exist because of surface quality rather than mechanics. Delaying
// the predecessor of one of these is what BestDetail refuses to do.
bool is_quality_edge(MultiRateDependencyReason reason)
{
    return reason == MultiRateDependencyReason::MaterialInterface ||
           reason == MultiRateDependencyReason::SupportContact ||
           reason == MultiRateDependencyReason::Enclosure;
}

std::string tool_label(int tool)
{
    return "T" + std::to_string(tool);
}

// Reject inputs that do not describe the schedule, rather than costing work we
// were never told about.
std::string validate_inputs(const MultiRateSchedule &schedule, const OptimizerInputs &inputs)
{
    const size_t count = schedule.operations().size();
    if (count == 0)
        return "the schedule contains no operations";
    if (inputs.extrusion_s.size() != count)
        return "extrusion times were supplied for " + std::to_string(inputs.extrusion_s.size()) +
               " of " + std::to_string(count) + " operations";
    if (!inputs.explicitly_assigned.empty() && inputs.explicitly_assigned.size() != count)
        return "explicit-assignment flags were supplied for " +
               std::to_string(inputs.explicitly_assigned.size()) + " of " +
               std::to_string(count) + " operations";

    size_t highest_tool = size_t(std::max(0, inputs.initial_tool));
    for (const MultiRateOperation &operation : schedule.operations())
        highest_tool = std::max<size_t>(highest_tool, operation.tool_id);

    if (inputs.tool_count <= highest_tool)
        return "tool_count is " + std::to_string(inputs.tool_count) + " but the schedule uses " +
               tool_label(int(highest_tool));
    if (inputs.thermal.size() < inputs.tool_count)
        return "thermal state is missing for " +
               std::to_string(inputs.tool_count - inputs.thermal.size()) + " tool(s)";
    // A multi-tool schedule with no purge characterization would silently cost
    // every change as waste-free.
    if (inputs.tool_count > 1 &&
        inputs.purge_volume_mm3.size() != inputs.tool_count * inputs.tool_count)
        return "purge volumes are not characterized for " + std::to_string(inputs.tool_count) +
               " tools";
    return {};
}

std::vector<size_t> indegrees(const MultiRateSchedule &schedule)
{
    std::vector<size_t> degree(schedule.operations().size(), 0);
    for (const MultiRateDependency &edge : schedule.dependencies())
        if (edge.successor < degree.size())
            ++degree[edge.successor];
    return degree;
}

std::vector<std::vector<size_t>> successor_lists(const MultiRateSchedule &schedule)
{
    std::vector<std::vector<size_t>> successors(schedule.operations().size());
    for (const MultiRateDependency &edge : schedule.dependencies())
        if (edge.predecessor < successors.size())
            successors[edge.predecessor].push_back(edge.successor);
    return successors;
}

// True when this operation is the predecessor of a quality edge.
std::vector<bool> quality_critical(const MultiRateSchedule &schedule)
{
    std::vector<bool> critical(schedule.operations().size(), false);
    for (const MultiRateDependency &edge : schedule.dependencies())
        if (is_quality_edge(edge.reason) && edge.predecessor < critical.size())
            critical[edge.predecessor] = true;
    return critical;
}

ToolchangeCost cost_of_change(const OptimizerInputs &inputs,
                              int from_tool,
                              int to_tool,
                              double preheat_lead_s)
{
    ToolchangeRequest request;
    request.from_tool             = from_tool;
    request.to_tool               = to_tool;
    request.dock_travel_mm        = inputs.dock_travel_mm;
    request.travel_speed_mm_s     = inputs.travel_speed_mm_s;
    request.purge_volume_mm3      = inputs.purge_volume_for(from_tool, to_tool);
    request.purge_flow_mm3_s      = inputs.purge_flow_mm3_s;
    request.brush_passes          = inputs.brush_passes;
    request.preheat_lead_s        = preheat_lead_s;
    request.probe_after_pickup    = inputs.probe_after_pickup;
    request.allow_retry_allowance = inputs.allow_retry_allowance;

    ToolThermalState thermal;
    if (to_tool >= 0 && size_t(to_tool) < inputs.thermal.size())
        thermal = inputs.thermal[size_t(to_tool)];
    return estimate_toolchange_cost(request, inputs.contract, thermal);
}

// Explain why the current tool could not simply continue.
struct Necessity
{
    ToolchangeNecessity value { ToolchangeNecessity::ToolExhausted };
    std::string         explanation;
};

Necessity classify_necessity(const MultiRateSchedule &schedule,
                             const std::vector<bool> &scheduled,
                             const std::vector<size_t> &degree,
                             int current_tool,
                             coordf_t last_z)
{
    Necessity necessity;
    bool remaining_on_current = false;
    bool ready_on_current     = false;
    for (size_t id = 0; id < schedule.operations().size(); ++id) {
        if (scheduled[id] || int(schedule.operations()[id].tool_id) != current_tool)
            continue;
        remaining_on_current = true;
        // Ready means more than in-degree zero: taking it must not move down
        // in Z, or the machine could not perform the order at all.
        if (degree[id] == 0 && schedule.operations()[id].print_z + EPSILON >= last_z)
            ready_on_current = true;
    }

    if (!remaining_on_current) {
        necessity.value       = ToolchangeNecessity::ToolExhausted;
        necessity.explanation = tool_label(current_tool) + " has no remaining work";
        return necessity;
    }

    if (ready_on_current) {
        // Nothing forced this change, so it is attributed to the policy rather
        // than to a constraint that does not exist.
        necessity.value       = ToolchangeNecessity::PolicyChosen;
        necessity.explanation = "ready work remained on " + tool_label(current_tool);
        return necessity;
    }

    // Work remained but none of it was ready: name the constraint holding it.
    necessity.value = ToolchangeNecessity::DependencyForced;
    for (const MultiRateDependency &edge : schedule.dependencies()) {
        if (edge.successor >= scheduled.size() || scheduled[edge.successor])
            continue;
        if (int(schedule.operations()[edge.successor].tool_id) != current_tool)
            continue;
        if (edge.predecessor < scheduled.size() && scheduled[edge.predecessor])
            continue;
        necessity.explanation = "all remaining " + tool_label(current_tool) +
                                " work is blocked by a " +
                                dependency_reason_name(edge.reason) + " dependency";
        return necessity;
    }
    necessity.explanation = "all remaining " + tool_label(current_tool) +
                            " work would require moving down in Z";
    return necessity;
}

} // namespace

ScheduleEvaluation evaluate_schedule(const MultiRateSchedule &schedule,
                                     const std::vector<size_t> &order,
                                     const OptimizerInputs &inputs,
                                     std::string *error)
{
    ScheduleEvaluation evaluation;
    auto fail = [&](const std::string &message) {
        if (error != nullptr)
            *error = message;
        return ScheduleEvaluation {};
    };
    if (error != nullptr)
        error->clear();

    const std::string invalid = validate_inputs(schedule, inputs);
    if (!invalid.empty())
        return fail(invalid);

    const size_t count = schedule.operations().size();
    if (order.size() != count)
        return fail("the order lists " + std::to_string(order.size()) + " of " +
                    std::to_string(count) + " operations");

    std::vector<bool> seen(count, false);
    for (size_t id : order) {
        if (id >= count)
            return fail("the order references operation " + std::to_string(id) +
                        ", which does not exist");
        if (seen[id])
            return fail("the order repeats operation " + std::to_string(id));
        seen[id] = true;
    }

    // A mis-costed schedule is worse than no schedule, so the order is checked
    // against the dependency graph before any time is attributed to it.
    std::vector<size_t> position(count, 0);
    for (size_t index = 0; index < order.size(); ++index)
        position[order[index]] = index;
    for (const MultiRateDependency &edge : schedule.dependencies())
        if (edge.predecessor < count && edge.successor < count &&
            position[edge.predecessor] >= position[edge.successor])
            return fail("operation " + std::to_string(edge.successor) + " is scheduled before " +
                        std::to_string(edge.predecessor) + ", violating its " +
                        dependency_reason_name(edge.reason) + " dependency");

    // Z order is a constraint in its own right: not every schedule carries
    // explicit monotonic-Z edges, and a downward move is not printable.
    coordf_t previous_z = -std::numeric_limits<coordf_t>::max();
    for (size_t id : order) {
        if (schedule.operations()[id].print_z + EPSILON < previous_z)
            return fail("operation " + std::to_string(id) + " would move downward in Z");
        previous_z = std::max(previous_z, schedule.operations()[id].print_z);
    }

    std::vector<size_t> degree = indegrees(schedule);
    const std::vector<std::vector<size_t>> successors = successor_lists(schedule);
    std::vector<bool> scheduled(count, false);

    int      current_tool = inputs.initial_tool;
    double   batch_time_s = 0.;
    coordf_t last_z       = -std::numeric_limits<coordf_t>::max();

    evaluation.order = order;
    for (size_t id : order) {
        const MultiRateOperation &operation = schedule.operations()[id];
        const int tool = int(operation.tool_id);

        if (tool != current_tool) {
            // Everything printed since the last change was time the incoming
            // tool could have spent heating.
            const ToolchangeCost cost = cost_of_change(inputs, current_tool, tool, batch_time_s);
            const Necessity necessity =
                classify_necessity(schedule, scheduled, degree, current_tool, last_z);

            ToolchangeEvent event;
            event.operation   = id;
            event.from_tool   = current_tool;
            event.to_tool     = tool;
            event.cost        = cost;
            event.necessity   = necessity.value;
            event.explanation = tool_label(current_tool) + " -> " + tool_label(tool) + ": " +
                                necessity.explanation;
            evaluation.toolchanges.push_back(std::move(event));

            evaluation.breakdown.add(cost);
            evaluation.purge_volume_mm3 += cost.purge_volume_mm3;
            current_tool = tool;
            batch_time_s = 0.;
        }

        const double extrusion_s = inputs.extrusion_s[id];
        evaluation.breakdown.extrusion_s += extrusion_s;
        batch_time_s += extrusion_s;

        scheduled[id] = true;
        last_z        = std::max(last_z, operation.print_z);
        for (size_t successor : successors[id])
            if (degree[successor] > 0)
                --degree[successor];
    }

    return evaluation;
}

namespace {

// Everything the greedy walk needs to continue from a partial schedule.
struct WalkState
{
    std::vector<bool>   scheduled;
    std::vector<size_t> degree;
    std::vector<size_t> order;
    int                 current_tool { 0 };
    coordf_t            last_z { -std::numeric_limits<coordf_t>::max() };
    size_t              explicit_head { 0 };
};

// Operations that may be scheduled next.
//
// Z is a hard constraint, so only the lowest ready layer is a candidate:
// taking a higher operation first would strand every lower one, since coming
// back down is not printable.
std::vector<size_t> candidates(const MultiRateSchedule &schedule,
                               const WalkState &state,
                               const std::vector<size_t> &explicit_queue,
                               const std::vector<bool> &explicitly_assigned)
{
    std::vector<size_t> ready;
    coordf_t minimum_z = std::numeric_limits<coordf_t>::max();
    for (size_t id = 0; id < schedule.operations().size(); ++id) {
        if (state.scheduled[id] || state.degree[id] != 0)
            continue;
        if (!explicit_queue.empty() && !explicitly_assigned.empty() && explicitly_assigned[id] &&
            (state.explicit_head >= explicit_queue.size() ||
             explicit_queue[state.explicit_head] != id))
            continue;
        ready.push_back(id);
        minimum_z = std::min(minimum_z, schedule.operations()[id].print_z);
    }

    std::vector<size_t> band;
    for (size_t id : ready)
        if (std::abs(schedule.operations()[id].print_z - minimum_z) <= EPSILON)
            band.push_back(id);
    return band;
}

void advance(const MultiRateSchedule &schedule,
             const std::vector<std::vector<size_t>> &successors,
             const std::vector<size_t> &explicit_queue,
             WalkState &state,
             size_t id)
{
    state.scheduled[id] = true;
    state.order.push_back(id);
    state.current_tool = int(schedule.operations()[id].tool_id);
    state.last_z       = std::max(state.last_z, schedule.operations()[id].print_z);
    if (!explicit_queue.empty() && state.explicit_head < explicit_queue.size() &&
        explicit_queue[state.explicit_head] == id)
        ++state.explicit_head;
    for (size_t successor : successors[id])
        if (state.degree[successor] > 0)
            --state.degree[successor];
}

// The fixed continuation rule. Comparing two decisions is only meaningful when
// everything after them is decided the same way, so this must stay
// deterministic and free of policy-specific scoring.
size_t greedy_pick(const MultiRateSchedule &schedule,
                   const std::vector<size_t> &band,
                   const WalkState &state,
                   OptimizationPolicy policy,
                   const std::vector<bool> &critical)
{
    if (policy == OptimizationPolicy::BestDetail) {
        // Quality-critical work goes as early as the graph allows, even when
        // that costs a change.
        size_t best = band.size();
        for (size_t id : band)
            if (critical[id] && (best == band.size() || id < best))
                best = id;
        if (best != band.size())
            return best;
    }
    for (size_t id : band)
        if (int(schedule.operations()[id].tool_id) == state.current_tool)
            return id;
    return *std::min_element(band.begin(), band.end());
}

// Finish a partial schedule with the fixed continuation rule, optionally
// forcing the first operation. Returns an empty order if it gets stuck.
std::vector<size_t> rollout(const MultiRateSchedule &schedule,
                            const std::vector<std::vector<size_t>> &successors,
                            const std::vector<size_t> &explicit_queue,
                            const std::vector<bool> &explicitly_assigned,
                            const std::vector<bool> &critical,
                            OptimizationPolicy policy,
                            WalkState state,
                            size_t forced_first)
{
    const size_t count = schedule.operations().size();
    if (forced_first < count)
        advance(schedule, successors, explicit_queue, state, forced_first);

    while (state.order.size() < count) {
        const std::vector<size_t> band =
            candidates(schedule, state, explicit_queue, explicitly_assigned);
        if (band.empty())
            return {};
        advance(schedule, successors, explicit_queue, state,
                greedy_pick(schedule, band, state, policy, critical));
    }
    return state.order;
}

double objective_value(const ScheduleEvaluation &evaluation,
                       OptimizationPolicy policy,
                       const std::vector<bool> &critical)
{
    switch (policy) {
    case OptimizationPolicy::FewestChanges:
        return double(evaluation.toolchange_count());
    case OptimizationPolicy::LeastWaste:
        return evaluation.purge_volume_mm3;
    case OptimizationPolicy::BestDetail: {
        // Sum of the positions of quality-critical work: lower means that work
        // happens earlier, which is the whole point of the policy.
        double sum = 0.;
        for (size_t index = 0; index < evaluation.order.size(); ++index)
            if (evaluation.order[index] < critical.size() && critical[evaluation.order[index]])
                sum += double(index);
        return sum;
    }
    case OptimizationPolicy::Balanced: {
        // Time, charged a second time for the material a change throws away.
        const double waste_s = evaluation.purge_volume_mm3;
        return evaluation.total_s() + waste_s;
    }
    case OptimizationPolicy::Fastest:
    case OptimizationPolicy::PreserveExplicitAssignment:
    default:
        return evaluation.total_s();
    }
}

} // namespace

OptimizationResult optimize_toolchanges(const MultiRateSchedule &schedule,
                                        const OptimizerInputs &inputs,
                                        OptimizationPolicy policy)
{
    OptimizationResult result;

    result.error = validate_inputs(schedule, inputs);
    if (!result.error.empty())
        return result;

    std::string baseline_error;
    const std::vector<size_t> baseline_order = schedule.ordered_operation_ids(&baseline_error);
    if (!baseline_error.empty()) {
        result.error = baseline_error;
        return result;
    }

    result.baseline = evaluate_schedule(schedule, baseline_order, inputs, &result.error);
    if (!result.error.empty())
        return result;

    const size_t count = schedule.operations().size();
    const std::vector<std::vector<size_t>> successors = successor_lists(schedule);
    const std::vector<bool> critical = quality_critical(schedule);

    // Explicit work keeps its relative baseline order under the policy that
    // promises exactly that. Everything else may move freely.
    std::vector<size_t> explicit_queue;
    if (policy == OptimizationPolicy::PreserveExplicitAssignment &&
        !inputs.explicitly_assigned.empty())
        for (size_t id : baseline_order)
            if (inputs.explicitly_assigned[id])
                explicit_queue.push_back(id);

    const bool use_rollout = count <= ROLLOUT_OPERATION_CAP;
    if (!use_rollout)
        result.notes.push_back("lookahead was limited to the local heuristic because the "
                               "schedule has " + std::to_string(count) +
                               " operations, above the rollout limit of " +
                               std::to_string(ROLLOUT_OPERATION_CAP));

    WalkState state;
    state.scheduled.assign(count, false);
    state.degree       = indegrees(schedule);
    state.current_tool = inputs.initial_tool;
    state.order.reserve(count);

    while (state.order.size() < count) {
        const std::vector<size_t> band =
            candidates(schedule, state, explicit_queue, inputs.explicitly_assigned);
        if (band.empty()) {
            result.error = explicit_queue.empty()
                               ? std::string("the dependency graph cannot be ordered without "
                                             "moving down in Z")
                               : std::string("preserving the explicit assignment order conflicts "
                                             "with the dependency graph");
            return result;
        }

        // Continuing on the current tool is free, so it is never worth a search.
        size_t chosen        = band.size();
        bool   need_decision = true;
        if (policy == OptimizationPolicy::BestDetail) {
            for (size_t id : band)
                if (critical[id] && (chosen == band.size() || id < chosen)) {
                    chosen        = id;
                    need_decision = false;
                }
        }
        if (need_decision)
            for (size_t id : band)
                if (int(schedule.operations()[id].tool_id) == state.current_tool) {
                    chosen        = id;
                    need_decision = false;
                    break;
                }

        if (need_decision) {
            // A change is unavoidable. Roll the whole remaining schedule out
            // once per reachable tool and keep the branch that ends up best,
            // so the choice accounts for what each tool unlocks later rather
            // than only for what it costs now.
            std::set<int> tools;
            for (size_t id : band)
                tools.insert(int(schedule.operations()[id].tool_id));

            double best = std::numeric_limits<double>::max();
            for (int tool : tools) {
                size_t first = count;
                for (size_t id : band)
                    if (int(schedule.operations()[id].tool_id) == tool) {
                        first = id;
                        break;
                    }
                if (first == count)
                    continue;
                if (chosen == band.size())
                    chosen = first;
                if (!use_rollout)
                    continue;

                const std::vector<size_t> candidate_order =
                    rollout(schedule, successors, explicit_queue, inputs.explicitly_assigned,
                            critical, policy, state, first);
                if (candidate_order.size() != count)
                    continue;
                std::string ignored;
                const ScheduleEvaluation evaluation =
                    evaluate_schedule(schedule, candidate_order, inputs, &ignored);
                if (!ignored.empty())
                    continue;
                const double score = objective_value(evaluation, policy, critical);
                if (score < best) {
                    best   = score;
                    chosen = first;
                }
            }
        }

        if (chosen == band.size())
            chosen = *std::min_element(band.begin(), band.end());
        advance(schedule, successors, explicit_queue, state, chosen);
    }

    // The necessity of each change is classified once, against the finished
    // order, so the optimizer and the report cannot disagree.
    std::string optimized_error;
    ScheduleEvaluation candidate =
        evaluate_schedule(schedule, state.order, inputs, &optimized_error);
    if (!optimized_error.empty()) {
        // The optimizer produced something the evaluator rejects. Report the
        // baseline rather than a schedule that was never validated.
        result.optimized = result.baseline;
        result.improved  = false;
        result.notes.push_back("optimization was discarded: " + optimized_error);
        for (const ToolchangeEvent &event : result.baseline.toolchanges)
            result.unavoidable.push_back(event.explanation);
        return result;
    }

    const double baseline_objective  = objective_value(result.baseline, policy, critical);
    const double optimized_objective = objective_value(candidate, policy, critical);

    if (optimized_objective < baseline_objective) {
        result.optimized = std::move(candidate);
        result.improved  = true;
    } else {
        // Nothing was gained. Reporting the baseline as "optimized" would be a
        // false claim of improvement.
        result.optimized = result.baseline;
        result.improved  = false;
    }

    // Explain the difference by transition, so a user can see which changes
    // went away and which the constraints kept.
    std::map<std::pair<int, int>, int> delta;
    for (const ToolchangeEvent &event : result.baseline.toolchanges)
        ++delta[{ event.from_tool, event.to_tool }];
    for (const ToolchangeEvent &event : result.optimized.toolchanges)
        --delta[{ event.from_tool, event.to_tool }];
    for (const auto &entry : delta) {
        if (entry.second == 0)
            continue;
        std::ostringstream line;
        line << tool_label(entry.first.first) << " -> " << tool_label(entry.first.second) << ": "
             << std::abs(entry.second) << " change(s) "
             << (entry.second > 0 ? "removed" : "introduced");
        (entry.second > 0 ? result.eliminated : result.introduced).push_back(line.str());
    }

    for (const ToolchangeEvent &event : result.optimized.toolchanges)
        result.unavoidable.push_back(event.explanation);

    return result;
}

} // namespace Slic3r
