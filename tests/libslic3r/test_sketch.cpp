#include <catch2/catch_all.hpp>

#include "libslic3r/Sketch.hpp"
#include "libslic3r/NSVGUtils.hpp"

#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
double area_mm2(const ExPolygons &area)
{
    double sum = 0.;
    for (const ExPolygon &ex : area)
        sum += ex.area();
    return sum * SCALING_FACTOR * SCALING_FACTOR;
}
} // namespace

TEST_CASE("Sketch shapes enclose the expected area", "[Sketch]")
{
    CHECK_THAT(area_mm2(sketch_to_expolygons({SketchShape::rectangle({0, 0}, {20, 10})})), WithinRel(200., 1e-6));
    // Corners in any order.
    CHECK_THAT(area_mm2(sketch_to_expolygons({SketchShape::rectangle({20, 10}, {0, 0})})), WithinRel(200., 1e-6));
    // Rounded corners take (4 - pi) r^2 off.
    CHECK_THAT(area_mm2(sketch_to_expolygons({SketchShape::rectangle({0, 0}, {20, 10}, 2.)})), WithinRel(200. - (4. - M_PI) * 4., 0.002));
    // Curves are straight segments no more than 0.01 mm inside the true curve.
    const double circle = area_mm2(sketch_to_expolygons({SketchShape::circle({5, 5}, 5.)}, 0.01));
    CHECK(circle <= M_PI * 25.);
    CHECK(circle >= M_PI * 25. - 2. * M_PI * 5. * 0.01);
    CHECK_THAT(area_mm2(sketch_to_expolygons({SketchShape::polygon({0, 0}, 10., 6)})), WithinRel(3. * std::sqrt(3.) / 2. * 100., 0.0001));
    // An outline drawn clockwise counts the same as one drawn counter-clockwise.
    CHECK_THAT(area_mm2(sketch_to_expolygons({SketchShape::outline({{0, 0}, {0, 10}, {10, 0}})})), WithinRel(50., 1e-6));
}

TEST_CASE("A sketch shape inside another cuts a hole, and one inside the hole fills it", "[Sketch]")
{
    const std::vector<SketchShape> plate_with_hole = {SketchShape::rectangle({0, 0}, {20, 10}), SketchShape::circle({10, 5}, 3.)};
    const ExPolygons area = sketch_to_expolygons(plate_with_hole);
    REQUIRE(area.size() == 1);
    CHECK(area.front().holes.size() == 1);
    CHECK_THAT(area_mm2(area), WithinRel(200. - M_PI * 9., 0.002));

    std::vector<SketchShape> filled = plate_with_hole;
    filled.push_back(SketchShape::circle({10, 5}, 1.5));
    CHECK(sketch_to_expolygons(filled).size() == 2);
}

TEST_CASE("Empty sketch shapes are ignored", "[Sketch]")
{
    CHECK(sketch_to_expolygons({}).empty());
    CHECK(sketch_to_expolygons({SketchShape::rectangle({0, 0}, {10, 0})}).empty());
    CHECK(sketch_to_expolygons({SketchShape::circle({0, 0}, 0.)}).empty());
    CHECK(sketch_to_expolygons({SketchShape::outline({{0, 0}, {5, 5}})}).empty());
    CHECK(sketch_to_svg({}).empty());
}

TEST_CASE("A sketch's SVG reads back at its size in mm", "[Sketch]")
{
    const std::string svg = sketch_to_svg({SketchShape::rectangle({-5, -2}, {15, 8}), SketchShape::circle({5, 3}, 2.)});
    REQUIRE(!svg.empty());
    NSVGimage_ptr image = nsvgParse(svg); // path points in mm, as the SVG tool reads them
    REQUIRE(image != nullptr);
    REQUIRE(image->shapes != nullptr);
    CHECK(image->shapes->fillRule == NSVG_FILLRULE_EVENODD);
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (NSVGpath *path = image->shapes->paths; path != nullptr; path = path->next)
        for (int i = 0; i < path->npts; ++i) {
            min_x = std::min(min_x, path->pts[2 * i]);
            max_x = std::max(max_x, path->pts[2 * i]);
            min_y = std::min(min_y, path->pts[2 * i + 1]);
            max_y = std::max(max_y, path->pts[2 * i + 1]);
        }
    // The 20 x 10 mm rectangle, moved to the origin.
    CHECK_THAT(max_x - min_x, WithinAbs(20., 1e-3));
    CHECK_THAT(max_y - min_y, WithinAbs(10., 1e-3));
    CHECK_THAT(min_x, WithinAbs(0., 1e-3));
}

TEST_CASE("Sketch shapes report bounds and hits", "[Sketch]")
{
    SketchShape circle = SketchShape::circle({0, 0}, 5.);
    CHECK(circle.contains({1, 1}));
    CHECK_FALSE(circle.contains({4.9, 4.9}));
    circle.translate({10, 0});
    const auto [lo, hi] = circle.bounds();
    CHECK_THAT(lo.x(), WithinAbs(5., 0.01));
    CHECK_THAT(hi.x(), WithinAbs(15., 0.01));
}
