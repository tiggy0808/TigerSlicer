#include "Sketch.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace Slic3r {

namespace {

// Number of segments for an arc of the given radius and angle, so no chord strays more than max_deviation.
int arc_segments(double radius, double angle, double max_deviation)
{
    if (radius <= max_deviation)
        return std::max(1, int(std::ceil(angle / (M_PI / 2.))));
    const double step = 2. * std::acos(1. - max_deviation / radius);
    return std::clamp(int(std::ceil(angle / step)), 3, 4096);
}

void append_arc(std::vector<Vec2d> &out, const Vec2d &center, double radius, double from, double to, double max_deviation)
{
    const int n = arc_segments(radius, to - from, max_deviation);
    for (int i = 0; i <= n; ++i) {
        const double t = from + (to - from) * double(i) / double(n);
        out.emplace_back(center + radius * Vec2d(std::cos(t), std::sin(t)));
    }
}

double signed_area(const std::vector<Vec2d> &pts)
{
    double area = 0.;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
        area += pts[j].x() * pts[i].y() - pts[i].x() * pts[j].y();
    return area / 2.;
}

std::string format_mm(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << value;
    std::string s = out.str();
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s == "-0" ? "0" : s;
}

} // namespace

SketchShape SketchShape::rectangle(const Vec2d &corner1, const Vec2d &corner2, double corner_radius)
{
    SketchShape shape;
    shape.kind          = Kind::Rectangle;
    shape.a             = corner1;
    shape.b             = corner2;
    shape.corner_radius = std::max(0., corner_radius);
    return shape;
}

SketchShape SketchShape::circle(const Vec2d &center, double radius)
{
    SketchShape shape;
    shape.kind = Kind::Circle;
    shape.a    = center;
    shape.b    = center + Vec2d(radius, 0.);
    return shape;
}

SketchShape SketchShape::polygon(const Vec2d &center, double radius, int sides)
{
    SketchShape shape;
    shape.kind  = Kind::Polygon;
    shape.a     = center;
    shape.b     = center + Vec2d(0., radius);
    shape.sides = std::max(3, sides);
    return shape;
}

SketchShape SketchShape::outline(std::vector<Vec2d> points)
{
    SketchShape shape;
    shape.kind   = Kind::Outline;
    shape.points = std::move(points);
    return shape;
}

std::vector<Vec2d> SketchShape::contour(double max_deviation) const
{
    std::vector<Vec2d> out;
    switch (kind) {
    case Kind::Rectangle: {
        const Vec2d  lo = a.cwiseMin(b), hi = a.cwiseMax(b);
        const double w = hi.x() - lo.x(), h = hi.y() - lo.y();
        if (w <= 0. || h <= 0.)
            return {};
        const double r = std::min(corner_radius, std::min(w, h) / 2.);
        if (r <= 0.) {
            out = {lo, {hi.x(), lo.y()}, hi, {lo.x(), hi.y()}};
            break;
        }
        append_arc(out, {hi.x() - r, lo.y() + r}, r, -M_PI / 2., 0., max_deviation);
        append_arc(out, {hi.x() - r, hi.y() - r}, r, 0., M_PI / 2., max_deviation);
        append_arc(out, {lo.x() + r, hi.y() - r}, r, M_PI / 2., M_PI, max_deviation);
        append_arc(out, {lo.x() + r, lo.y() + r}, r, M_PI, 1.5 * M_PI, max_deviation);
        break;
    }
    case Kind::Circle: {
        const double r = radius();
        if (r <= 0.)
            return {};
        append_arc(out, a, r, 0., 2. * M_PI, max_deviation);
        out.pop_back(); // the last point repeats the first
        break;
    }
    case Kind::Polygon: {
        const double r = radius();
        if (r <= 0. || sides < 3)
            return {};
        const double start = std::atan2(b.y() - a.y(), b.x() - a.x());
        for (int i = 0; i < sides; ++i) {
            const double t = start + 2. * M_PI * double(i) / double(sides);
            out.emplace_back(a + r * Vec2d(std::cos(t), std::sin(t)));
        }
        break;
    }
    case Kind::Outline:
        out = points;
        break;
    }
    if (out.size() < 3 || std::abs(signed_area(out)) <= 0.)
        return {};
    if (signed_area(out) < 0.)
        std::reverse(out.begin(), out.end());
    return out;
}

std::pair<Vec2d, Vec2d> SketchShape::bounds() const
{
    const std::vector<Vec2d> pts = contour();
    if (pts.empty())
        return {a.cwiseMin(b), a.cwiseMax(b)};
    Vec2d lo = pts.front(), hi = pts.front();
    for (const Vec2d &p : pts) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    return {lo, hi};
}

bool SketchShape::contains(const Vec2d &point) const
{
    const std::vector<Vec2d> pts = contour();
    bool inside = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
        if ((pts[i].y() > point.y()) != (pts[j].y() > point.y()) &&
            point.x() < (pts[j].x() - pts[i].x()) * (point.y() - pts[i].y()) / (pts[j].y() - pts[i].y()) + pts[i].x())
            inside = !inside;
    return inside;
}

void SketchShape::translate(const Vec2d &offset)
{
    a += offset;
    b += offset;
    for (Vec2d &p : points)
        p += offset;
}

ExPolygons sketch_to_expolygons(const std::vector<SketchShape> &shapes, double max_deviation)
{
    Polygons polygons;
    for (const SketchShape &shape : shapes) {
        const std::vector<Vec2d> pts = shape.contour(max_deviation);
        if (pts.empty())
            continue;
        Polygon polygon;
        polygon.points.reserve(pts.size());
        for (const Vec2d &p : pts)
            polygon.points.emplace_back(scaled(p.x()), scaled(p.y()));
        polygons.emplace_back(std::move(polygon));
    }
    return polygons.empty() ? ExPolygons{} : union_ex(polygons, ClipperLib::pftEvenOdd);
}

std::string sketch_to_svg(const std::vector<SketchShape> &shapes, double max_deviation)
{
    const ExPolygons area = sketch_to_expolygons(shapes, max_deviation);
    if (area.empty())
        return {};
    const BoundingBox bb   = get_extents(area);
    const double      minx = unscaled(bb.min.x()), maxy = unscaled(bb.max.y());
    const double      w = unscaled(bb.max.x()) - minx, h = maxy - unscaled(bb.min.y());

    std::ostringstream path;
    auto add_loop = [&](const Polygon &loop) {
        for (size_t i = 0; i < loop.points.size(); ++i)
            // SVG's y axis points down.
            path << (i == 0 ? "M" : "L") << format_mm(unscaled(loop.points[i].x()) - minx) << ","
                 << format_mm(maxy - unscaled(loop.points[i].y())) << " ";
        path << "Z ";
    };
    for (const ExPolygon &ex : area) {
        add_loop(ex.contour);
        for (const Polygon &hole : ex.holes)
            add_loop(hole);
    }
    std::string d = path.str();
    if (!d.empty())
        d.pop_back();

    std::ostringstream svg;
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << format_mm(w) << "mm\" height=\"" << format_mm(h)
        << "mm\" viewBox=\"0 0 " << format_mm(w) << " " << format_mm(h) << "\">\n"
        << "<path fill=\"#F6A5C1\" fill-rule=\"evenodd\" d=\"" << d << "\"/>\n"
        << "</svg>\n";
    return svg.str();
}

} // namespace Slic3r
