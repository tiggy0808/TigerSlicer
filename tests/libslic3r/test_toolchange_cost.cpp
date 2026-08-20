// Phase 3: toolchange cost model. The point of this model is that a toolchange
// is not a constant, so the tests concentrate on heating overlapping with
// printing, on timings coming from the machine's macro contract rather than
// being invented, and on the breakdown accounting for time by cause.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include "libslic3r/ToolchangeCostModel.hpp"

using namespace Slic3r;

namespace {

MacroTimingContract contract()
{
    MacroTimingContract value;
    value.park_s            = 2.;
    value.pickup_s          = 3.;
    value.probe_s           = 4.;
    value.brush_pass_s      = 0.5;
    value.prime_s           = 1.5;
    value.retry_allowance_s = 6.;
    return value;
}

ToolThermalState thermal(double current = 170., double active = 215.)
{
    ToolThermalState state;
    state.tool                  = 1;
    state.active_temperature_c  = active;
    state.standby_temperature_c = 170.;
    state.idle_temperature_c    = 100.;
    state.heat_rate_c_s         = 1.5;
    state.cool_rate_c_s         = 0.5;
    state.current_temperature_c = current;
    return state;
}

ToolchangeRequest request()
{
    ToolchangeRequest value;
    value.from_tool         = 0;
    value.to_tool           = 1;
    value.dock_travel_mm    = 300.;
    value.travel_speed_mm_s = 150.;
    value.purge_volume_mm3  = 60.;
    value.purge_flow_mm3_s  = 12.;
    value.brush_passes      = 4;
    return value;
}

} // namespace

TEST_CASE("A change to the same tool costs nothing", "[toolchange-cost]")
{
    ToolchangeRequest same = request();
    same.to_tool = same.from_tool;

    const ToolchangeCost cost = estimate_toolchange_cost(same, contract(), thermal());
    CHECK(cost.total_s() == 0.);
    CHECK(cost.purge_volume_mm3 == 0.);
}

TEST_CASE("Costs come from the macro contract and the actual motion", "[toolchange-cost]")
{
    const ToolchangeCost cost = estimate_toolchange_cost(request(), contract(), thermal());

    CHECK(cost.park_s == 2.);
    CHECK(cost.pickup_s == 3.);
    CHECK(cost.travel_s == Catch::Approx(2.));    // 300 mm at 150 mm/s
    CHECK(cost.purge_s == Catch::Approx(5.));     // 60 mm3 at 12 mm3/s
    CHECK(cost.brush_s == Catch::Approx(2.));     // 4 passes at 0.5 s
    CHECK(cost.prime_s == 1.5);
}

TEST_CASE("Heating that fits inside the lead time is free", "[toolchange-cost]")
{
    // 45 degrees at 1.5 deg/s is a 30 s heat.
    ToolchangeRequest ample = request();
    ample.preheat_lead_s = 60.;
    CHECK(estimate_toolchange_cost(ample, contract(), thermal()).heating_wait_s == 0.);

    // With no lead time the whole heat is a wait.
    ToolchangeRequest none = request();
    none.preheat_lead_s = 0.;
    CHECK(estimate_toolchange_cost(none, contract(), thermal()).heating_wait_s ==
          Catch::Approx(30.));

    // Partial overlap costs only the remainder.
    ToolchangeRequest partial = request();
    partial.preheat_lead_s = 20.;
    CHECK(estimate_toolchange_cost(partial, contract(), thermal()).heating_wait_s ==
          Catch::Approx(10.));
}

TEST_CASE("A tool already at temperature waits for nothing", "[toolchange-cost]")
{
    const ToolchangeCost cost =
        estimate_toolchange_cost(request(), contract(), thermal(215., 215.));
    CHECK(cost.heating_wait_s == 0.);
}

TEST_CASE("A colder tool costs proportionally more heating", "[toolchange-cost]")
{
    ToolchangeRequest value = request();
    value.preheat_lead_s = 0.;

    const double warm = estimate_toolchange_cost(value, contract(), thermal(170.)).heating_wait_s;
    const double cold = estimate_toolchange_cost(value, contract(), thermal(100.)).heating_wait_s;
    CHECK(cold > warm);
}

TEST_CASE("Probing and retry allowance are only charged when requested", "[toolchange-cost]")
{
    const ToolchangeCost plain = estimate_toolchange_cost(request(), contract(), thermal());
    CHECK(plain.probe_s == 0.);
    // A plan that assumes reliable pickups must not be silently padded.
    CHECK(plain.retry_allowance_s == 0.);

    ToolchangeRequest cautious = request();
    cautious.probe_after_pickup    = true;
    cautious.allow_retry_allowance = true;

    const ToolchangeCost guarded = estimate_toolchange_cost(cautious, contract(), thermal());
    CHECK(guarded.probe_s == 4.);
    CHECK(guarded.retry_allowance_s == 6.);
    CHECK(guarded.total_s() > plain.total_s());
}

TEST_CASE("Purge volume is reported for waste accounting", "[toolchange-cost]")
{
    CHECK(estimate_toolchange_cost(request(), contract(), thermal()).purge_volume_mm3 ==
          Catch::Approx(60.));
}

TEST_CASE("Missing rates degrade to zero rather than dividing by zero", "[toolchange-cost]")
{
    ToolchangeRequest value = request();
    value.travel_speed_mm_s = 0.;
    value.purge_flow_mm3_s  = 0.;

    ToolThermalState no_heater = thermal();
    no_heater.heat_rate_c_s = 0.;

    const ToolchangeCost cost = estimate_toolchange_cost(value, contract(), no_heater);
    CHECK(cost.travel_s == 0.);
    CHECK(cost.purge_s == 0.);
    CHECK(cost.heating_wait_s == 0.);
    // The contract-driven parts still apply.
    CHECK(cost.park_s == 2.);
}

TEST_CASE("The breakdown attributes time by cause", "[toolchange-cost]")
{
    ToolchangeRequest value = request();
    value.preheat_lead_s       = 0.;
    value.probe_after_pickup   = true;

    PrintTimeBreakdown breakdown;
    breakdown.extrusion_s = 1000.;
    breakdown.travel_s    = 100.;
    breakdown.add(estimate_toolchange_cost(value, contract(), thermal()));

    // Toolchange motion is kept apart from model travel.
    CHECK(breakdown.travel_s == 100.);
    CHECK(breakdown.toolchange_motion_s == Catch::Approx(7.));   // park + pickup + travel
    CHECK(breakdown.heating_wait_s == Catch::Approx(30.));
    CHECK(breakdown.wipe_prime_s == Catch::Approx(8.5));         // purge + brush + prime
    CHECK(breakdown.probe_s == 4.);
    CHECK(breakdown.total_s() == Catch::Approx(1149.5));
}

TEST_CASE("Several changes accumulate", "[toolchange-cost]")
{
    PrintTimeBreakdown breakdown;
    const ToolchangeCost cost = estimate_toolchange_cost(request(), contract(), thermal());
    breakdown.add(cost);
    breakdown.add(cost);

    CHECK(breakdown.toolchange_motion_s == Catch::Approx(2. * (2. + 3. + 2.)));
}
