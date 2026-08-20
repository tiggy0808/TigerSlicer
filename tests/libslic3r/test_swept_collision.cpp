// Phase 10: swept-volume collision simulation. The property that matters most
// is the third verdict: motion whose inputs were incomplete comes back
// Unproven, never Clear, and only a proven-clear result permits motion.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/SweptCollision.hpp"

using namespace Slic3r;

namespace {

// A toolhead whose nozzle tip is at the origin and whose body sits above and
// around it, as a real heater block and fan shroud do.
ToolheadShape toolhead(int tool = 0)
{
    ToolheadShape shape;
    shape.tool   = tool;
    shape.nozzle = Aabb::of(-0.5, -0.5, 0., 0.5, 0.5, 2.);
    shape.body   = Aabb::of(-15., -15., 2., 15., 15., 40.);
    return shape;
}

// Real fixtures stand up into the gantry, so these are tall enough to reach
// the toolhead body, which sits from Z+2 to Z+40 above the nozzle tip.
Obstacle obstacle(const std::string &name, ObstacleKind kind,
                  double x, double y, double z, double size = 20., double height = 60.)
{
    Obstacle value;
    value.name = name;
    value.kind = kind;
    value.box  = Aabb::of(x, y, z, x + size, y + size, z + height);
    return value;
}

// A 10 x 10 field of 1 mm cells with nothing printed on it.
PrintedHeightField flat_field(double height = 0.)
{
    PrintedHeightField field;
    field.origin_x = 0.;
    field.origin_y = 0.;
    field.cell_mm  = 1.;
    field.nx       = 10;
    field.ny       = 10;
    field.height_mm.assign(field.nx * field.ny, height);
    return field;
}

CollisionOptions options(double clearance = 0., double step = 1.)
{
    CollisionOptions value;
    value.clearance_mm   = clearance;
    value.sample_step_mm = step;
    return value;
}

MotionSegment travel(double x0, double y0, double z0, double x1, double y1, double z1)
{
    MotionSegment segment;
    segment.kind = MotionKind::Travel;
    segment.x0 = x0; segment.y0 = y0; segment.z0 = z0;
    segment.x1 = x1; segment.y1 = y1; segment.z1 = z1;
    return segment;
}

bool mentions(const CollisionReport &report, const std::string &fragment)
{
    return std::any_of(report.findings.begin(), report.findings.end(),
                       [&](const CollisionFinding &finding) {
                           return finding.reason.find(fragment) != std::string::npos ||
                                  finding.with.find(fragment) != std::string::npos;
                       });
}

} // namespace

TEST_CASE("A clear travel over empty bed is permitted", "[swept-collision]")
{
    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            {}, flat_field(), options());

    CHECK(report.verdict == CollisionVerdict::Clear);
    CHECK(report.motion_permitted());
    CHECK(report.findings.empty());
    CHECK(report.precise_checks > 0);
}

TEST_CASE("A travel through a fixture is a collision", "[swept-collision]")
{
    const std::vector<Obstacle> obstacles { obstacle("camera mount", ObstacleKind::Fixture,
                                                     140., 90., 0., 20.) };
    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            obstacles, flat_field(), options());

    CHECK(report.verdict == CollisionVerdict::Collision);
    CHECK_FALSE(report.motion_permitted());
    CHECK(mentions(report, "camera mount"));
    CHECK(mentions(report, "brings the toolhead into"));
}

TEST_CASE("The broad phase rejects obstacles the sweep cannot reach",
          "[swept-collision]")
{
    std::vector<Obstacle> obstacles;
    for (int index = 0; index < 20; ++index)
        obstacles.push_back(obstacle("far " + std::to_string(index), ObstacleKind::Fixture,
                                     1000. + index * 50., 1000., 0., 10.));

    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            obstacles, flat_field(), options());

    CHECK(report.verdict == CollisionVerdict::Clear);
    CHECK(report.broad_phase_rejections == 20);
}

TEST_CASE("A travel over a tall printed region collides with the body",
          "[swept-collision]")
{
    // A 25 mm tall tower, and a travel at Z 10: the nozzle clears it but the
    // body does not.
    PrintedHeightField field = flat_field();
    for (size_t index = 0; index < field.height_mm.size(); ++index)
        field.height_mm[index] = 25.;

    const CollisionReport report =
        simulate_collisions({ travel(5., 5., 10., 8., 5., 10.) }, { toolhead() }, {},
                            field, options());

    CHECK(report.verdict == CollisionVerdict::Collision);
    CHECK(mentions(report, "printed geometry"));
    CHECK(mentions(report, "standing 25.00 mm tall"));
}

