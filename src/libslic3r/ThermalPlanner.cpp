///|/ TigerSlicer Phase 6: predictive heating and ooze control.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ThermalPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace Slic3r {

const char *thermal_action_kind_name(ThermalActionKind kind)
{
    switch (kind) {
    case ThermalActionKind::Preheat:            return "preheat";
    case ThermalActionKind::HoldStandby:        return "hold standby";
    case ThermalActionKind::DropToIdle:         return "drop to idle";
    case ThermalActionKind::WaitForTemperature: return "wait for temperature";
    }
    return "unknown";
}

const char *thermal_severity_name(ThermalSeverity severity)
{
    switch (severity) {
    case ThermalSeverity::Info:    return "info";
    case ThermalSeverity::Warning: return "warning";
    case ThermalSeverity::Blocked: return "blocked";
    }
    return "unknown";
}

bool ThermalPlan::has_blocking_note() const
{
    for (const ThermalNote &note : notes)
        if (note.severity == ThermalSeverity::Blocked)
            return true;
    return false;
}

namespace {

std::string tool_label(int tool)
{
    return "T" + std::to_string(tool);
}

std::string seconds(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value << " s";
    return out.str();
}

std::string degrees(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(0);
    out << value << " C";
    return out.str();
}

std::string substitute(const std::string &hook, int tool, double temperature)
{
    if (hook.empty())
        return {};
    std::string result = hook;
    const std::string tool_text = std::to_string(tool);
    std::ostringstream temperature_text;
    temperature_text.setf(std::ios::fixed);
    temperature_text.precision(0);
    temperature_text << temperature;

    auto replace_all = [&result](const std::string &token, const std::string &value) {
        for (size_t at = result.find(token); at != std::string::npos;
             at = result.find(token, at + value.size()))
            result.replace(at, token.size(), value);
    };
    replace_all("[tool]", tool_text);
    replace_all("[temperature]", temperature_text.str());
    return result;
}

const MaterialThermalLimits *material_for(const std::vector<MaterialThermalLimits> &materials,
                                          int tool)
{
    if (tool < 0 || size_t(tool) >= materials.size())
        return nullptr;
    return &materials[size_t(tool)];
}

double heat_time(const ToolThermalState &state, double from_c, double to_c)
{
    if (state.heat_rate_c_s <= 0. || to_c <= from_c)
        return 0.;
    return (to_c - from_c) / state.heat_rate_c_s;
}

double cool_time(const ToolThermalState &state, double from_c, double to_c)
{
    if (state.cool_rate_c_s <= 0. || from_c <= to_c)
        return 0.;
    return (from_c - to_c) / state.cool_rate_c_s;
}

} // namespace

