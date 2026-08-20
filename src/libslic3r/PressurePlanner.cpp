///|/ TigerSlicer Phase 17: per-tool pressure-transient planning.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PressurePlanner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *pressure_event_kind_name(PressureEventKind kind)
{
    switch (kind) {
    case PressureEventKind::Steady:       return "steady";
    case PressureEventKind::Accelerate:   return "acceleration";
    case PressureEventKind::Decelerate:   return "deceleration";
    case PressureEventKind::FlowChange:   return "flow change";
    case PressureEventKind::WidthChange:  return "width change";
    case PressureEventKind::HeightChange: return "height change";
    case PressureEventKind::BridgeEntry:  return "bridge entry";
    case PressureEventKind::BridgeExit:   return "bridge exit";
    case PressureEventKind::Retract:      return "retraction";
    case PressureEventKind::Unretract:    return "unretraction";
    case PressureEventKind::Park:         return "parking";
    case PressureEventKind::Idle:         return "idle";
    case PressureEventKind::Reheat:       return "reheat";
    case PressureEventKind::Prime:        return "prime";
    case PressureEventKind::Pickup:       return "pickup";
    }
    return "steady";
}

const char *pressure_condition_name(PressureCondition condition)
{
    switch (condition) {
    case PressureCondition::Established: return "established";
    case PressureCondition::Released:    return "released";
    case PressureCondition::Recovering:  return "recovering";
    case PressureCondition::Unknown:     return "unknown";
    }
    return "unknown";
}

const char *pressure_action_kind_name(PressureActionKind kind)
{
    switch (kind) {
    case PressureActionKind::SetAdvance: return "set advance";
    case PressureActionKind::LeadIn:     return "lead-in";
    case PressureActionKind::Restore:    return "restore";
    case PressureActionKind::Release:    return "release";
    }
    return "set advance";
}

const char *pressure_severity_name(PressureSeverity severity)
{
    switch (severity) {
    case PressureSeverity::Info:    return "info";
    case PressureSeverity::Warning: return "warning";
    case PressureSeverity::Blocked: return "blocked";
    }
    return "info";
}

bool PressurePlan::has_blocking_note() const
{
    return std::any_of(notes.begin(), notes.end(), [](const PressureNote &note) {
        return note.severity == PressureSeverity::Blocked;
    });
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

std::string substitute(const std::string &hook, int tool, double advance)
{
    if (hook.empty())
        return {};
    std::string result = hook;
    auto        replace_all = [&result](const std::string &token, const std::string &value) {
        for (size_t at = result.find(token); at != std::string::npos;
             at        = result.find(token, at + value.size()))
            result.replace(at, token.size(), value);
    };
    replace_all("[tool]", std::to_string(tool));
    replace_all("[advance]", number(advance, 4));
    return result;
}

// Does this event let the pressure down?
bool releases_pressure(PressureEventKind kind)
{
    return kind == PressureEventKind::Retract || kind == PressureEventKind::Park ||
           kind == PressureEventKind::Idle || kind == PressureEventKind::Reheat ||
           kind == PressureEventKind::Pickup;
}

// Does this event need pressure fully established before it prints?
bool needs_established_pressure(PressureEventKind kind)
{
    return kind == PressureEventKind::BridgeEntry || kind == PressureEventKind::Steady ||
           kind == PressureEventKind::Accelerate || kind == PressureEventKind::FlowChange ||
           kind == PressureEventKind::WidthChange || kind == PressureEventKind::HeightChange;
}

} // namespace

bool PressureAdvanceCurve::covers(double flow_mm3_s) const
{
    if (points.empty())
        return false;
    if (points.size() == 1)
        // A single point is a constant, and a constant covers everything it is
        // declared for.
        return true;
    return flow_mm3_s >= points.front().first - EPSILON &&
           flow_mm3_s <= points.back().first + EPSILON;
}

std::optional<double> resolve_pressure_advance(const std::vector<PressureAdvanceCurve> &curves,
                                               int                                      tool,
                                               const std::string     &material,
                                               std::optional<double>  temperature_c,
                                               double                 flow_mm3_s,
                                               std::string           *reason)
{
    auto refuse = [reason](const std::string &text) -> std::optional<double> {
        if (reason != nullptr)
            *reason = text;
        return std::nullopt;
    };

    const PressureAdvanceCurve *best  = nullptr;
    int                         score = -1;

    for (const PressureAdvanceCurve &curve : curves) {
        if (curve.tool != tool)
            continue;
        if (!curve.material.empty() && curve.material != material)
            continue;
        if (curve.temperature_c.has_value()) {
            if (!temperature_c.has_value())
                // The curve is temperature-specific and we do not know the
                // temperature, so it cannot be claimed to apply.
                continue;
            if (std::abs(*curve.temperature_c - *temperature_c) >
                curve.temperature_tolerance_c + EPSILON)
                continue;
        }
        if (!curve.covers(flow_mm3_s))
            continue;

        const int candidate = (curve.material.empty() ? 0 : 1) +
                              (curve.temperature_c.has_value() ? 2 : 0);
        if (candidate > score) {
            score = candidate;
            best  = &curve;
        }
    }

    if (best == nullptr)
        return refuse("no pressure advance curve covers T" + std::to_string(tool) + " with " +
                      (material.empty() ? "an unrecorded material" : material) + " at " +
                      number(flow_mm3_s, 2) + " mm3/s");

    if (best->points.size() == 1)
        return best->points.front().second;

    // Linear interpolation inside the measured range only; covers() has
    // already refused anything outside it.
    for (size_t index = 1; index < best->points.size(); ++index) {
        const auto &low  = best->points[index - 1];
        const auto &high = best->points[index];
        if (flow_mm3_s <= high.first + EPSILON) {
            const double span = high.first - low.first;
            if (span <= EPSILON)
                return low.second;
            const double t = (flow_mm3_s - low.first) / span;
            return low.second + t * (high.second - low.second);
        }
    }
    return best->points.back().second;
}