TEST_CASE("A print move at the same height is not a collision with itself",
          "[swept-collision]")
{
    // Printing is the nozzle touching printed geometry. A print move whose
    // body clears the part is legitimate.
    PrintedHeightField field = flat_field(0.2);

    MotionSegment print = travel(5., 5., 0.2, 8., 5., 0.2);
    print.kind = MotionKind::Print;

    const CollisionReport report =
        simulate_collisions({ print }, { toolhead() }, {}, field, options());
    CHECK(report.verdict == CollisionVerdict::Clear);
}

TEST_CASE("A downward move into printed geometry is caught", "[swept-collision]")
{
    PrintedHeightField field = flat_field(5.);

    MotionSegment down = travel(5., 5., 20., 5., 5., 1.);
    down.kind = MotionKind::DownwardZ;

    const CollisionReport report =
        simulate_collisions({ down }, { toolhead() }, {}, field, options());

    CHECK(report.verdict == CollisionVerdict::Collision);
    CHECK(mentions(report, "downward move"));
}

TEST_CASE("A tool may touch its own dock, but only while docking",
          "[swept-collision]")
{
    Obstacle dock = obstacle("dock 0", ObstacleKind::Dock, 95., 95., 20., 20.);
    dock.owning_tool = 0;

    SECTION("approaching its own dock is legitimate") {
        MotionSegment approach = travel(100., 100., 30., 105., 100., 30.);
        approach.kind = MotionKind::DockApproach;
        const CollisionReport report =
            simulate_collisions({ approach }, { toolhead() }, { dock }, flat_field(),
                                options());
        CHECK(report.verdict == CollisionVerdict::Clear);
    }

    SECTION("travelling through it is not") {
        const CollisionReport report =
            simulate_collisions({ travel(100., 100., 30., 105., 100., 30.) }, { toolhead() },
                                { dock }, flat_field(), options());
        CHECK(report.verdict == CollisionVerdict::Collision);
    }

    SECTION("another tool may never touch it, even while docking") {
        MotionSegment approach = travel(100., 100., 30., 105., 100., 30.);
        approach.kind = MotionKind::DockApproach;
        approach.tool = 1;
        const CollisionReport report =
            simulate_collisions({ approach }, { toolhead(1) }, { dock }, flat_field(),
                                options());
        CHECK(report.verdict == CollisionVerdict::Collision);
    }
}

TEST_CASE("An inactive parked tool is an obstacle like any other",
          "[swept-collision]")
{
    Obstacle parked = obstacle("T2 parked", ObstacleKind::ParkedTool, 140., 90., 20., 20.);
    parked.owning_tool = 2;

    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            { parked }, flat_field(), options());
    CHECK(report.verdict == CollisionVerdict::Collision);
    CHECK(mentions(report, "T2 parked"));
}

TEST_CASE("Motion with no described toolhead is unproven, never clear",
          "[swept-collision]")
{
    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, {}, {},
                            flat_field(), options());

    CHECK(report.verdict == CollisionVerdict::Unproven);
    CHECK_FALSE(report.motion_permitted());
    CHECK(mentions(report, "no toolhead shape is described"));
}

TEST_CASE("An incomplete toolhead shape is unproven, never clear",
          "[swept-collision]")
{
    ToolheadShape partial;
    partial.tool   = 0;
    partial.nozzle = Aabb::of(-0.5, -0.5, 0., 0.5, 0.5, 2.);
    // The body was never described.

    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { partial }, {},
                            flat_field(), options());
    CHECK(report.verdict == CollisionVerdict::Unproven);
    CHECK(mentions(report, "incomplete"));
}

TEST_CASE("An undetermined printed column cannot be cleared", "[swept-collision]")
{
    PrintedHeightField field = flat_field();
    field.height_mm[5 * field.nx + 5].reset();   // one column never determined

    const CollisionReport report =
        simulate_collisions({ travel(5., 5., 30., 6., 5., 30.) }, { toolhead() }, {},
                            field, options());

    CHECK(report.verdict == CollisionVerdict::Unproven);
    CHECK_FALSE(report.motion_permitted());
    CHECK(mentions(report, "never determined"));
}