ThermalPlan plan_thermals(const std::vector<ToolUsageWindow>       &windows,
                          const std::vector<ToolThermalState>      &thermal,
                          const std::vector<MaterialThermalLimits> &materials,
                          const FirmwareThermalCapabilities        &firmware)
{
    ThermalPlan plan;

    if (windows.empty()) {
        plan.error = "no tool usage windows were supplied";
        return plan;
    }

    std::vector<std::string> missing;
    auto record_missing = [&](const std::string &capability) {
        if (std::find(missing.begin(), missing.end(), capability) == missing.end())
            missing.push_back(capability);
    };

    // Emit through a declared hook or not at all.
    auto emit = [&](ThermalAction &action, bool capability, const std::string &hook,
                    const std::string &capability_name) {
        if (!capability) {
            record_missing(capability_name + " was not declared by the firmware");
            return;
        }
        if (hook.empty()) {
            record_missing(capability_name + " is declared but its G-code hook is not configured");
            return;
        }
        action.gcode   = substitute(hook, action.tool, action.target_c);
        action.emitted = true;
    };

    // Group each tool's uses in time order.
    std::map<int, std::vector<ToolUsageWindow>> by_tool;
    for (const ToolUsageWindow &window : windows) {
        if (window.end_s < window.start_s) {
            plan.error = tool_label(window.tool) + " has a usage window that ends before it starts";
            return plan;
        }
        by_tool[window.tool].push_back(window);
    }
    for (auto &entry : by_tool)
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const ToolUsageWindow &a, const ToolUsageWindow &b) {
                      return a.start_s < b.start_s;
                  });

    for (const auto &entry : by_tool) {
        const int tool = entry.first;
        const std::vector<ToolUsageWindow> &uses = entry.second;

        if (tool < 0 || size_t(tool) >= thermal.size()) {
            plan.notes.push_back({ tool, ThermalSeverity::Blocked,
                                   tool_label(tool) + " has no thermal state, so its heating "
                                                      "cannot be planned" });
            continue;
        }
        const ToolThermalState &state = thermal[size_t(tool)];
        const MaterialThermalLimits *material = material_for(materials, tool);

        if (state.heat_rate_c_s <= 0.) {
            plan.notes.push_back({ tool, ThermalSeverity::Blocked,
                                   tool_label(tool) + " has no heating rate, so the time it needs "
                                                      "to reach temperature is unknown" });
            continue;
        }

        // A target the material cannot survive is refused, never clamped into
        // a range the user did not ask for.
        if (material != nullptr) {
            if (material->max_active_c.has_value() &&
                state.active_temperature_c > *material->max_active_c + EPSILON)
                plan.notes.push_back({ tool, ThermalSeverity::Blocked,
                                       tool_label(tool) + " is set to " +
                                           degrees(state.active_temperature_c) + " but " +
                                           material->material + " is rated to " +
                                           degrees(*material->max_active_c) });
            if (material->min_active_c.has_value() &&
                state.active_temperature_c + EPSILON < *material->min_active_c)
                plan.notes.push_back({ tool, ThermalSeverity::Blocked,
                                       tool_label(tool) + " is set to " +
                                           degrees(state.active_temperature_c) + " but " +
                                           material->material + " needs at least " +
                                           degrees(*material->min_active_c) });
        }

        // --- before the first use ------------------------------------------
        const double first_heat = heat_time(state, state.current_temperature_c,
                                            state.active_temperature_c);
        const double first_start = uses.front().start_s;
        ThermalAction preheat;
        preheat.tool     = tool;
        preheat.kind     = ThermalActionKind::Preheat;
        preheat.target_c = state.active_temperature_c;
        preheat.at_s     = std::max(0., first_start - first_heat);
        preheat.reason   = "ready for its first use at " + seconds(first_start) + "; " +
                         seconds(first_heat) + " of heating from " +
                         degrees(state.current_temperature_c);
        emit(preheat, firmware.can_set_tool_temperature, firmware.set_temperature_gcode,
             "setting a tool temperature");
        plan.actions.push_back(preheat);

        if (first_heat > first_start + EPSILON) {
            // The heat does not fit in the time available. Say so instead of
            // quietly producing an estimate the printer cannot meet.
            const double wait = first_heat - first_start;
            plan.total_heating_wait_s += wait;

            ThermalAction hold;
            hold.tool     = tool;
            hold.kind     = ThermalActionKind::WaitForTemperature;
            hold.target_c = state.active_temperature_c;
            hold.at_s     = first_start;
            hold.reason   = "the print must wait " + seconds(wait) +
                          "; there is not enough lead time to preheat " + tool_label(tool);
            emit(hold, firmware.can_wait_for_temperature, firmware.wait_temperature_gcode,
                 "waiting for a temperature");
            plan.actions.push_back(hold);
            plan.notes.push_back({ tool, ThermalSeverity::Warning,
                                   tool_label(tool) + " cannot reach " +
                                       degrees(state.active_temperature_c) + " before it is "
                                       "needed; the print waits " + seconds(wait) });
        }

        // --- between uses ----------------------------------------------------
        for (size_t index = 0; index + 1 < uses.size(); ++index) {
            const double gap_start = uses[index].end_s;
            const double gap_end   = uses[index + 1].start_s;
            const double gap       = std::max(0., gap_end - gap_start);

            const double to_idle    = cool_time(state, state.active_temperature_c,
                                                state.idle_temperature_c);
            const double from_idle  = heat_time(state, state.idle_temperature_c,
                                                state.active_temperature_c);
            const double to_standby = cool_time(state, state.active_temperature_c,
                                                state.standby_temperature_c);
            const double from_standby = heat_time(state, state.standby_temperature_c,
                                                  state.active_temperature_c);

            auto schedule_return = [&](double target_c, double reheat, const char *from) {
                ThermalAction back;
                back.tool     = tool;
                back.kind     = ThermalActionKind::Preheat;
                back.target_c = state.active_temperature_c;
                back.at_s     = gap_end - reheat;
                back.reason   = std::string("back to printing temperature from ") + from +
                              ", starting " + seconds(reheat) + " before it is needed again";
                emit(back, firmware.can_set_tool_temperature, firmware.set_temperature_gcode,
                     "setting a tool temperature");
                plan.actions.push_back(back);
                (void) target_c;
            };

            if (gap >= to_idle + from_idle) {
                ThermalAction down;
                down.tool     = tool;
                down.kind     = ThermalActionKind::DropToIdle;
                down.target_c = state.idle_temperature_c;
                down.at_s     = gap_start;
                down.reason   = "idle for " + seconds(gap) + ", which is long enough to cool to " +
                              degrees(state.idle_temperature_c) + " and come back in time";
                emit(down, firmware.can_set_tool_temperature, firmware.set_temperature_gcode,
                     "setting a tool temperature");
                plan.actions.push_back(down);
                schedule_return(state.idle_temperature_c, from_idle, "idle");
            } else if (gap >= to_standby + from_standby) {
                ThermalAction down;
                down.tool     = tool;
                down.kind     = ThermalActionKind::HoldStandby;
                down.target_c = state.standby_temperature_c;
                down.at_s     = gap_start;
                down.reason   = "idle for " + seconds(gap) + ", enough for standby at " +
                              degrees(state.standby_temperature_c) + " but not for a full cool-down";
                emit(down, firmware.can_set_standby_temperature, firmware.set_standby_gcode,
                     "setting a standby temperature");
                plan.actions.push_back(down);
                schedule_return(state.standby_temperature_c, from_standby, "standby");
            } else {
                // Too short to be worth cooling: the tool stays hot, and that
                // is counted rather than ignored.
                plan.total_hot_idle_s += gap;
                if (material != nullptr && material->ooze_onset_c.has_value() &&
                    state.active_temperature_c > *material->ooze_onset_c + EPSILON && gap > 0.)
                    plan.notes.push_back(
                        { tool, ThermalSeverity::Info,
                          tool_label(tool) + " sits " + seconds(gap) + " above " +
                              material->material + "'s " + degrees(*material->ooze_onset_c) +
                              " ooze threshold; the gap is too short to cool and return in time" });
            }
        }

        // --- hot idle across the whole print --------------------------------
        double hot_idle_for_tool = 0.;
        for (size_t index = 0; index + 1 < uses.size(); ++index) {
            const double gap = std::max(0., uses[index + 1].start_s - uses[index].end_s);
            const double to_standby = cool_time(state, state.active_temperature_c,
                                                state.standby_temperature_c);
            const double from_standby = heat_time(state, state.standby_temperature_c,
                                                  state.active_temperature_c);
            if (gap < to_standby + from_standby)
                hot_idle_for_tool += gap;
        }
        if (material != nullptr && material->max_hot_idle_s.has_value() &&
            hot_idle_for_tool > *material->max_hot_idle_s + EPSILON)
            plan.notes.push_back({ tool, ThermalSeverity::Warning,
                                   tool_label(tool) + " spends " + seconds(hot_idle_for_tool) +
                                       " hot and idle, beyond " + material->material + "'s " +
                                       seconds(*material->max_hot_idle_s) + " limit" });
        else if (material != nullptr && !material->max_hot_idle_s.has_value() &&
                 hot_idle_for_tool > 0.)
            // Unknown is not the same as fine.
            plan.notes.push_back({ tool, ThermalSeverity::Info,
                                   tool_label(tool) + " spends " + seconds(hot_idle_for_tool) +
                                       " hot and idle; no hot-idle limit is characterized for " +
                                       (material->material.empty() ? std::string("its material")
                                                                   : material->material) });

        // --- after the last use ---------------------------------------------
        ThermalAction finish;
        finish.tool     = tool;
        finish.kind     = ThermalActionKind::DropToIdle;
        finish.target_c = state.idle_temperature_c;
        finish.at_s     = uses.back().end_s;
        finish.reason   = tool_label(tool) + " has no further work in this print";
        emit(finish, firmware.can_set_tool_temperature, firmware.set_temperature_gcode,
             "setting a tool temperature");
        plan.actions.push_back(finish);
    }

    std::sort(plan.actions.begin(), plan.actions.end(),
              [](const ThermalAction &a, const ThermalAction &b) {
                  if (a.at_s != b.at_s)
                      return a.at_s < b.at_s;
                  return a.tool < b.tool;
              });

    plan.limitations = std::move(missing);
    return plan;
}

