///|/ TigerSlicer Phase 3: toolchange cost and elapsed-time model.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ToolchangeCostModel_hpp_
#define slic3r_ToolchangeCostModel_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Timing contract for the firmware macros that actually perform a toolchange.
// These are configuration, not hardcoded constants: TigerSlicer does not know
// how long a particular machine's dock sequence takes.
struct MacroTimingContract
{
    double park_s { 0. };
    double pickup_s { 0. };
    double probe_s { 0. };          // optional verification after pickup
    double brush_pass_s { 0. };
    double prime_s { 0. };
    // Allowance for a retry the firmware may perform on a failed pickup.
    double retry_allowance_s { 0. };
};

struct ToolThermalState
{
    int    tool { 0 };
    double active_temperature_c { 0. };
    double standby_temperature_c { 0. };
    double idle_temperature_c { 0. };
    // Heating and cooling rates, degrees per second.
    double heat_rate_c_s { 0. };
    double cool_rate_c_s { 0. };
    // Temperature the tool is actually at when the change begins.
    double current_temperature_c { 0. };
};

struct ToolchangeRequest
{
    int from_tool { 0 };
    int to_tool { 0 };

    // Distance from the current position to the dock and back, in mm, with the
    // speed the machine will actually travel at.
    double dock_travel_mm { 0. };
    double travel_speed_mm_s { 0. };

    // Purge/prime volume required for this specific transition.
    double purge_volume_mm3 { 0. };
    double purge_flow_mm3_s { 0. };
    int    brush_passes { 0 };

    // How long the incoming tool has been idle, which drives whether a
    // pre-heat has already completed and how much ooze must be cleared.
    double incoming_idle_s { 0. };

    // Wall-clock available before the incoming tool is needed. Heating that
    // fits inside this window overlaps with printing and costs nothing.
    double preheat_lead_s { 0. };

    bool probe_after_pickup { false };
    bool allow_retry_allowance { false };
};

// Time broken out by cause, so Preview can explain where a print's time goes
// rather than presenting one opaque number.
struct ToolchangeCost
{
    double park_s { 0. };
    double pickup_s { 0. };
    double travel_s { 0. };
    double heating_wait_s { 0. };
    double purge_s { 0. };
    double brush_s { 0. };
    double prime_s { 0. };
    double probe_s { 0. };
    double retry_allowance_s { 0. };

    double total_s() const;
    // Material consumed by this change, for waste accounting.
    double purge_volume_mm3 { 0. };
};

// Aggregate breakdown for a whole print.
struct PrintTimeBreakdown
{
    double extrusion_s { 0. };
    double travel_s { 0. };
    double toolchange_motion_s { 0. };
    double heating_wait_s { 0. };
    double wipe_prime_s { 0. };
    double probe_s { 0. };

    double total_s() const;
    void add(const ToolchangeCost &cost);
};

// Cost one toolchange.
//
// Heating is modelled as an overlap rather than a constant: if the incoming
// tool can reach temperature within the lead time available, the wait is zero.
ToolchangeCost estimate_toolchange_cost(const ToolchangeRequest &request,
                                        const MacroTimingContract &contract,
                                        const ToolThermalState &incoming);

} // namespace Slic3r

#endif // slic3r_ToolchangeCostModel_hpp_