TEST_CASE("An obstacle with no described volume is unproven", "[swept-collision]")
{
    Obstacle shapeless;
    shapeless.name = "unmodelled bracket";
    shapeless.kind = ObstacleKind::Fixture;
    // box left invalid on purpose

    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            { shapeless }, flat_field(), options());

    CHECK(report.verdict == CollisionVerdict::Unproven);
    CHECK(mentions(report, "no described volume"));
}

TEST_CASE("A collision outranks an unproven motion in the overall verdict",
          "[swept-collision]")
{
    Obstacle shapeless;
    shapeless.name = "unmodelled bracket";

    const std::vector<Obstacle> obstacles {
        shapeless, obstacle("camera mount", ObstacleKind::Fixture, 140., 90., 0., 20.) };

    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            obstacles, flat_field(), options());
    CHECK(report.verdict == CollisionVerdict::Collision);
}

TEST_CASE("Required clearance is enforced, not just contact", "[swept-collision]")
{
    // The fixture sits 3 mm clear of the swept body.
    const std::vector<Obstacle> obstacles { obstacle("post", ObstacleKind::Fixture,
                                                      118., 95., 0., 10.) };

    SECTION("touching is not required to fail when clearance is demanded") {
        const CollisionReport strict =
            simulate_collisions({ travel(100., 100., 30., 100., 100., 30.) }, { toolhead() },
                                obstacles, flat_field(), options(6.));
        CHECK(strict.verdict == CollisionVerdict::Collision);
    }

    SECTION("with no clearance requirement the same motion is clear") {
        const CollisionReport lenient =
            simulate_collisions({ travel(100., 100., 30., 100., 100., 30.) }, { toolhead() },
                                obstacles, flat_field(), options(0.));
        CHECK(lenient.verdict == CollisionVerdict::Clear);
    }
}

TEST_CASE("Independent objects at different heights are each checked",
          "[swept-collision]")
{
    // Two towers; the travel clears the short one and hits the tall one.
    PrintedHeightField field = flat_field(0.);
    for (size_t y = 0; y < field.ny; ++y)
        for (size_t x = 0; x < field.nx; ++x)
            field.height_mm[y * field.nx + x] = x < 5 ? 2. : 30.;

    const CollisionReport report =
        simulate_collisions({ travel(1., 5., 20., 9., 5., 20.) }, { toolhead() }, {},
                            field, options());
    CHECK(report.verdict == CollisionVerdict::Collision);
    CHECK(mentions(report, "30.00 mm tall"));
}

TEST_CASE("A truncated report says it is truncated", "[swept-collision]")
{
    std::vector<MotionSegment> segments;
    for (int index = 0; index < 20; ++index)
        segments.push_back(travel(100., 100., 30., 105., 100., 30.));

    const std::vector<Obstacle> obstacles { obstacle("post", ObstacleKind::Fixture,
                                                      95., 95., 0., 20.) };
    CollisionOptions capped = options();
    capped.max_findings = 3;

    const CollisionReport report =
        simulate_collisions(segments, { toolhead() }, obstacles, flat_field(), capped);

    CHECK(report.verdict == CollisionVerdict::Collision);
    CHECK(mentions(report, "this report is truncated"));
}

TEST_CASE("Malformed simulation inputs are refused", "[swept-collision]")
{
    SECTION("no motion") {
        const CollisionReport report =
            simulate_collisions({}, { toolhead() }, {}, flat_field(), options());
        CHECK(report.error.find("no motion") != std::string::npos);
        CHECK_FALSE(report.motion_permitted());
    }

    SECTION("a non-positive sampling step") {
        const CollisionReport report =
            simulate_collisions({ travel(0., 0., 0., 1., 0., 0.) }, { toolhead() }, {},
                                flat_field(), options(0., 0.));
        CHECK(report.error.find("sampling step") != std::string::npos);
    }
}

TEST_CASE("An empty height field does not fabricate printed geometry",
          "[swept-collision]")
{
    PrintedHeightField nothing;   // nx and ny are zero

    const CollisionReport report =
        simulate_collisions({ travel(100., 100., 30., 200., 100., 30.) }, { toolhead() },
                            {}, nothing, options());
    CHECK(report.verdict == CollisionVerdict::Clear);
}
