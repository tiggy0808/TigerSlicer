///|/ TigerSlicer Phase 25B: bounded lead/lag scheduler for the experimental
///|/ bounded-Z colour/tool batching mode.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BoundedBatchScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *batch_policy_name(BatchPolicy policy)
{
    switch (policy) {
    case BatchPolicy::Conservative: return "conservative";
    case BatchPolicy::Balanced:     return "balanced";
    case BatchPolicy::Aggressive:   return "aggressive";
    }
    return "conservative";
}

const char *schedule_mode_name(ScheduleMode mode)
{
    switch (mode) {
    case ScheduleMode::Legacy:         return "ordinary";
    case ScheduleMode::BoundedBatch:   return "bounded-Z batching";
    case ScheduleMode::FallbackLegacy: return "ordinary, after batching was ruled out";
    }
    return "ordinary";
}

const char *batch_verdict_name(BatchVerdict verdict)
{
    switch (verdict) {
    case BatchVerdict::Batched:  return "batched";
    case BatchVerdict::Reduced:  return "batched, every batch cut short";
    case BatchVerdict::FellBack: return "fell back to the ordinary schedule";
    case BatchVerdict::Blocked:  return "blocked";
    case BatchVerdict::Disabled: return "disabled";
    }
    return "disabled";
}

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

std::string component_name(size_t id) { return "component " + std::to_string(id); }

// How much of the configured allowance a policy actually spends.
double policy_fraction(BatchPolicy policy)
{
    switch (policy) {
    case BatchPolicy::Conservative: return 0.5;
    case BatchPolicy::Balanced:     return 0.75;
    case BatchPolicy::Aggressive:   return 1.0;
    }
    return 0.5;
}

double xy_separation(const BatchComponent &a, const BatchComponent &b)
{
    const double dx = std::max({ a.min_x - b.max_x, b.min_x - a.max_x, 0. });
    const double dy = std::max({ a.min_y - b.max_y, b.min_y - a.max_y, 0. });
    // Boxes that overlap on one axis are only separated by the other.
    if (dx <= 0. && dy <= 0.)
        return 0.;
    return std::max(dx, dy);
}

ScheduleMetrics measure(const std::vector<ScheduledOperation> &operations,
                        const std::vector<BatchComponent>     &components,
                        const BatchingOptions                 &options)
{
    ScheduleMetrics metrics;
    for (const ScheduledOperation &operation : operations) {
        metrics.total_time_s += operation.duration_s;
        if (operation.toolchange_before) {
            ++metrics.toolchanges;
            metrics.total_time_s += options.toolchange_cost_s;
        }
        if (operation.downward_move)
            ++metrics.downward_moves;
        metrics.max_idle_s = std::max(metrics.max_idle_s, operation.idle_before_s);
    }

    // Lead is measured over the schedule rather than assumed from the limits,
    // and it is measured here so that all three schedules - by-layer,
    // bounded-batch and complete-object - are compared on the same terms.
    if (! components.empty()) {
        std::vector<double> current(components.size());
        for (size_t index = 0; index < components.size(); ++index)
            current[index] = components[index].layers.empty()
                                 ? 0.
                                 : components[index].layers.front().z_bottom_mm;
        std::vector<size_t> done(components.size(), 0);

        for (const ScheduledOperation &operation : operations) {
            for (size_t index = 0; index < components.size(); ++index)
                if (components[index].id == operation.component) {
                    current[index] = operation.z_top_mm;
                    ++done[index];
                }
            double lowest  = current.front();
            double highest = current.front();
            size_t fewest  = done.front();
            size_t most    = done.front();
            for (size_t index = 0; index < components.size(); ++index) {
                lowest  = std::min(lowest, current[index]);
                highest = std::max(highest, current[index]);
                fewest  = std::min(fewest, done[index]);
                most    = std::max(most, done[index]);
            }
            metrics.max_z_lead_mm     = std::max(metrics.max_z_lead_mm, highest - lowest);
            metrics.max_z_lead_layers = std::max(metrics.max_z_lead_layers, most - fewest);
        }
    }

    std::optional<double> closest;
    for (size_t left = 0; left < components.size(); ++left)
        for (size_t right = left + 1; right < components.size(); ++right) {
            const double gap = xy_separation(components[left], components[right]);
            closest          = closest.has_value() ? std::min(*closest, gap) : gap;
        }
    metrics.clearance_margin_mm = closest;
    return metrics;
}

} // namespace

ScheduleMetrics measure_schedule(const std::vector<ScheduledOperation> &operations,
                                 const std::vector<BatchComponent>     &components,
                                 const BatchingOptions                 &options)
{
    return measure(operations, components, options);
}

