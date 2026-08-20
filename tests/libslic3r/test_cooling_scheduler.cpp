// Phase 18: thermal and cooling-aware scheduling. The model is a bounded
// approximation and the tests hold it to that: every estimate carries the
// disclaimer, the clamp is observable, and confidence follows what was known
// rather than how confident the arithmetic looks. The remedies preserve
// adhesion and dependencies absolutely - no extra fan or lower temperature on
// the first layer, no slowing a bridge, no reorder across a dependency - and
// when nothing is available the operation is reported unresolved rather than
// quietly left hot.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/CoolingScheduler.hpp"

using namespace Slic3r;

namespace {

MaterialThermalModel pla()
{
    MaterialThermalModel material;
    material.material       = "PLA";
    material.softening_c    = 60.;
    material.cooling_factor = 1.;
    return material;
}

CoolingOperation op(size_t index, double layer_time_s)
{
    CoolingOperation value;
    value.index                   = index;
    value.tool                    = 0;
    value.material                = "PLA";
    value.print_z                 = 0.2 * double(index + 1);
    value.extrusion_temperature_c = 210.;
    value.deposited_volume_mm3    = 30.;
    value.wall_thickness_mm       = 0.8;
    value.layer_time_s            = layer_time_s;
    value.fan_fraction            = 0.5;
    return value;
}

EnvironmentModel room()
{
    EnvironmentModel environment;
    environment.ambient_temperature_c = 25.;
    environment.bed_temperature_c     = 60.;
    return environment;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

const CoolingAction *action_for(const CoolingPlan &plan, size_t operation)
{
    for (const CoolingAction &action : plan.actions)
        if (action.operation == operation)
            return &action;
    return nullptr;
}

} // namespace

TEST_CASE("Every estimate says it is an approximation", "[cooling-scheduler]")
{
    const std::vector<HeatEstimate> estimates =
        estimate_heat_accumulation({ op(0, 30.) }, { pla() }, room(), HeatModelBounds {});

    REQUIRE(estimates.size() == 1);
    // The disclaimer travels with the number so no caller can drop it.
    CHECK(estimates.front().basis.front() == std::string(heat_model_disclaimer()));
    CHECK(std::string(heat_model_disclaimer()).find("not a fluid simulation") !=
          std::string::npos);
}

TEST_CASE("A short layer leaves the part hotter than a long one",
          "[cooling-scheduler]")
{
    const std::vector<HeatEstimate> quick =
        estimate_heat_accumulation({ op(0, 2.) }, { pla() }, room(), HeatModelBounds {});
    const std::vector<HeatEstimate> slow =
        estimate_heat_accumulation({ op(0, 120.) }, { pla() }, room(), HeatModelBounds {});

    CHECK(quick.front().estimated_temperature_c > slow.front().estimated_temperature_c);
}

TEST_CASE("The estimate is clamped and says when the clamp bit",
          "[cooling-scheduler]")
{
    HeatModelBounds tight;
    tight.max_delta_c = 2.;

    // No cooling time, no fan, a bridge with nothing beneath it: the raw sum
    // runs past the bound, and the bound is what is reported.
    CoolingOperation hot = op(0, 0.);
    hot.fan_fraction     = 0.;
    hot.is_bridge        = true;

    const std::vector<HeatEstimate> estimates =
        estimate_heat_accumulation({ hot }, { pla() }, room(), tight);

    REQUIRE(estimates.size() == 1);
    CHECK(estimates.front().clamped);
    CHECK(estimates.front().estimated_temperature_c == Catch::Approx(212.));
    CHECK(mentions(estimates.front().basis, "clamped to the model's"));
}

TEST_CASE("A thoroughly cooled part is not pushed back up by the bound",
          "[cooling-scheduler]")
{
    // The downward limit is ambient, not `extrusion - max_delta`; the latter
    // would report a cold part as hot.
    const std::vector<HeatEstimate> estimates =
        estimate_heat_accumulation({ op(0, 600.) }, { pla() }, room(), HeatModelBounds {});
    CHECK(estimates.front().estimated_temperature_c < 60.);
}

TEST_CASE("Confidence follows what was recorded, not the arithmetic",
          "[cooling-scheduler]")
{
    SECTION("no softening temperature is insufficient, whatever else is known") {
        MaterialThermalModel bare;
        bare.material       = "PLA";
        bare.cooling_factor = 1.;
        const std::vector<HeatEstimate> estimates =
            estimate_heat_accumulation({ op(0, 30.) }, { bare }, room(), HeatModelBounds {});
        CHECK(estimates.front().confidence == Confidence::Insufficient);
        CHECK(mentions(estimates.front().gaps, "nothing to compare this estimate against"));
    }

    SECTION("a bare environment lowers confidence") {
        MaterialThermalModel partial;
        partial.material    = "PLA";
        partial.softening_c = 60.;
        CoolingOperation thin = op(0, 30.);
        thin.wall_thickness_mm = 0.;

        const std::vector<HeatEstimate> estimates = estimate_heat_accumulation(
            { thin }, { partial }, EnvironmentModel {}, HeatModelBounds {});
        CHECK(estimates.front().confidence == Confidence::Low);
        CHECK(estimates.front().gaps.size() >= 3);
    }

    SECTION("everything recorded reaches high confidence") {
        const std::vector<HeatEstimate> estimates =
            estimate_heat_accumulation({ op(0, 30.) }, { pla() }, room(), HeatModelBounds {});
        CHECK(estimates.front().confidence == Confidence::High);
        CHECK(estimates.front().gaps.empty());
    }
}

TEST_CASE("Recent nearby work makes an operation hotter", "[cooling-scheduler]")
{
    std::vector<CoolingOperation> together { op(0, 5.), op(1, 5.) };
    together[1].print_z = together[0].print_z;

    std::vector<CoolingOperation> apart = together;
    apart[1].x                          = 200.;

    const double crowded = estimate_heat_accumulation(together, { pla() }, room(),
                                                      HeatModelBounds {})[1]
                               .estimated_temperature_c;
    const double lonely =
        estimate_heat_accumulation(apart, { pla() }, room(), HeatModelBounds {})[1]
            .estimated_temperature_c;

    CHECK(crowded > lonely);
}

TEST_CASE("An unrecorded environment is not a cold one", "[cooling-scheduler]")
{
    const std::vector<HeatEstimate> estimates = estimate_heat_accumulation(
        { op(0, 30.) }, { pla() }, EnvironmentModel {}, HeatModelBounds {});
    CHECK(mentions(estimates.front().gaps, "neither chamber nor ambient"));
}

TEST_CASE("An operation with no softening temperature gets no cooling decision",
          "[cooling-scheduler]")
{
    MaterialThermalModel bare;
    bare.material = "PLA";

    const CoolingPlan plan = plan_cooling({ op(0, 2.) }, { bare }, room(),
                                          HeatModelBounds {}, CoolingPolicy {});
    CHECK(plan.actions.empty());
    CHECK(mentions(plan.notes, "no cooling decision is made for it"));
}

TEST_CASE("More fan is the first remedy on ordinary work", "[cooling-scheduler]")
{
    std::vector<CoolingOperation> operations { op(0, 1.) };
    operations[0].fan_fraction = 0.2;

    const CoolingPlan plan =
        plan_cooling(operations, { pla() }, room(), HeatModelBounds {}, CoolingPolicy {});

    const CoolingAction *action = action_for(plan, 0);
    REQUIRE(action != nullptr);
    CHECK(action->remedy == CoolingRemedy::IncreaseFan);
    CHECK(action->value == Catch::Approx(0.45));
    // A remedy always explains both the problem and the choice.
    CHECK(action->explanation.size() >= 2);
    CHECK(mentions(action->explanation, "above where PLA softens"));
}

TEST_CASE("The first layer is never given more fan or a lower temperature",
          "[cooling-scheduler]")
{
    std::vector<CoolingOperation> operations { op(0, 1.) };
    operations[0].is_first_layer = true;
    operations[0].fan_fraction   = 0.;
    // Give it a dependency so reordering is unavailable too.
    operations[0].depends_on = { 99 };

    const CoolingPlan plan =
        plan_cooling(operations, { pla() }, room(), HeatModelBounds {}, CoolingPolicy {});

    const CoolingAction *action = action_for(plan, 0);
    REQUIRE(action != nullptr);
    // Slowing down is allowed on the first layer; fan and temperature are not.
    CHECK(action->remedy == CoolingRemedy::ReduceSpeed);
}

TEST_CASE("A bridge is never slowed and never has its fan taken away",
          "[cooling-scheduler]")
{
    std::vector<CoolingOperation> operations { op(0, 1.), op(1, 600.) };
    operations[0].is_bridge    = true;
    operations[0].fan_fraction = 1.;
    // Something is waiting on the bridge, so it cannot be deferred either;
    // that leaves slowing it or dropping its temperature, and slowing a bridge
    // lets it sag.
    operations[1].depends_on = { 0 };

    const CoolingPlan plan =
        plan_cooling(operations, { pla() }, room(), HeatModelBounds {}, CoolingPolicy {});

    const CoolingAction *action = action_for(plan, 0);
    REQUIRE(action != nullptr);
    CHECK(action->remedy == CoolingRemedy::LowerTemperature);
}

TEST_CASE("A reorder is only proposed where no dependency forbids it",
          "[cooling-scheduler]")
{
    std::vector<CoolingOperation> operations { op(0, 1.), op(1, 1.) };
    operations[0].fan_fraction = 1.;
    operations[1].fan_fraction = 1.;
    // Operation 1 depends on operation 0, so 0 cannot move.
    operations[1].depends_on = { 0 };

    const CoolingPlan plan =
        plan_cooling(operations, { pla() }, room(), HeatModelBounds {}, CoolingPolicy {});

    const CoolingAction *pinned = action_for(plan, 0);
    REQUIRE(pinned != nullptr);
    CHECK(pinned->remedy != CoolingRemedy::Reorder);

    // Operation 1 has a dependency of its own, but nothing waits on it, so it
    // can still be deferred - which is the only direction that helps.
    const CoolingAction *free_to_move = action_for(plan, 1);
    REQUIRE(free_to_move != nullptr);
    CHECK(free_to_move->remedy == CoolingRemedy::Reorder);
    CHECK(mentions(free_to_move->explanation, "costs no dependency"));
}

TEST_CASE("A weak estimate gets only the remedy that cannot make things worse",
          "[cooling-scheduler]")
{
    MaterialThermalModel partial;
    partial.material    = "PLA";
    partial.softening_c = 60.;

    std::vector<CoolingOperation> operations { op(0, 1.) };
    operations[0].wall_thickness_mm = 0.;
    operations[0].fan_fraction      = 0.;

    const CoolingPlan plan = plan_cooling(operations, { partial }, EnvironmentModel {},
                                          HeatModelBounds {}, CoolingPolicy {});

    const CoolingAction *action = action_for(plan, 0);
    REQUIRE(action != nullptr);
    CHECK(action->remedy == CoolingRemedy::Delay);
    CHECK(mentions(action->explanation, "cannot degrade the part"));
}

TEST_CASE("An operation nothing can be done for is reported, not left silent",
          "[cooling-scheduler]")
{
    CoolingPolicy strict;
    strict.max_speed_reduction   = 0.;
    strict.max_temperature_drop_c = 0.;

    std::vector<CoolingOperation> operations { op(0, 1.) };
    operations[0].is_first_layer = true;
    operations[0].fan_fraction   = 1.;
    operations[0].depends_on     = { 99 };

    const CoolingPlan plan =
        plan_cooling(operations, { pla() }, room(), HeatModelBounds {}, strict);

    CHECK(plan.actions.empty());
    REQUIRE(plan.unresolved.size() == 1);
    CHECK(mentions(plan.unresolved, "nothing can be done about it"));
    CHECK(mentions(plan.unresolved, "first-layer adhesion"));
}

TEST_CASE("A part that never gets too hot needs no remedy", "[cooling-scheduler]")
{
    const CoolingPlan plan = plan_cooling({ op(0, 600.) }, { pla() }, room(),
                                          HeatModelBounds {}, CoolingPolicy {});
    CHECK(plan.actions.empty());
    CHECK(plan.unresolved.empty());
    CHECK(mentions(plan.notes, "bounded approximation"));
}

TEST_CASE("An empty schedule is an error, not an empty plan", "[cooling-scheduler]")
{
    const CoolingPlan plan =
        plan_cooling({}, { pla() }, room(), HeatModelBounds {}, CoolingPolicy {});
    CHECK_FALSE(plan.error.empty());
}
