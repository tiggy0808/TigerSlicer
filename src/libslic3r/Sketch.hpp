#ifndef slic3r_Sketch_hpp_
#define slic3r_Sketch_hpp_

// TigerSlicer: the geometry behind the Sketch tool. A sketch is a list of closed 2D shapes in mm, drawn on a
// plane. It is not parametric: shapes have no constraints and nothing refers to anything else. Extruding it
// turns the shapes into an SVG in mm, which the SVG tool (GLGizmoSVG) makes into a new object, a part or a cut.

#include "ExPolygon.hpp"
#include "Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {

struct SketchShape
{
    enum class Kind { Rectangle, Circle, Polygon, Outline };

    Kind  kind = Kind::Rectangle;
    // Rectangle: two opposite corners. Circle and Polygon: the centre (a) and a point on the circumference (b).
    Vec2d a = Vec2d::Zero();
    Vec2d b = Vec2d::Zero();
    // Rectangle only: radius of its rounded corners, limited to half the shorter side.
    double corner_radius = 0.;
    // Polygon only: number of sides, at least 3.
    int sides = 6;
    // Outline only: the points of a closed outline, in drawing order.
    std::vector<Vec2d> points;

    static SketchShape rectangle(const Vec2d &corner1, const Vec2d &corner2, double corner_radius = 0.);
    static SketchShape circle(const Vec2d &center, double radius);
    static SketchShape polygon(const Vec2d &center, double radius, int sides);
    static SketchShape outline(std::vector<Vec2d> points);

    // Closed outline in mm, counter-clockwise. Curves are split so no chord strays more than
    // max_deviation (mm) from the true curve. Empty for a shape that encloses no area.
    std::vector<Vec2d> contour(double max_deviation = 0.01) const;
    // Axis-aligned bounds of the contour, as min and max corners.
    std::pair<Vec2d, Vec2d> bounds() const;
    bool  contains(const Vec2d &point) const;
    void  translate(const Vec2d &offset);
    double radius() const { return (b - a).norm(); }
};

// The area a set of shapes encloses, in scaled coordinates. Overlaps count even-odd: a shape drawn inside
// another one cuts a hole in it, and a shape inside that hole fills it again.
ExPolygons sketch_to_expolygons(const std::vector<SketchShape> &shapes, double max_deviation = 0.01);

// The same area as an SVG whose units are mm (width and height in mm, viewBox in mm), one even-odd path.
// Empty when the shapes enclose no area.
std::string sketch_to_svg(const std::vector<SketchShape> &shapes, double max_deviation = 0.01);

} // namespace Slic3r

#endif // slic3r_Sketch_hpp_