std::vector<ScheduledOperation> complete_object_schedule(
    const std::vector<BatchComponent> &components)
{
    std::vector<const BatchComponent *> ordered;
    for (const BatchComponent &component : components)
        ordered.push_back(&component);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const BatchComponent *a, const BatchComponent *b) {
                         return a->id < b->id;
                     });

    std::vector<ScheduledOperation> operations;
    std::optional<int>              current_tool;
    double                          highest_z = 0.;
    bool                            first     = true;

    for (const BatchComponent *component : ordered) {
        for (size_t index = 0; index < component->layers.size(); ++index) {
            const BatchLayer &layer = component->layers[index];

            ScheduledOperation operation;
            operation.component   = component->id;
            operation.layer       = index;
            operation.tool        = layer.tool;
            operation.z_bottom_mm = layer.z_bottom_mm;
            operation.z_top_mm    = layer.z_top_mm;
            operation.duration_s  = layer.print_time_s;
            operation.toolchange_before =
                current_tool.has_value() && *current_tool != layer.tool;
            // One batch per object, so only its first layer starts one.
            operation.starts_batch  = index == 0;
            operation.downward_move = ! first && layer.z_top_mm < highest_z;

            current_tool = layer.tool;
            if (first || layer.z_top_mm > highest_z)
                highest_z = layer.z_top_mm;
            first = false;
            operations.push_back(operation);
        }
    }
    return operations;
}

std::vector<ScheduledOperation> ordinary_schedule(
    const std::vector<BatchComponent> &components)
{
    struct Entry
    {
        double z_top;
        size_t component;
        size_t layer;
    };

    std::vector<Entry> entries;
    for (const BatchComponent &component : components)
        for (size_t index = 0; index < component.layers.size(); ++index)
            entries.push_back({ component.layers[index].z_top_mm, component.id, index });

    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.z_top != b.z_top)
            return a.z_top < b.z_top;
        return a.component < b.component;
    });

    auto component_by_id = [&](size_t id) -> const BatchComponent * {
        for (const BatchComponent &component : components)
            if (component.id == id)
                return &component;
        return nullptr;
    };

    std::vector<ScheduledOperation> operations;
    std::optional<int>              current_tool;
    for (const Entry &entry : entries) {
        const BatchComponent *component = component_by_id(entry.component);
        if (component == nullptr)
            continue;
        const BatchLayer &layer = component->layers[entry.layer];

        ScheduledOperation operation;
        operation.component        = entry.component;
        operation.layer            = entry.layer;
        operation.tool             = layer.tool;
        operation.z_bottom_mm      = layer.z_bottom_mm;
        operation.z_top_mm         = layer.z_top_mm;
        operation.duration_s       = layer.print_time_s;
        operation.toolchange_before =
            current_tool.has_value() && *current_tool != layer.tool;
        operation.starts_batch = true;
        current_tool           = layer.tool;
        operations.push_back(operation);
    }
    return operations;
}

