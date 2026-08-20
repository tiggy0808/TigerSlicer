///|/ TigerSlicer Phase 3: toolchange cost and elapsed-time model.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ToolchangeCostModel.hpp"

#include <algorithm>

namespace Slic3r {

double ToolchangeCost::total_s() const
{
    return park_s + pickup_s + travel_s + heating_wait_s + purge_s + brush_s + prime_s +
           probe_s + retry_allowance_s;
}

double PrintTimeBreakdown::total_s() const
{
    return extrusion_s + travel_s + toolchange_motion_s + heating_wait_s + wipe_prime_s + probe_s;
}

void PrintTimeBreakdown::add(const ToolchangeCost &cost)
{
    // Motion the toolchange itself performs, kept separate from model travel
    // so the two can be reported independently.
    toolchange_motion_s += cost.park_s + cost.pickup_s + cost.travel_s + cost.retry_allowance_s;
    heating_wait_s      += cost.heating_wait_s;
    wipe_prime_s        += cost.purge_s + cost.brush_s + cost.prime_s;
    probe_s             += cost.probe_s;
}

ToolchangeCost estimate_toolchange_cost(const ToolchangeRequest &request,
                                        const MacroTimingContract &contract,
                                        const ToolThermalState &incoming)
{
    ToolchangeCost cost;

    // A change to the same tool is not a change.
    if (request.from_tool == request.to_tool)
        return cost;

    cost.park_s   = std::max(0., contract.park_s);
    cost.pickup_s = std::max(0., contract.pickup_s);

    if (request.travel_speed_mm_s > 0. && request.dock_travel_mm > 0.)
        cost.travel_s = request.dock_travel_mm / request.travel_speed_mm_s;

    // --- heating -----------------------------------------------------------
    // The incoming tool sits at standby (or has drifted toward idle) and must
    // reach its active temperature. Any part of that which fits inside the
    // lead time overlaps with printing and is free.
    if (incoming.heat_rate_c_s > 0.) {
        const double deficit = incoming.active_temperature_c - incoming.current_temperature_c;
        if (deficit > 0.) {
            const double heat_time = deficit / incoming.heat_rate_c_s;
            cost.heating_wait_s = std::max(0., heat_time - std::max(0., request.preheat_lead_s));
        }
    }

    // --- purge / brush / prime ----------------------------------------------
    if (request.purge_flow_mm3_s > 0. && request.purge_volume_mm3 > 0.)
        cost.purge_s = request.purge_volume_mm3 / request.purge_flow_mm3_s;
    cost.purge_volume_mm3 = std::max(0., request.purge_volume_mm3);

    cost.brush_s = std::max(0, request.brush_passes) * std::max(0., contract.brush_pass_s);
    cost.prime_s = std::max(0., contract.prime_s);

    if (request.probe_after_pickup)
        cost.probe_s = std::max(0., contract.probe_s);

    // Retry allowance is only budgeted when the caller asks for it, so a plan
    // that assumes reliable pickups is not silently padded.
    if (request.allow_retry_allowance)
        cost.retry_allowance_s = std::max(0., contract.retry_allowance_s);

    return cost;
}

} // namespace Slic3r
