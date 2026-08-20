// Phase 23: curvature-aware adaptive layers, planned inside each tool's own
// grid. The properties held here are that a shallow or curved visible surface
// gets finer layers than a vertical or hidden one, that a band whose slope was
// never measured gets the finest layer and does not claim a surface error at
// all, that a boundary is never moved to make the arithmetic work - a sliver
// is folded into the layer below it or the schedule is refused - and that both
// the "not worth refining" and "too complex to be worth it" decisions are
// stated rather than made quietly.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/AdaptiveLayers.hpp"

using namespace Slic3r;

namespace {

SurfaceBand band(double bottom, double top, std::optional<double> slope,
                 bool visible = true, double curvature = 0.)
{
    SurfaceBand value;
    value.z_bottom_mm = bottom;
    value.z_top_mm    = top;
    value.slope_deg   = slope;
    value.visible     = visible;
    value.curvature   = curvature;
    return value;
}

ToolLayerGrid grid(int tool = 0, double max_height = 0.30,
                   std::optional<double> change_cost = 20.)
{
    ToolLayerGrid value;
    value.tool                = tool;
    value.nozzle_mm           = 0.4;
    value.min_layer_height_mm = 0.06;
    value.max_layer_height_mm = max_height;
    value.change_cost_s       = change_cost;
    return value;
}

LayerConstraint constraint(double z, LayerConstraintKind kind,
                           std::optional<int> tool = std::nullopt)
{
    LayerConstraint value;
    value.z_mm = z;
    value.kind = kind;
    value.tool = tool;
    return value;
}

double first_height(const AdaptiveLayerPlan &plan)
{
    return plan.schedules.front().layers.front().height_mm;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

bool lands_on(const ToolSchedule &schedule, double z)
{
    return std::any_of(schedule.layers.begin(), schedule.layers.end(),
                       [&](const PlannedLayer &layer) {
                           return std::abs(layer.z_top_mm - z) < 1e-6 && layer.at_constraint;
                       });
}

} // namespace

TEST_CASE("The surface error of an unmeasured slope is not claimed",
          "[adaptive-layers]")
{
    CHECK_FALSE(estimate_surface_error(band(0., 1., std::nullopt), 0.2).has_value());

    // A vertical wall does not stair-step; a shallow slope steps the full
    // layer height.
    REQUIRE(estimate_surface_error(band(0., 1., 90.), 0.2).has_value());
    CHECK(*estimate_surface_error(band(0., 1., 90.), 0.2) == Catch::Approx(0.).margin(1e-9));
    CHECK(*estimate_surface_error(band(0., 1., 0.), 0.2) == Catch::Approx(0.2));
}

TEST_CASE("A shallow visible slope gets finer layers than a vertical wall",
          "[adaptive-layers]")
{
    const AdaptiveLayerPlan shallow =
        plan_adaptive_layers({ band(0., 1., 10.) }, { grid() }, {}, AdaptiveLayerOptions {});
    const AdaptiveLayerPlan vertical =
        plan_adaptive_layers({ band(0., 1., 90.) }, { grid() }, {}, AdaptiveLayerOptions {});

    CHECK(first_height(shallow) < first_height(vertical));
    CHECK(first_height(vertical) == Catch::Approx(0.30));
    CHECK(first_height(shallow) == Catch::Approx(0.06));
}

TEST_CASE("Curvature tightens the layer height on the same slope",
          "[adaptive-layers]")
{
    const AdaptiveLayerPlan flat = plan_adaptive_layers(
        { band(0., 1., 45., true, 0.) }, { grid() }, {}, AdaptiveLayerOptions {});
    const AdaptiveLayerPlan curved = plan_adaptive_layers(
        { band(0., 1., 45., true, 1.0) }, { grid() }, {}, AdaptiveLayerOptions {});

    CHECK(first_height(curved) < first_height(flat));
}

TEST_CASE("A hidden region is printed at the thickest layer the tool allows",
          "[adaptive-layers]")
{
    const AdaptiveLayerPlan plan = plan_adaptive_layers(
        { band(0., 1., 10., false) }, { grid() }, {}, AdaptiveLayerOptions {});

    CHECK(first_height(plan) == Catch::Approx(0.30));
    CHECK(mentions(plan.schedules.front().notes, "is not visible"));
}

TEST_CASE("A band whose slope was never measured is unproven and gets the finest layer",
          "[adaptive-layers]")
{
    const AdaptiveLayerPlan plan = plan_adaptive_layers(
        { band(0., 1., std::nullopt) }, { grid() }, {}, AdaptiveLayerOptions {});

    const ToolSchedule &schedule = plan.schedules.front();
    CHECK(schedule.verdict == LayerVerdict::Unproven);
    CHECK(schedule.layers.front().height_mm == Catch::Approx(0.06));
    CHECK_FALSE(schedule.layers.front().estimated_surface_error_mm.has_value());
    CHECK(mentions(schedule.notes, "was never measured"));
}

TEST_CASE("Refining for a gain below the minimum benefit is declined out loud",
          "[adaptive-layers]")
{
    // At 80 degrees the tolerance permits 0.288 mm, which is barely finer than
    // the 0.30 mm the tool would use anyway.
    const AdaptiveLayerPlan plan = plan_adaptive_layers(
        { band(0., 1., 80.) }, { grid() }, {}, AdaptiveLayerOptions {});

    CHECK(first_height(plan) == Catch::Approx(0.30));
    CHECK(mentions(plan.schedules.front().notes, "less than the"));
    CHECK(mentions(plan.schedules.front().notes, "left coarse"));
}

TEST_CASE("Detail survives the minimum-benefit test", "[adaptive-layers]")
{
    // A vertical wall does not step at all, so refining it buys nothing - but
    // engraved detail on it still needs a layer it can resolve.
    SurfaceBand engraved   = band(0., 1., 90.);
    engraved.carries_detail = true;

    const AdaptiveLayerPlan plan =
        plan_adaptive_layers({ engraved }, { grid() }, {}, AdaptiveLayerOptions {});
    CHECK(first_height(plan) == Catch::Approx(0.20));
}

TEST_CASE("Layers land exactly on a top surface or colour boundary",
          "[adaptive-layers]")
{
    const AdaptiveLayerPlan plan = plan_adaptive_layers(
        { band(0., 1., 90.) }, { grid() },
        { constraint(0.5, LayerConstraintKind::TopSurface),
          constraint(0.75, LayerConstraintKind::ColorBoundary) },
        AdaptiveLayerOptions {});

    const ToolSchedule &schedule = plan.schedules.front();
    CHECK(schedule.verdict == LayerVerdict::Planned);
    CHECK(lands_on(schedule, 0.5));
    CHECK(lands_on(schedule, 0.75));
}

TEST_CASE("A sliver is folded into the layer below, never moved off the boundary",
          "[adaptive-layers]")
{
    const std::vector<LayerConstraint> boundaries {
        constraint(0.30, LayerConstraintKind::TopSurface),
        constraint(0.31, LayerConstraintKind::ColorBoundary),
    };

    SECTION("folded in where the layer below has room") {
        const AdaptiveLayerPlan plan = plan_adaptive_layers(
            { band(0., 1., 90.) }, { grid(0, 0.35) }, boundaries, AdaptiveLayerOptions {});

        const ToolSchedule &schedule = plan.schedules.front();
        CHECK(schedule.verdict == LayerVerdict::Planned);
        CHECK(lands_on(schedule, 0.31));
        CHECK(mentions({ schedule.layers.front().reason }, "sliver"));
        CHECK(schedule.layers.front().height_mm == Catch::Approx(0.31));
    }

    SECTION("refused where it has not, rather than moving the boundary") {
        const AdaptiveLayerPlan plan = plan_adaptive_layers(
            { band(0., 1., 90.) }, { grid(0, 0.30) }, boundaries, AdaptiveLayerOptions {});

        const ToolSchedule &schedule = plan.schedules.front();
        CHECK(schedule.verdict == LayerVerdict::Refused);
        CHECK(mentions(schedule.notes, "boundary is not moved"));
    }
}

TEST_CASE("A collision limit stops a tool's schedule and says so",
          "[adaptive-layers]")
{
    const AdaptiveLayerPlan plan = plan_adaptive_layers(
        { band(0., 1., 90.) }, { grid() },
        { constraint(0.6, LayerConstraintKind::Collision, 0) }, AdaptiveLayerOptions {});

    const ToolSchedule &schedule = plan.schedules.front();
    CHECK(schedule.layers.back().z_top_mm <= 0.6 + 1e-9);
    CHECK(mentions(schedule.notes, "must not print above"));
}

TEST_CASE("Synchronization points are reported, and priced only when every cost is known",
          "[adaptive-layers]")
{
    const std::vector<LayerConstraint> meet {
        constraint(0.5, LayerConstraintKind::Synchronization)
    };

    SECTION("priced when both tools have a recorded change cost") {
        const AdaptiveLayerPlan plan =
            plan_adaptive_layers({ band(0., 1., 90.) }, { grid(0), grid(1, 0.30, 25.) }, meet,
                                 AdaptiveLayerOptions {});

        REQUIRE(plan.synchronization_points.size() == 1);
        CHECK(plan.synchronization_points.front() == Catch::Approx(0.5));
        REQUIRE(plan.synchronization_cost_s.has_value());
        CHECK(*plan.synchronization_cost_s == Catch::Approx(25.));
        // Both tools have to land there.
        CHECK(lands_on(plan.schedules[0], 0.5));
        CHECK(lands_on(plan.schedules[1], 0.5));
    }

    SECTION("withheld when a change cost was never recorded") {
        const AdaptiveLayerPlan plan = plan_adaptive_layers(
            { band(0., 1., 90.) }, { grid(0), grid(1, 0.30, std::nullopt) }, meet,
            AdaptiveLayerOptions {});

        CHECK_FALSE(plan.synchronization_cost_s.has_value());
        CHECK(mentions(plan.notes, "no recorded change cost"));
    }
}

TEST_CASE("A schedule too complex for the caller is replanned uniform and says what was lost",
          "[adaptive-layers]")
{
    const std::vector<SurfaceBand> varied {
        band(0.0, 0.3, 90.), band(0.3, 0.6, 10.), band(0.6, 0.9, 45.), band(0.9, 1.2, 90.),
    };

    AdaptiveLayerOptions options;
    options.max_height_changes = 1;

    const AdaptiveLayerPlan plan =
        plan_adaptive_layers(varied, { grid() }, {}, options);

    const ToolSchedule &schedule = plan.schedules.front();
    CHECK(schedule.verdict == LayerVerdict::Coarsened);
    CHECK(schedule.height_changes == 0);
    CHECK(mentions(schedule.notes, "replanned at a uniform"));
    CHECK(mentions(schedule.notes, "finer detail that would have bought is gone"));
}

TEST_CASE("A schedule within the complexity limit keeps its detail",
          "[adaptive-layers]")
{
    const std::vector<SurfaceBand> varied {
        band(0.0, 0.3, 90.), band(0.3, 0.6, 10.),
    };

    const AdaptiveLayerPlan plan =
        plan_adaptive_layers(varied, { grid() }, {}, AdaptiveLayerOptions {});

    CHECK(plan.schedules.front().verdict == LayerVerdict::Planned);
    CHECK(plan.schedules.front().height_changes > 0);
}

TEST_CASE("A capped schedule says it stopped rather than looking complete",
          "[adaptive-layers]")
{
    AdaptiveLayerOptions options;
    options.max_layers_per_tool = 3;

    const AdaptiveLayerPlan plan =
        plan_adaptive_layers({ band(0., 10., 90.) }, { grid() }, {}, options);

    CHECK(plan.schedules.front().capped);
    CHECK(plan.schedules.front().layers.size() == 3);
    CHECK(mentions(plan.schedules.front().notes, "was not laid out"));
}

TEST_CASE("The micro-layer floor overrides a tool that claims to go thinner",
          "[adaptive-layers]")
{
    ToolLayerGrid fine     = grid();
    fine.min_layer_height_mm = 0.02;

    AdaptiveLayerOptions options;
    options.micro_layer_floor_mm = 0.08;

    const AdaptiveLayerPlan plan =
        plan_adaptive_layers({ band(0., 1., 5.) }, { fine }, {}, options);

    CHECK(first_height(plan) == Catch::Approx(0.08));
    CHECK(mentions(plan.schedules.front().notes, "micro-layer floor"));
}

TEST_CASE("An unusable tool grid is refused rather than worked around",
          "[adaptive-layers]")
{
    SECTION("a tool that is not available") {
        ToolLayerGrid missing = grid();
        missing.available     = false;

        const AdaptiveLayerPlan plan = plan_adaptive_layers(
            { band(0., 1., 90.) }, { missing }, {}, AdaptiveLayerOptions {});
        CHECK(plan.schedules.front().verdict == LayerVerdict::Refused);
        CHECK(mentions(plan.schedules.front().notes, "is not available"));
    }

    SECTION("a maximum below the minimum") {
        ToolLayerGrid inverted        = grid();
        inverted.max_layer_height_mm  = 0.03;

        const AdaptiveLayerPlan plan = plan_adaptive_layers(
            { band(0., 1., 90.) }, { inverted }, {}, AdaptiveLayerOptions {});
        CHECK(plan.schedules.front().verdict == LayerVerdict::Refused);
        CHECK(mentions(plan.schedules.front().notes, "not a grid it can print on"));
    }
}

TEST_CASE("A model that is not fully described produces an error, not a guess",
          "[adaptive-layers]")
{
    CHECK_FALSE(
        plan_adaptive_layers({}, { grid() }, {}, AdaptiveLayerOptions {}).error.empty());
    CHECK_FALSE(plan_adaptive_layers({ band(0., 1., 90.) }, {}, {}, AdaptiveLayerOptions {})
                    .error.empty());

    // A gap between two bands is a piece of the model nobody described.
    const AdaptiveLayerPlan gapped = plan_adaptive_layers(
        { band(0., 0.4, 90.), band(0.6, 1.0, 90.) }, { grid() }, {},
        AdaptiveLayerOptions {});
    CHECK_FALSE(gapped.error.empty());
    CHECK(gapped.error.find("undescribed") != std::string::npos);
}