BatchPlan plan_bounded_batches(const std::vector<BatchComponent> &components,
                               const ComponentDecomposition      &decomposition,
                               const BatchingOptions             &options)
{
    BatchPlan plan;

    if (components.empty()) {
        plan.error = "no components were supplied, so there is nothing to schedule";
        return plan;
    }
    for (const BatchComponent &component : components)
        if (component.layers.empty()) {
            plan.error = component_name(component.id) +
                         " has no layers, so the schedule would silently leave it out";
            return plan;
        }

    plan.operations     = ordinary_schedule(components);
    plan.legacy_metrics = measure(plan.operations, components, options);
    plan.metrics        = plan.legacy_metrics;

    if (plan.operations.size() > options.max_operations) {
        plan.mode    = ScheduleMode::Legacy;
        plan.verdict = BatchVerdict::Blocked;
        plan.notes.push_back("this print has " + std::to_string(plan.operations.size()) +
                             " layer operations, beyond the " +
                             std::to_string(options.max_operations) +
                             " this scheduler will reason about; batching is not offered");
        return plan;
    }

    if (!options.enabled) {
        plan.mode    = ScheduleMode::Legacy;
        plan.verdict = BatchVerdict::Disabled;
        plan.notes.push_back("experimental bounded-Z batching is off; this is the ordinary "
                             "by-layer schedule and nothing about it has changed");
        return plan;
    }

    // Everything from here needs a reason to refuse or a reason to proceed.
    auto rule_out = [&](const std::string &why) {
        plan.notes.push_back(why);
        if (options.allow_reported_fallback) {
            plan.mode    = ScheduleMode::FallbackLegacy;
            plan.verdict = BatchVerdict::FellBack;
            plan.notes.push_back("the ordinary schedule is used instead, and this fallback "
                                 "is reported rather than silent");
        } else {
            plan.mode    = ScheduleMode::Legacy;
            plan.verdict = BatchVerdict::Blocked;
            plan.operations.clear();
            plan.metrics = ScheduleMetrics {};
            plan.notes.push_back("a reported fallback was not permitted, so nothing is "
                                 "offered for export rather than exporting a schedule that "
                                 "was not verified");
        }
        return plan;
    };

    if (!decomposition.batching_eligible)
        return rule_out("the print was not shown to decompose into independent components, "
                        "so no component may be printed ahead of another");

    for (const BatchComponent &component : components) {
        const bool known =
            std::any_of(decomposition.components.begin(), decomposition.components.end(),
                        [&](const DependencyComponent &known_component) {
                            return known_component.id == component.id;
                        });
        if (!known)
            return rule_out(component_name(component.id) +
                            " is not in the dependency decomposition, so whether anything "
                            "depends on it is unknown");
    }

    if (components.size() < 2)
        return rule_out("there is only one component, so there is nothing to interleave it "
                        "with");

    for (size_t left = 0; left < components.size(); ++left)
        for (size_t right = left + 1; right < components.size(); ++right) {
            const double gap = xy_separation(components[left], components[right]);
            if (gap < options.min_component_separation_mm - options.tolerance_mm)
                return rule_out(component_name(components[left].id) + " and " +
                                component_name(components[right].id) + " are " +
                                number(gap, 2) + " mm apart, closer than the " +
                                number(options.min_component_separation_mm, 2) +
                                " mm the toolhead needs to work over one without reaching "
                                "the other");
        }

    if (!options.allow_downward_z)
        return rule_out("coming back to a lagging component means descending to a lower z, "
                        "and downward z motion has not been permitted; without it no "
                        "component can be printed ahead of another");

    // Effective allowances after the policy has had its say.
    const double fraction = policy_fraction(options.policy);
    const size_t effective_layers =
        std::max<size_t>(1, size_t(double(options.max_layers_per_batch) * fraction));
    const double effective_lead_mm = options.max_z_lead_mm * fraction;
    const size_t effective_lead_layers =
        std::max<size_t>(1, size_t(double(options.max_z_lead_layers) * fraction));

    plan.notes.push_back(
        std::string("the ") + batch_policy_name(options.policy) + " policy spends " +
        number(fraction * 100., 0) + "% of the configured allowance: at most " +
        std::to_string(effective_layers) + " layers per batch and " +
        number(effective_lead_mm, 2) + " mm of lead");
    if (!options.automatic_batch_size)
        plan.notes.push_back("a fixed batch size of " +
                             std::to_string(options.max_layers_per_batch) +
                             " layers was requested; it is an upper bound and every "
                             "dependency, lead and clearance limit still applies under it");

    // The schedule itself.
    struct Progress
    {
        size_t next { 0 };
        double z { 0. };
        double last_active_s { 0. };
        bool   started { false };
    };

    std::vector<size_t> order(components.size());
    for (size_t index = 0; index < components.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return components[a].id < components[b].id;
    });

    std::vector<Progress> progress(components.size());
    for (size_t index = 0; index < components.size(); ++index)
        progress[index].z = components[index].layers.front().z_bottom_mm;

    std::vector<ScheduledOperation> batched;
    std::optional<int>              current_tool;
    double                          now       = 0.;
    double                          highest_z = 0.;
    size_t                          batches   = 0;
    bool                            every_batch_cut = false;
    bool                            any_batch_full  = false;

    auto remaining = [&](size_t index) {
        return progress[index].next < components[index].layers.size();
    };

    while (true) {
        // The lagging component goes next; ties by component id, so the same
        // print always schedules the same way.
        std::optional<size_t> active;
        for (size_t index : order) {
            if (!remaining(index))
                continue;
            if (!active.has_value() ||
                progress[index].z < progress[*active].z - options.tolerance_mm)
                active = index;
        }
        if (!active.has_value())
            break;

        const BatchComponent &component = components[*active];
        size_t                in_batch  = 0;
        std::string           limit;

        while (remaining(*active)) {
            if (in_batch >= effective_layers) {
                limit = "the maximum of " + std::to_string(effective_layers) +
                        " layers per batch";
                break;
            }

            const BatchLayer &layer = component.layers[progress[*active].next];

            std::optional<double> lowest_other;
            for (size_t index : order) {
                if (index == *active || !remaining(index))
                    continue;
                if (!lowest_other.has_value() || progress[index].z < *lowest_other)
                    lowest_other = progress[index].z;
            }

            if (lowest_other.has_value()) {
                if (layer.z_top_mm > *lowest_other + effective_lead_mm +
                                         options.tolerance_mm) {
                    limit = "the maximum z lead of " + number(effective_lead_mm, 2) +
                            " mm over the component furthest behind";
                    break;
                }
                if (in_batch >= effective_lead_layers) {
                    limit = "the maximum z lead of " +
                            std::to_string(effective_lead_layers) + " layers";
                    break;
                }

                // Would going on leave another component sitting too long?
                double longest_idle = 0.;
                for (size_t index : order) {
                    if (index == *active || !remaining(index))
                        continue;
                    if (progress[index].started)
                        longest_idle = std::max(longest_idle,
                                                now + layer.print_time_s -
                                                    progress[index].last_active_s);
                }
                if (longest_idle > options.max_idle_s + options.tolerance_mm) {
                    limit = "the maximum idle time of " + number(options.max_idle_s, 0) +
                            " s, past which a waiting component oozes and cools";
                    break;
                }
            }

            ScheduledOperation operation;
            operation.component   = component.id;
            operation.layer       = progress[*active].next;
            operation.tool        = layer.tool;
            operation.z_bottom_mm = layer.z_bottom_mm;
            operation.z_top_mm    = layer.z_top_mm;
            operation.duration_s  = layer.print_time_s;
            operation.starts_batch = in_batch == 0;
            operation.toolchange_before =
                current_tool.has_value() && *current_tool != layer.tool;
            operation.downward_move =
                in_batch == 0 && layer.z_bottom_mm < highest_z - options.tolerance_mm;
            if (in_batch == 0 && progress[*active].started)
                operation.idle_before_s = now - progress[*active].last_active_s;

            if (operation.toolchange_before)
                now += options.toolchange_cost_s;
            now += layer.print_time_s;

            current_tool = layer.tool;
            highest_z    = std::max(highest_z, layer.z_top_mm);
            progress[*active].z             = layer.z_top_mm;
            progress[*active].started       = true;
            progress[*active].last_active_s = now;
            ++progress[*active].next;
            ++in_batch;

            batched.push_back(operation);
        }

        if (in_batch == 0) {
            // Nothing could be emitted for the lagging component, which means
            // the limits contradict each other rather than bounding anything.
            return rule_out("no layer could be scheduled for " +
                            component_name(component.id) +
                            " without breaking a limit, so the bounded schedule cannot make "
                            "progress: " + (limit.empty() ? std::string("no reason was "
                                                                       "recorded")
                                                          : limit));
        }

        ++batches;
        if (!limit.empty())
            plan.limits.push_back(component_name(component.id) + "'s batch stopped after " +
                                  std::to_string(in_batch) + " layers at " +
                                  number(progress[*active].z, 2) + " mm: " + limit);
        else
            any_batch_full = true;
    }

    every_batch_cut = !any_batch_full;

    // Lead metrics come out of measure(), over the schedule rather than
    // assumed from the limits.
    ScheduleMetrics batch_metrics = measure(batched, components, options);

    const long saved_changes = long(plan.legacy_metrics.toolchanges) -
                               long(batch_metrics.toolchanges);
    const double saving_s = double(saved_changes) * options.toolchange_cost_s;
    const double extra_cost =
        double(batches > 0 ? batches - 1 : 0) * options.reprime_cost_s;
    const double net = saving_s - extra_cost;

    if (net <= options.toolchange_saving_threshold_s + EPSILON) {
        plan.toolchanges_saved = saved_changes;
        plan.time_saved_s      = net;
        return rule_out(
            "batching would remove " + std::to_string(saved_changes) +
            " toolchange(s), worth " + number(saving_s, 1) + " s, and cost " +
            number(extra_cost, 1) + " s in repriming across " +
            std::to_string(batches) + " batches - a net " + number(net, 1) +
            " s, which does not clear the " +
            number(options.toolchange_saving_threshold_s, 1) + " s it was asked to beat");
    }

    plan.operations        = batched;
    plan.metrics           = batch_metrics;
    plan.mode              = ScheduleMode::BoundedBatch;
    plan.verdict           = every_batch_cut ? BatchVerdict::Reduced : BatchVerdict::Batched;
    plan.toolchanges_saved = saved_changes;
    plan.time_saved_s      = net;
    plan.notes.push_back("this schedule moves global z downward " +
                         std::to_string(batch_metrics.downward_moves) +
                         " time(s); every one of those descents still has to be proven "
                         "clear before it is emitted");
    return plan;
}

} // namespace Slic3r