PrimeRequirement estimate_prime_requirement(const MaterialThermalLimits &material,
                                            double idle_s,
                                            double temperature_c,
                                            bool   depressurized,
                                            double base_purge_volume_mm3,
                                            int    base_brush_passes)
{
    PrimeRequirement requirement;
    requirement.purge_volume_mm3 = std::max(0., base_purge_volume_mm3);
    requirement.brush_passes     = std::max(0, base_brush_passes);
    requirement.reasons.push_back("base preparation for this transition");

    if (!material.ooze_onset_c.has_value()) {
        // Without an ooze threshold there is no basis for adding purge, and
        // inventing one would be a number the user could not check.
        requirement.reasons.push_back(
            "no ooze threshold is characterized for " +
            (material.material.empty() ? std::string("this material") : material.material) +
            ", so no idle-time allowance was added");
    } else if (temperature_c > *material.ooze_onset_c + EPSILON && idle_s > 0.) {
        // Material that oozed out has to be replaced before the first
        // extrusion is trustworthy. Scale with how long it oozed.
        const double excess_c = temperature_c - *material.ooze_onset_c;
        const double extra    = idle_s * excess_c * 0.001;
        requirement.purge_volume_mm3 += extra;
        requirement.brush_passes += 1;
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(1);
        line << "sat " << idle_s << " s at " << temperature_c << " C, "
             << excess_c << " C above the ooze threshold";
        requirement.reasons.push_back(line.str());
    }

    if (depressurized) {
        // Pressure that was deliberately released has to be rebuilt, which is
        // extrusion that produces no useful path.
        requirement.needs_pressure_recovery = true;
        requirement.reasons.push_back("pressure was released and must be rebuilt before the "
                                      "first extrusion is accurate");
    }

    return requirement;
}

} // namespace Slic3r
