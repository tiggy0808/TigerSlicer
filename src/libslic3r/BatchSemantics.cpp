///|/ TigerSlicer Phase 25D: scheduling semantics and quality for the
///|/ experimental bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BatchSemantics.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace Slic3r {

const char *semantic_verdict_name(SemanticVerdict verdict)
{
    switch (verdict) {
    case SemanticVerdict::Sound:    return "sound";
    case SemanticVerdict::Degraded: return "degraded";
    case SemanticVerdict::Unsound:  return "unsound";
    case SemanticVerdict::Unproven: return "unproven";
    }
    return "unproven";
}

const char *semantic_severity_name(SemanticSeverity severity)
{
    switch (severity) {
    case SemanticSeverity::Info:     return "info";
    case SemanticSeverity::Warning:  return "warning";
    case SemanticSeverity::Blocking: return "blocking";
    }
    return "info";
}

SemanticSeverity worse_of(SemanticSeverity a, SemanticSeverity b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

const char *semantic_issue_name(SemanticIssue issue)
{
    switch (issue) {
    case SemanticIssue::Chronology:     return "chronology";
    case SemanticIssue::Prerequisite:   return "prerequisite";
    case SemanticIssue::InterfacePlane: return "interface plane";
    case SemanticIssue::SealedAccess:   return "sealed access";
    case SemanticIssue::Bonding:        return "bonding";
    case SemanticIssue::Ooze:           return "ooze";
    case SemanticIssue::ReprimeCost:    return "reprime cost";
    case SemanticIssue::Estimation:     return "estimation";
    case SemanticIssue::Checkpoint:     return "checkpoint";
    case SemanticIssue::CancelObject:   return "cancel object";
    }
    return "estimation";
}

bool SemanticReview::emittable() const
{
    return verdict == SemanticVerdict::Sound || verdict == SemanticVerdict::Degraded;
}

size_t SemanticReview::count(SemanticSeverity severity) const
{
    size_t total = 0;
    for (const SemanticFinding &finding : findings)
        if (finding.severity == severity)
            ++total;
    return total;
}

namespace {

std::string number(double value, int precision = 1)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

const ComponentSemantics *semantics_of(const std::vector<ComponentSemantics> &components,
                                       size_t                                 id)
{
    for (const ComponentSemantics &component : components)
        if (component.id == id)
            return &component;
    return nullptr;
}

} // namespace

ScheduleEstimate estimate_schedule(const BatchPlan                       &plan,
                                   const std::vector<ComponentSemantics> &components,
                                   const SemanticPolicy                  &policy)
{
    ScheduleEstimate estimate;

    // Schedule order, not z order. With a bounded-Z batch the two are
    // different prints, and only one of them is going to be run.
    std::map<size_t, size_t>          index_of; // component id -> timeline slot
    std::optional<int>                held_tool;
    double                            clock = 0.;
    bool                              first = true;

    for (const ScheduledOperation &op : plan.operations) {
        auto slot = index_of.find(op.component);
        if (slot == index_of.end()) {
            ComponentTimeline timeline;
            timeline.component     = op.component;
            timeline.tool          = op.tool;
            timeline.last_touched_s = clock;
            index_of[op.component] = estimate.components.size();
            estimate.components.push_back(timeline);
            slot = index_of.find(op.component);
        }

        ComponentTimeline &timeline = estimate.components[slot->second];

        if (! held_tool.has_value() || *held_tool != op.tool) {
            if (held_tool.has_value()) {
                ++estimate.toolchanges;
                estimate.toolchange_time_s += policy.toolchange_cost_s;
                clock += policy.toolchange_cost_s;
            }
            held_tool = op.tool;
        }

        if (op.starts_batch && timeline.layers_printed > 0) {
            ++timeline.returns;
            ++estimate.returns;
            estimate.reprime_time_s += policy.reprime_cost_s;
            clock += policy.reprime_cost_s;
        }

        clock += op.duration_s;
        timeline.elapsed_s += op.duration_s;
        timeline.last_touched_s = clock;
        timeline.last_z_mm      = op.z_top_mm;
        ++timeline.layers_printed;

        if (first || op.z_top_mm > estimate.peak_z_mm + policy.tolerance_mm) {
            estimate.peak_z_mm = op.z_top_mm;
        } else if (op.z_top_mm < estimate.peak_z_mm - policy.tolerance_mm) {
            estimate.non_monotonic_z = true;
        }
        first = false;
    }

    // Worst idle per component, with the operation it happened before. This is
    // a second pass because the answer wants an operation index, and threading
    // one through the accumulation above buys nothing.
    {
        std::map<size_t, double> last_touch;
        double                   pass_clock = 0.;
        std::optional<int>       tool;
        for (size_t i = 0; i < plan.operations.size(); ++i) {
            const ScheduledOperation &op = plan.operations[i];
            if (! tool.has_value() || *tool != op.tool) {
                if (tool.has_value())
                    pass_clock += policy.toolchange_cost_s;
                tool = op.tool;
            }
            auto seen = last_touch.find(op.component);
            if (seen != last_touch.end()) {
                const double idle = pass_clock - seen->second;
                ComponentTimeline &timeline = estimate.components[index_of[op.component]];
                if (idle > timeline.max_idle_s) {
                    timeline.max_idle_s                = idle;
                    timeline.max_idle_before_operation = i;
                }
            }
            if (op.starts_batch && seen != last_touch.end())
                pass_clock += policy.reprime_cost_s;
            pass_clock += op.duration_s;
            last_touch[op.component] = pass_clock;
        }
    }

    estimate.total_time_s = clock;
    estimate.final_tool   = held_tool;
    (void) components;
    return estimate;
}

std::vector<RecoverableOperation> batch_boundary_operations(
    const BatchPlan &plan, const std::vector<ComponentSemantics> &components)
{
    // The component semantics are not consulted here on purpose: where the
    // machine may be left is a property of the schedule, not of what the
    // component is made of.
    (void) components;

    std::vector<RecoverableOperation> boundaries;
    double                            elapsed = 0.;

    for (size_t i = 0; i < plan.operations.size(); ++i) {
        const ScheduledOperation &op = plan.operations[i];
        elapsed += op.duration_s;

        const bool ends_batch = i + 1 == plan.operations.size() ||
                                plan.operations[i + 1].starts_batch;
        if (! ends_batch)
            continue;

        RecoverableOperation boundary;
        boundary.index    = i;
        boundary.tool     = op.tool;
        boundary.print_z  = op.z_top_mm;
        boundary.elapsed_s = elapsed;
        // A batch boundary is a place the tool is about to leave, which is a
        // place the machine can be left. Whether the melt zone is still under
        // pressure is not recorded here, and Unknown forces a reprime rather
        // than pretending it was relieved.
        boundary.pressure                 = PressureState::Unknown;
        boundary.safe_to_interrupt_after  = true;
        boundary.on_synchronization_plane = false;

        boundaries.push_back(boundary);
    }

    return boundaries;
}

SemanticReview review_batch_semantics(const BatchPlan                         &plan,
                                      const std::vector<ComponentSemantics>   &components,
                                      const std::vector<InterfaceRequirement> &interfaces,
                                      const std::vector<AccessRequirement>    &access,
                                      const SemanticPolicy                    &policy)
{
    SemanticReview review;
    review.estimate = estimate_schedule(plan, components, policy);

    auto add = [&](SemanticIssue issue, SemanticSeverity severity,
                   std::optional<size_t> operation, std::optional<size_t> component,
                   bool unmeasured, std::string message) {
        SemanticFinding finding;
        finding.issue      = issue;
        finding.severity   = severity;
        finding.operation  = operation;
        finding.component  = component;
        finding.unmeasured = unmeasured;
        finding.message    = std::move(message);
        review.findings.push_back(std::move(finding));
    };

    if (plan.operations.empty()) {
        review.verdict = SemanticVerdict::Sound;
        review.notes.push_back("this schedule contains no operations, so there is nothing "
                               "about it to be wrong");
        return review;
    }

    // --- the walk ----------------------------------------------------------
    std::map<size_t, size_t> printed_layers; // component -> layers already printed
    std::map<size_t, double> highest_z;      // component -> highest z_top printed
    std::map<size_t, bool>   fine_tool_done; // access requirement index -> satisfied
    std::map<int, double>    tool_last_used;
    std::map<size_t, double> component_last_touched;

    double             clock = 0.;
    std::optional<int> held_tool;
    double             peak_z          = 0.;
    bool               saw_descent     = false;
    double             duration_total  = 0.;
    bool               chronology_broken = false;

    for (size_t i = 0; i < plan.operations.size(); ++i) {
        const ScheduledOperation &op        = plan.operations[i];
        const ComponentSemantics *semantics = semantics_of(components, op.component);

        // Toolchange and ooze, on the schedule's clock.
        if (! held_tool.has_value() || *held_tool != op.tool) {
            if (held_tool.has_value()) {
                auto last = tool_last_used.find(op.tool);
                if (last != tool_last_used.end()) {
                    const double idle = clock - last->second;
                    if (policy.ooze_window_s.has_value() && idle > *policy.ooze_window_s)
                        add(SemanticIssue::Ooze, SemanticSeverity::Warning, i, op.component,
                            false,
                            "T" + std::to_string(op.tool) + " sat hot and idle for " +
                                number(idle) + " s, past the " + number(*policy.ooze_window_s) +
                                " s it is known to ooze after, before it was picked back up");
                }
                tool_last_used[*held_tool] = clock;
                clock += policy.toolchange_cost_s;
            }
            held_tool = op.tool;
        }

        // Chronology: a component's own layers stay in its own order, whatever
        // the global z is doing.
        const size_t printed  = printed_layers.count(op.component) ? printed_layers[op.component] : 0;
        const bool   returned = component_last_touched.count(op.component) > 0;
        if (op.layer != printed) {
            chronology_broken = true;
            add(SemanticIssue::Chronology, SemanticSeverity::Blocking, i, op.component, false,
                "component " + std::to_string(op.component) + " has printed " +
                    std::to_string(printed) + " layer(s) but this operation is its layer " +
                    std::to_string(op.layer) +
                    "; batching may reorder components against each other, never a "
                    "component against itself");
        }

        // Prerequisites: nothing gets advanced ahead of what carries it.
        if (semantics != nullptr) {
            for (const auto &entry : semantics->prerequisites) {
                if (entry.first != op.layer)
                    continue;
                const LayerPrerequisite &need = entry.second;
                const size_t             have = printed_layers.count(need.component)
                                                    ? printed_layers[need.component]
                                                    : 0;
                if (have <= need.layer)
                    add(SemanticIssue::Prerequisite, SemanticSeverity::Blocking, i,
                        op.component, false,
                        "layer " + std::to_string(op.layer) + " of component " +
                            std::to_string(op.component) + " needs layer " +
                            std::to_string(need.layer) + " of component " +
                            std::to_string(need.component) + " first (" + need.what +
                            "), and that layer has not been printed; batching it here would "
                            "print into free air");
            }
        }

        // Cross-tool interface planes: neither side goes past the plane until
        // both sides have reached it.
        for (const InterfaceRequirement &interface : interfaces) {
            size_t self  = 0;
            size_t other = 0;
            if (op.component == interface.region_a) {
                self  = interface.region_a;
                other = interface.region_b;
            } else if (op.component == interface.region_b) {
                self  = interface.region_b;
                other = interface.region_a;
            } else {
                continue;
            }
            if (op.z_top_mm <= interface.print_z + policy.tolerance_mm)
                continue;
            const double reached = highest_z.count(other) ? highest_z[other] : 0.;
            if (reached < interface.print_z - policy.tolerance_mm)
                add(SemanticIssue::InterfacePlane, SemanticSeverity::Blocking, i, op.component,
                    false,
                    "component " + std::to_string(self) + " (T" + std::to_string(interface.tool_a) +
                        "/T" + std::to_string(interface.tool_b) + ") would print to " +
                        number(op.z_top_mm, 2) + " mm past the interface plane at " +
                        number(interface.print_z, 2) + " mm while component " +
                        std::to_string(other) + " has only reached " + number(reached, 2) +
                        " mm; the two tools meet at that plane and both have to be there");
        }

        // A coarse layer must not close over an access a finer tool still
        // needs.
        for (size_t a = 0; a < access.size(); ++a) {
            const AccessRequirement &need = access[a];
            if (need.component != op.component)
                continue;
            if (op.tool == need.fine_tool) {
                fine_tool_done[a] = true;
                continue;
            }
            if (fine_tool_done.count(a) && fine_tool_done[a])
                continue;
            if (! need.sealed_above_z_mm.has_value()) {
                add(SemanticIssue::SealedAccess, SemanticSeverity::Warning, i, op.component,
                    true,
                    "the height at which " + need.what + " in component " +
                        std::to_string(op.component) + " is sealed to T" +
                        std::to_string(need.fine_tool) +
                        " was never established, so this schedule cannot be shown not to "
                        "seal it");
                continue;
            }
            if (op.z_top_mm > *need.sealed_above_z_mm + policy.tolerance_mm)
                add(SemanticIssue::SealedAccess, SemanticSeverity::Blocking, i, op.component,
                    false,
                    "printing component " + std::to_string(op.component) + " to " +
                        number(op.z_top_mm, 2) + " mm with T" + std::to_string(op.tool) +
                        " seals " + need.what + " above " +
                        number(*need.sealed_above_z_mm, 2) + " mm, and T" +
                        std::to_string(need.fine_tool) + " has not been in there yet");
        }

        // Bonding: what it cost this component to sit while others printed.
        if (returned) {
            const double idle = clock - component_last_touched[op.component];
            if (semantics == nullptr || ! semantics->bond_window_s.has_value()) {
                if (idle > 0.)
                    add(SemanticIssue::Bonding, SemanticSeverity::Warning, i, op.component,
                        true,
                        "component " + std::to_string(op.component) + " sat for " +
                            number(idle) + " s while other components printed, and how long "
                            "its surface stays bondable was never characterized" +
                            (semantics == nullptr
                                 ? " (no material was described for it at all)"
                                 : " for " + (semantics->material.empty()
                                                  ? std::string("its material")
                                                  : semantics->material)));
            } else if (idle > *semantics->bond_window_s) {
                add(SemanticIssue::Bonding, SemanticSeverity::Warning, i, op.component, false,
                    "component " + std::to_string(op.component) + " cooled for " +
                        number(idle) + " s while other components printed, past the " +
                        number(*semantics->bond_window_s) +
                        " s its surface stays bondable for; the layer printed on top of it "
                        "will not be as strong as it would be in the ordinary schedule");
            }
        }

        if (op.starts_batch && returned)
            clock += policy.reprime_cost_s;
        clock += op.duration_s;
        duration_total += op.duration_s;

        component_last_touched[op.component] = clock;
        tool_last_used[op.tool]              = clock;
        printed_layers[op.component]         = printed + 1;
        if (! highest_z.count(op.component) || op.z_top_mm > highest_z[op.component])
            highest_z[op.component] = op.z_top_mm;

        if (i == 0 || op.z_top_mm > peak_z + policy.tolerance_mm)
            peak_z = op.z_top_mm;
        else if (op.z_top_mm < peak_z - policy.tolerance_mm)
            saw_descent = true;
    }

    // --- estimation across a non-monotonic global Z -------------------------
    // What the schedule's own total has to be: the operations in it, plus the
    // toolchanges between them at the cost this policy prices them at. That is
    // the same quantity the scheduler measures, reconstructed independently
    // from the operations rather than taken from the plan.
    const double reconstructed =
        duration_total + double(review.estimate.toolchanges) * policy.toolchange_cost_s;
    if (std::abs(reconstructed - plan.metrics.total_time_s) > policy.tolerance_s)
        add(SemanticIssue::Estimation, SemanticSeverity::Blocking, std::nullopt, std::nullopt,
            false,
            "the schedule reports " + number(plan.metrics.total_time_s, 3) +
                " s but the operations in it, with their " +
                std::to_string(review.estimate.toolchanges) + " toolchange(s), add up to " +
                number(reconstructed, 3) +
                " s; with a non-monotonic global Z an estimate accumulated in z order is "
                "an estimate of a different print");

    if (saw_descent && plan.metrics.downward_moves == 0)
        add(SemanticIssue::Estimation, SemanticSeverity::Blocking, std::nullopt, std::nullopt,
            false,
            "the schedule prints below a height it has already reached but reports no "
            "downward moves, so its own metrics do not describe it");

    if (saw_descent)
        review.notes.push_back("global Z is non-monotonic in this schedule; every estimate "
                               "here was accumulated in emission order, which is the order "
                               "the machine will run");

    if (! policy.ooze_window_s.has_value() && review.estimate.toolchanges > 0)
        add(SemanticIssue::Ooze, SemanticSeverity::Warning, std::nullopt, std::nullopt, true,
            "how long a tool may sit hot before it oozes was never characterized for this "
            "machine, so the " + std::to_string(review.estimate.toolchanges) +
                " toolchange(s) in this schedule cannot be shown not to ooze");

    // --- what repriming cost against what batching saved --------------------
    const double reprime_total = review.estimate.reprime_time_s;
    if (review.estimate.returns > 0) {
        if (! plan.toolchanges_saved.has_value()) {
            add(SemanticIssue::ReprimeCost, SemanticSeverity::Warning, std::nullopt,
                std::nullopt, true,
                "coming back to " + std::to_string(review.estimate.returns) +
                    " component run(s) costs " + number(reprime_total) +
                    " s in repriming, and the schedule does not say how many toolchanges it "
                    "saved, so there is nothing to weigh that against");
        } else {
            const double saved = double(*plan.toolchanges_saved) * policy.toolchange_cost_s;
            if (reprime_total > saved + policy.tolerance_s)
                add(SemanticIssue::ReprimeCost, SemanticSeverity::Warning, std::nullopt,
                    std::nullopt, false,
                    "repriming after " + std::to_string(review.estimate.returns) +
                        " return(s) costs " + number(reprime_total) + " s, more than the " +
                        number(saved) + " s saved by dropping " +
                        std::to_string(*plan.toolchanges_saved) +
                        " toolchange(s); this schedule is slower than the ordinary one");
            else
                review.notes.push_back("repriming costs " + number(reprime_total) +
                                       " s against " + number(saved) +
                                       " s saved in toolchanges");
        }
    }

    // --- recovery checkpoints at the batch boundaries -----------------------
    if (policy.checkpoint_batch_boundaries) {
        const std::vector<RecoverableOperation> boundaries =
            batch_boundary_operations(plan, components);
        RecoveryOptions options = policy.recovery_options;
        options.enabled         = true;
        RecoveryPlan recovery =
            plan_recovery_checkpoints(boundaries, options, policy.recovery_capabilities);

        for (const std::string &limitation : recovery.limitations)
            add(SemanticIssue::Checkpoint, SemanticSeverity::Warning, std::nullopt,
                std::nullopt, false,
                "a checkpoint at each batch boundary was asked for, but " + limitation);

        if (recovery.checkpoints.empty() && ! boundaries.empty() && recovery.limitations.empty())
            add(SemanticIssue::Checkpoint, SemanticSeverity::Warning, std::nullopt,
                std::nullopt, false,
                "no checkpoint could be placed at any of the " +
                    std::to_string(boundaries.size()) +
                    " batch boundaries, so an interrupted batched print has nowhere to "
                    "resume from");

        review.checkpoints = std::move(recovery);
    } else if (review.estimate.returns > 0) {
        add(SemanticIssue::Checkpoint, SemanticSeverity::Info, std::nullopt, std::nullopt,
            false,
            "this schedule leaves and returns to components " +
                std::to_string(review.estimate.returns) +
                " time(s); checkpointing the batch boundaries would give an interrupted "
                "print somewhere to resume from");
    }

    // --- cancel-object metadata --------------------------------------------
    {
        std::map<size_t, CancelObjectRegion> regions;
        for (size_t i = 0; i < plan.operations.size(); ++i) {
            const size_t              component = plan.operations[i].component;
            const ComponentSemantics *semantics = semantics_of(components, component);
            if (semantics == nullptr || ! semantics->cancellable)
                continue;
            CancelObjectRegion &region = regions[component];
            region.component           = component;
            region.name                = semantics->cancel_object_name;
            if (! region.runs.empty() && region.runs.back().second + 1 == i)
                region.runs.back().second = i;
            else
                region.runs.emplace_back(i, i);
        }
        for (auto &entry : regions) {
            CancelObjectRegion &region = entry.second;
            region.contiguous          = region.runs.size() == 1;
            region.actionable          = ! region.name.empty();

            if (region.name.empty())
                add(SemanticIssue::CancelObject, SemanticSeverity::Warning, std::nullopt,
                    region.component, false,
                    "component " + std::to_string(region.component) +
                        " is marked cancellable but has no object name, so the cancel-object "
                        "metadata names nothing a firmware could cancel");
            else if (! region.contiguous)
                add(SemanticIssue::CancelObject, SemanticSeverity::Info, std::nullopt,
                    region.component, false,
                    "batching splits \"" + region.name + "\" into " +
                        std::to_string(region.runs.size()) +
                        " runs; every one of them is marked, because metadata that names "
                        "only the first run would cancel part of an object and leave the "
                        "rest printing");
            review.cancel_objects.push_back(region);
        }
    }

    // --- verdict ------------------------------------------------------------
    SemanticSeverity worst      = SemanticSeverity::Info;
    bool             unmeasured = false;
    for (const SemanticFinding &finding : review.findings) {
        worst = worse_of(worst, finding.severity);
        if (finding.unmeasured)
            unmeasured = true;
    }

    if (worst == SemanticSeverity::Blocking)
        review.verdict = SemanticVerdict::Unsound;
    else if (unmeasured)
        review.verdict = SemanticVerdict::Unproven;
    else if (worst == SemanticSeverity::Warning)
        review.verdict = SemanticVerdict::Degraded;
    else
        review.verdict = SemanticVerdict::Sound;

    if (chronology_broken)
        review.notes.push_back("a component's own layer order was broken, so nothing "
                               "downstream of that point in this review means very much");

    return review;
}

} // namespace Slic3r