PressurePlan plan_pressure(const std::vector<PressureSegment>      &segments,
                           const std::vector<PressureAdvanceCurve> &curves,
                           const PressureCapabilities              &capabilities,
                           const PressurePolicy                    &policy)
{
    PressurePlan plan;

    if (segments.empty()) {
        plan.error = "there are no segments to plan pressure across";
        return plan;
    }

    const bool can_emit =
        capabilities.can_set_pressure_advance && !capabilities.set_advance_gcode.empty();
    if (!can_emit)
        plan.limitations.push_back(
            capabilities.can_set_pressure_advance
                ? "the firmware declares pressure advance but no command is configured, so no "
                  "advance is set"
                : "the firmware does not declare pressure advance, so no advance is set");

    // Pressure state is per tool: parking T0 says nothing about T1.
    std::map<int, PressureCondition>     condition;
    std::map<int, std::optional<double>> applied_advance;

    for (const PressureSegment &segment : segments) {
        if (condition.find(segment.tool) == condition.end())
            condition[segment.tool] = PressureCondition::Unknown;

        PressureCondition &state = condition[segment.tool];

        if (releases_pressure(segment.kind)) {
            state = PressureCondition::Released;
            PressureAction action;
            action.at_segment = segment.index;
            action.tool       = segment.tool;
            action.kind       = PressureActionKind::Release;
            action.reason     = std::string(pressure_event_kind_name(segment.kind)) +
                            " lets the melt zone down";
            plan.actions.push_back(action);
            // A tool that parked has no meaningful advance applied any more.
            applied_advance[segment.tool].reset();
            plan.conditions.push_back(state);
            continue;
        }

        if (segment.kind == PressureEventKind::Prime ||
            segment.kind == PressureEventKind::Unretract) {
            state = PressureCondition::Recovering;
            plan.conditions.push_back(state);
            continue;
        }

        if (!needs_established_pressure(segment.kind)) {
            plan.conditions.push_back(state);
            continue;
        }

        // Set the advance this segment actually needs.
        std::string                 why;
        const std::optional<double> wanted = resolve_pressure_advance(
            curves, segment.tool, segment.material, segment.temperature_c, segment.flow_mm3_s,
            &why);

        if (!wanted.has_value()) {
            // Zero is a real setting meaning "no compensation". Falling back to
            // it would look like a decision rather than a gap.
            plan.notes.push_back({ segment.index, segment.tool, PressureSeverity::Warning,
                                   why + "; the advance in force is left unchanged" });
        } else {
            const std::optional<double> &current = applied_advance[segment.tool];
            if (!current.has_value() ||
                std::abs(*current - *wanted) > policy.min_advance_change_s) {
                PressureAction action;
                action.at_segment = segment.index;
                action.tool       = segment.tool;
                action.kind       = PressureActionKind::SetAdvance;
                action.value      = *wanted;
                action.reason     = "advance " + number(*wanted, 4) + " s for " +
                                number(segment.flow_mm3_s, 2) + " mm3/s at " +
                                number(segment.extrusion_width_mm, 2) + " mm wide";
                if (can_emit) {
                    action.gcode = substitute(capabilities.set_advance_gcode, segment.tool,
                                              *wanted);
                    action.emitted = !action.gcode.empty();
                }
                plan.actions.push_back(action);
                applied_advance[segment.tool] = *wanted;
            }
        }

        // A segment that must print at full pressure, entered from a released
        // or unknown state, needs a lead-in first.
        if (state != PressureCondition::Established) {
            if (segment.on_visible_surface) {
                // Preparation extrusion belongs anywhere except where it will
                // be seen.
                plan.notes.push_back(
                    { segment.index, segment.tool, PressureSeverity::Blocked,
                      "pressure must be restored before this segment, but it prints a visible "
                      "surface, so the lead-in would be seen; move the restoration off the "
                      "visible surface" });
            } else if (segment.available_lead_in_mm + EPSILON < policy.restore_lead_in_mm) {
                // A shortened lead-in under-extrudes silently, which is worse
                // than saying it does not fit.
                plan.notes.push_back(
                    { segment.index, segment.tool, PressureSeverity::Blocked,
                      "restoring pressure needs " + number(policy.restore_lead_in_mm, 2) +
                          " mm of lead-in but only " +
                          number(segment.available_lead_in_mm, 2) + " mm is available" });
            } else {
                PressureAction action;
                action.at_segment = segment.index;
                action.tool       = segment.tool;
                action.kind       = PressureActionKind::LeadIn;
                action.value      = policy.restore_lead_in_mm;
                action.reason = "pressure was " + std::string(pressure_condition_name(state)) +
                                " entering " + pressure_event_kind_name(segment.kind);
                plan.actions.push_back(action);
                state = PressureCondition::Established;
            }
        }

        if (segment.kind == PressureEventKind::BridgeEntry &&
            state == PressureCondition::Established)
            plan.notes.push_back({ segment.index, segment.tool, PressureSeverity::Info,
                                   "the bridge starts at full pressure" });

        if (state == PressureCondition::Unknown || state == PressureCondition::Recovering)
            state = PressureCondition::Established;

        plan.conditions.push_back(state);
    }

    return plan;
}

} // namespace Slic3r
