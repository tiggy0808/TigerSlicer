///|/ TigerSlicer Phase 7: dock, brush, purge and prime station planner.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "StationPlanner.hpp"

#include <algorithm>
#include <sstream>

namespace Slic3r {

const char *station_kind_name(StationKind kind)
{
    switch (kind) {
    case StationKind::Dock:          return "dock";
    case StationKind::Brush:         return "brush";
    case StationKind::PurgePosition: return "purge position";
    case StationKind::Cutter:        return "cutter";
    case StationKind::Bucket:        return "bucket";
    case StationKind::PurgeObject:   return "purge object";
    case StationKind::WipeTower:     return "wipe tower";
    }
    return "unknown";
}

bool Station::serves(int tool) const
{
    if (serves_tools.empty())
        return true;
    return std::find(serves_tools.begin(), serves_tools.end(), tool) != serves_tools.end();
}

namespace {

std::string tool_label(int tool)
{
    return "T" + std::to_string(tool);
}

std::string volume(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value << " mm3";
    return out.str();
}

std::string describe(const Station &station)
{
    return station.name.empty() ? std::string(station_kind_name(station.kind))
                                : station.name + " (" + station_kind_name(station.kind) + ")";
}

// A wipe tower is software geometry: it needs no configured coordinate,
// because the slicer places it. Everything else is a physical location the
// machine will move to, and a wrong guess there is a crash.
bool needs_configured_position(StationKind kind)
{
    return kind != StationKind::WipeTower && kind != StationKind::PurgeObject;
}

bool usable(const Station &station, int tool)
{
    if (!station.serves(tool))
        return false;
    return !needs_configured_position(station.kind) || station.position.configured;
}

} // namespace

std::vector<std::string> validate_stations(const std::vector<Station> &stations)
{
    std::vector<std::string> problems;
    for (const Station &station : stations) {
        if (needs_configured_position(station.kind) && !station.position.configured)
            problems.push_back(describe(station) +
                               " has no configured position, so it cannot be used; TigerSlicer "
                               "does not assume a reference printer's coordinates");
        if (station.pass_duration_s < 0.)
            problems.push_back(describe(station) + " has a negative pass duration");
        if (station.remaining_capacity_mm3.has_value() && *station.remaining_capacity_mm3 < 0.)
            problems.push_back(describe(station) + " has a negative remaining capacity");
    }
    return problems;
}

PreparationPlan plan_preparation(const PreparationContext   &context,
                                 const std::vector<Station> &stations)
{
    PreparationPlan plan;

    if (context.from_tool == context.to_tool) {
        plan.notes.push_back("no tool change, so no preparation is required");
        return plan;
    }

    auto add_step = [&](const Station &station, const std::string &action, double duration_s,
                        double material_mm3, const std::string &reason) {
        PreparationStep step;
        step.station      = station.id;
        step.kind         = station.kind;
        step.station_name = describe(station);
        step.action       = action;
        step.duration_s   = std::max(0., duration_s);
        step.material_mm3 = std::max(0., material_mm3);
        step.reason       = reason;
        plan.total_duration_s   += step.duration_s;
        plan.total_material_mm3 += step.material_mm3;
        plan.steps.push_back(std::move(step));
    };

    auto find_usable = [&](StationKind kind, int tool) -> const Station * {
        for (const Station &station : stations)
            if (station.kind == kind && usable(station, tool))
                return &station;
        return nullptr;
    };

    // A station that exists but cannot be used should say why, rather than
    // reading as if the machine simply lacks the hardware.
    auto note_unusable = [&](StationKind kind, int tool) {
        for (const Station &station : stations) {
            if (station.kind != kind)
                continue;
            if (!station.serves(tool))
                plan.notes.push_back(describe(station) + " does not serve " + tool_label(tool));
            else if (needs_configured_position(kind) && !station.position.configured)
                plan.notes.push_back(describe(station) + " has no configured position");
        }
    };

    // --- park the outgoing tool ---------------------------------------------
    if (const Station *dock = find_usable(StationKind::Dock, context.from_tool)) {
        add_step(*dock, "park " + tool_label(context.from_tool), dock->pass_duration_s, 0.,
                 "the outgoing tool must be parked before another is picked up");
    } else {
        note_unusable(StationKind::Dock, context.from_tool);
        plan.unmet.push_back("no usable dock serves " + tool_label(context.from_tool));
    }

    if (const Station *dock = find_usable(StationKind::Dock, context.to_tool)) {
        add_step(*dock, "pick up " + tool_label(context.to_tool), dock->pass_duration_s, 0.,
                 "the incoming tool is collected from its dock");
    } else {
        note_unusable(StationKind::Dock, context.to_tool);
        plan.unmet.push_back("no usable dock serves " + tool_label(context.to_tool));
    }

    // --- trim the tip if asked ----------------------------------------------
    if (context.requires_tip_cut) {
        if (const Station *cutter = find_usable(StationKind::Cutter, context.to_tool)) {
            add_step(*cutter, "cut the filament tip", cutter->pass_duration_s, 0.,
                     "a trimmed tip was requested for this transition");
        } else {
            note_unusable(StationKind::Cutter, context.to_tool);
            plan.unmet.push_back("a tip cut was required but no usable cutter serves " +
                                 tool_label(context.to_tool));
        }
    }

    // --- absorb the purge ----------------------------------------------------
    //
    // Hardware first. If the machine can swallow the waste, a wipe tower is
    // not needed at all, which is the whole point of having a bucket.
    double remaining = std::max(0., context.required_purge_volume_mm3);
    if (remaining > 0.) {
        const StationKind order[] = { StationKind::Bucket, StationKind::PurgePosition,
                                      StationKind::PurgeObject, StationKind::WipeTower };
        for (StationKind kind : order) {
            if (remaining <= 0.)
                break;
            for (const Station &station : stations) {
                if (remaining <= 0.)
                    break;
                if (station.kind != kind || !usable(station, context.to_tool))
                    continue;

                double take = remaining;
                if (station.remaining_capacity_mm3.has_value()) {
                    take = std::min(remaining, std::max(0., *station.remaining_capacity_mm3));
                    if (take <= 0.) {
                        plan.notes.push_back(describe(station) +
                                             " is full and can absorb nothing");
                        continue;
                    }
                    if (take < remaining)
                        plan.notes.push_back(describe(station) + " can absorb only " +
                                             volume(take) + " of the required " +
                                             volume(remaining) + " and needs emptying");
                }

                std::ostringstream reason;
                reason << "purging " << volume(take) << " after "
                       << (context.from_material.empty() ? std::string("the previous material")
                                                         : context.from_material)
                       << " -> "
                       << (context.to_material.empty() ? std::string("the next material")
                                                       : context.to_material);
                add_step(station, "purge " + volume(take), 0., take, reason.str());

                if (kind == StationKind::WipeTower)
                    plan.wipe_tower_required = true;
                remaining -= take;
            }
        }

        plan.unabsorbed_purge_mm3 = std::max(0., remaining);
        if (plan.unabsorbed_purge_mm3 > 0.)
            // Waste with nowhere to go is a real problem; dropping it silently
            // would mean extruding onto the part.
            plan.unmet.push_back("nothing on this machine can absorb " +
                                 volume(plan.unabsorbed_purge_mm3) + " of the required purge");
        else if (!plan.wipe_tower_required)
            plan.notes.push_back("hardware cleaning absorbed the whole purge, so no wipe tower "
                                 "is needed for this transition");
    }

    // --- wipe the nozzle -----------------------------------------------------
    if (context.required_brush_passes > 0) {
        if (const Station *brush = find_usable(StationKind::Brush, context.to_tool)) {
            std::ostringstream reason;
            reason << context.required_brush_passes << " pass(es) after "
                   << context.idle_s << " s idle at " << context.temperature_c << " C";
            add_step(*brush, "brush the nozzle",
                     brush->pass_duration_s * context.required_brush_passes, 0., reason.str());
        } else {
            note_unusable(StationKind::Brush, context.to_tool);
            plan.unmet.push_back("a nozzle wipe was required but no usable brush serves " +
                                 tool_label(context.to_tool));
        }
    }

    if (context.depressurized)
        plan.notes.push_back("pressure was released, so the first extrusion after this "
                             "preparation is not yet accurate");

    if (context.nozzle_diameter_mm <= 0.)
        plan.notes.push_back("the incoming nozzle diameter was not reported, so the purge was "
                             "taken as given rather than checked against it");

    return plan;
}

} // namespace Slic3r
