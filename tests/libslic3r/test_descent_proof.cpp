// Phase 25C: the downward-Z and clearance proof that stands behind every
// descent Phase 25B schedules. The rules under test are the ones that make a
// descent safe rather than plausible: it happens inside a corridor verified
// empty and finishes before the toolhead is ever over the lagging component;
// the whole toolhead is swept, because clearing the nozzle point says nothing
// about the carriage; calibration uncertainty widens the clearance and an
// uncharacterized uncertainty proves nothing at all; and a corridor whose
// printed height was never determined is unknown, not clear.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/DescentProof.hpp"

using namespace Slic3r;

namespace {

// A bed height field covering x -50..350 and y -50..150 in 10 mm cells, with
// a tower printed to 2 mm at the origin end and one printed to 1 mm further
// along. Everything else is bare bed.
PrintedHeightField bed()
{
    PrintedHeightField field;
    field.origin_x = -50.;
    field.origin_y = -50.;
    field.cell_mm  = 10.;
    field.nx       = 40;
    field.ny       = 20;
    field.height_mm.assign(field.nx * field.ny, std::optional<double>(0.));

    auto set = [&](size_t ix, size_t iy, std::optional<double> height) {
        field.height_mm[iy * field.nx + ix] = height;
    };
    for (size_t ix : { size_t(5), size_t(6) })
        for (size_t iy : { size_t(5), size_t(6) })
            set(ix, iy, 2.0); // the leading tower, printed ahead
    for (size_t ix : { size_t(15), size_t(16) })
        for (size_t iy : { size_t(5), size_t(6) })
            set(ix, iy, 1.0); // the lagging tower
    return field;
}

ToolheadShape toolhead()
{
    ToolheadShape shape;
    shape.tool   = 0;
    shape.nozzle = Aabb::of(-0.5, -0.5, 0., 0.5, 0.5, 1.);
    shape.body   = Aabb::of(-12., -12., 1., 12., 12., 60.);
    return shape;
}

DescentCorridor corridor(const std::string &name, double x, double y,
                         double half_width = 15.)
{
    DescentCorridor value;
    value.name          = name;
    value.center_x      = x;
    value.center_y      = y;
    value.half_width_mm = half_width;
    return value;
}

DescentContext context()
{
    DescentContext value;
    value.shapes                     = { toolhead() };
    value.printed                    = bed();
    value.corridors                  = { corridor("the gap between the towers", 60., 10.) };
    value.calibration_uncertainty_mm = 0.2;
    value.clearance_mm               = 1.0;
    value.sample_step_mm             = 2.0;
    return value;
}

// Coming off the leading tower at 2 mm, back down to the lagging one at 1 mm.
DescentRequest descent()
{
    DescentRequest value;
    value.tool   = 0;
    value.from_x = 10.;
    value.from_y = 10.;
    value.from_z = 2.0;
    value.to_x   = 110.;
    value.to_y   = 10.;
    value.to_z   = 1.0;
    return value;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("A descent through a verified empty corridor is proven clear",
          "[descent-proof]")
{
    const DescentPlan plan = prove_descent(descent(), context());

    CHECK(plan.verdict == DescentVerdict::Clear);
    REQUIRE(plan.corridor.has_value());
    CHECK(*plan.corridor == "the gap between the towers");

    // Across at height, down inside the corridor, then in at low z. The order
    // is the point: the descent finishes before the toolhead is over the
    // lagging component.
    REQUIRE(plan.segments.size() == 3);
    CHECK(plan.segments[0].kind == MotionKind::Travel);
    CHECK(plan.segments[0].z0 == plan.segments[0].z1);
    CHECK(plan.segments[1].kind == MotionKind::DownwardZ);
    CHECK(plan.segments[1].x0 == plan.segments[1].x1);
    CHECK(plan.segments[1].y0 == plan.segments[1].y1);
    CHECK(plan.segments[2].kind == MotionKind::Travel);
    CHECK(plan.segments[2].z0 == 1.0);

    CHECK(plan.report.motion_permitted());
    CHECK(mentions(plan.reasons, "verified empty"));
    CHECK(mentions(plan.reasons, "calibration uncertainty"));
}

TEST_CASE("A corridor over printed geometry is rejected, and the next one tried",
          "[descent-proof]")
{
    DescentContext over_the_part = context();
    over_the_part.corridors      = { corridor("straight down onto the leading tower", 10., 10.),
                                     corridor("the gap between the towers", 60., 10.) };

    const DescentPlan plan = prove_descent(descent(), over_the_part);

    CHECK(plan.verdict == DescentVerdict::Clear);
    CHECK(*plan.corridor == "the gap between the towers");
    CHECK(mentions(plan.rejected_corridors, "dropping onto the part"));
    CHECK(mentions(plan.rejected_corridors, "straight down onto the leading tower"));
}

TEST_CASE("A corridor whose printed height was never determined is not clear",
          "[descent-proof]")
{
    DescentContext unknown = context();
    // Blank one cell inside the corridor footprint.
    unknown.printed.height_mm[6 * unknown.printed.nx + 11] = std::nullopt;

    const DescentPlan plan = prove_descent(descent(), unknown);

    CHECK(plan.verdict == DescentVerdict::Unproven);
    CHECK_FALSE(plan.corridor.has_value());
    CHECK(mentions(plan.rejected_corridors, "unknown is not the same as clear"));
    CHECK(mentions(plan.reasons, "unproven rather than known bad"));
}

TEST_CASE("Clearing the nozzle point does not prove the carriage clears",
          "[descent-proof]")
{
    DescentContext tip_only = context();
    tip_only.shapes.front().body = Aabb {};

    const DescentPlan plan = prove_descent(descent(), tip_only);

    CHECK(plan.verdict == DescentVerdict::Unproven);
    CHECK(mentions(plan.reasons, "does not prove the carriage or gantry clears"));
}

TEST_CASE("A tool with no described shape proves nothing", "[descent-proof]")
{
    DescentContext bare = context();
    bare.shapes.clear();

    const DescentPlan plan = prove_descent(descent(), bare);
    CHECK(plan.verdict == DescentVerdict::Unproven);
    CHECK(mentions(plan.reasons, "no toolhead shape is described"));
}

TEST_CASE("An uncharacterized calibration uncertainty proves nothing",
          "[descent-proof]")
{
    DescentContext uncalibrated = context();
    uncalibrated.calibration_uncertainty_mm.reset();

    const DescentPlan plan = prove_descent(descent(), uncalibrated);
    CHECK(plan.verdict == DescentVerdict::Unproven);
    CHECK(mentions(plan.reasons, "never characterized"));
}

TEST_CASE("Descending wherever the toolhead happens to be is not a plan",
          "[descent-proof]")
{
    DescentContext nowhere = context();
    nowhere.corridors.clear();

    const DescentPlan plan = prove_descent(descent(), nowhere);
    CHECK(plan.verdict == DescentVerdict::Unproven);
    CHECK(mentions(plan.reasons, "no descent corridor was described"));
}

TEST_CASE("Hardware standing in the corridor refuses the descent",
          "[descent-proof]")
{
    Obstacle brush;
    brush.name = "the purge brush";
    brush.kind = ObstacleKind::Brush;
    brush.box  = Aabb::of(55., 5., 0., 65., 15., 40.);

    DescentContext blocked = context();
    blocked.obstacles      = { brush };

    const DescentPlan plan = prove_descent(descent(), blocked);

    CHECK(plan.verdict == DescentVerdict::Refused);
    CHECK(mentions(plan.rejected_corridors, "the purge brush"));
    CHECK(mentions(plan.reasons, "none of them is clear"));
}

TEST_CASE("Calibration uncertainty widens the clearance that has to hold",
          "[descent-proof]")
{
    // A fixture two millimetres off the toolhead body, all the way along.
    Obstacle clamp;
    clamp.name = "a bed clamp";
    clamp.kind = ObstacleKind::Fixture;
    clamp.box  = Aabb::of(-40., 24., 0., 300., 30., 40.);

    DescentContext tight = context();
    tight.obstacles      = { clamp };

    SECTION("clear when the machine is well characterized") {
        CHECK(prove_descent(descent(), tight).verdict == DescentVerdict::Clear);
    }

    SECTION("refused once the uncertainty is large enough to reach it") {
        tight.calibration_uncertainty_mm = 3.0;
        const DescentPlan plan           = prove_descent(descent(), tight);
        CHECK(plan.verdict == DescentVerdict::Refused);
        CHECK(mentions(plan.rejected_corridors, "a bed clamp"));
    }
}

TEST_CASE("Every descent in a batched schedule has to be proven",
          "[descent-proof]")
{
    // A return that would have to cross the leading tower at low z to reach
    // where it is going. The corridor itself is empty; the approach is not.
    DescentRequest across_the_part = descent();
    across_the_part.to_x           = 10.;
    across_the_part.to_z           = 0.5;

    SECTION("all clear") {
        const BatchDescentReview review =
            review_descents({ descent(), descent() }, context());
        CHECK(review.all_proven);
        CHECK(review.proven == 2);
        CHECK(provable_batch_prefix(review) == 2);
    }

    SECTION("one that cannot be proven stops the schedule there") {
        const BatchDescentReview review =
            review_descents({ descent(), across_the_part, descent() }, context());

        CHECK_FALSE(review.all_proven);
        CHECK(review.proven == 2);
        CHECK(review.refused == 1);
        // The schedule may only keep the batches before the one that failed.
        CHECK(provable_batch_prefix(review) == 1);
        CHECK(mentions(review.notes, "cannot be emitted"));
        CHECK(mentions(review.descents[1].rejected_corridors, "printed geometry"));
    }
}

TEST_CASE("A schedule with no downward moves needs no proof", "[descent-proof]")
{
    const BatchDescentReview review = review_descents({}, context());
    CHECK_FALSE(review.all_proven);
    CHECK(mentions(review.notes, "needs no downward moves at all"));
}
