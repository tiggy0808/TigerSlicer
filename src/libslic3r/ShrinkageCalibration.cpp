#include "ShrinkageCalibration.hpp"

#include "ClipperUtils.hpp"
#include "Emboss.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r::shrinkage_calibration {

namespace {

// Test part layout, mm. The arms are kArmWidth wide; each carries three tiers of kTierHeight on a base plate of
// kBaseHeight. The base plate is inset by kInset from every measured face, so first layer squish never touches one.
constexpr double kArmWidth    = 14.;
constexpr double kBaseHeight  = 2.;
constexpr double kTierHeight  = 2.;
constexpr double kInset       = 1.;
constexpr double kEngraving   = 0.6;  // depth of the X and Y letters
constexpr double kLetterSize  = 8.;
constexpr double kStroke      = 1.4;
// Holes, wall and Z staircase stand in the inner corner of the L, on a pad of the base plate.
constexpr double kFeatures    = kArmWidth + 4.;     // where they start, in X and in Y
constexpr double kPadMaxX     = kArmWidth + 56.;
constexpr double kPadMaxY     = kArmWidth + 38.;
constexpr double kBossHeight  = 6.;
constexpr double kWallHeight  = 10.;
constexpr double kMargin      = 10.;                // kept free around the part on the bed
constexpr double kMinScale    = 0.5;                // smaller beds than this are refused
constexpr double kStairWidth  = 12.;
constexpr double kStairDepth  = 10.;

Polygon rect(double x0, double y0, double x1, double y1)
{
    return Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
}

Polygon circle(double cx, double cy, double r)
{
    Polygon out;
    const int n = 180;
    for (int i = 0; i < n; ++i) {
        const double a = 2. * M_PI * i / n;
        out.points.push_back(Point::new_scale(cx + r * std::cos(a), cy + r * std::sin(a)));
    }
    return out;
}

// A straight stroke from p to q, kStroke wide.
Polygon stroke(Vec2d p, Vec2d q)
{
    const Vec2d n = (q - p).normalized();
    const Vec2d s = Vec2d(-n.y(), n.x()) * (kStroke / 2.);
    p -= n * (kStroke / 2.);
    q += n * (kStroke / 2.);
    Polygon out({Point::new_scale(p.x() + s.x(), p.y() + s.y()), Point::new_scale(p.x() - s.x(), p.y() - s.y()),
                 Point::new_scale(q.x() - s.x(), q.y() - s.y()), Point::new_scale(q.x() + s.x(), q.y() + s.y())});
    if (out.is_clockwise())
        out.reverse();
    return out;
}

ExPolygons letter(char c, const Vec2d &center)
{
    const double h = kLetterSize / 2. - kStroke / 2.;
    Polygons strokes;
    if (c == 'X') {
        strokes = {stroke(center + Vec2d(-h, -h), center + Vec2d(h, h)), stroke(center + Vec2d(-h, h), center + Vec2d(h, -h))};
    } else {
        strokes = {stroke(center + Vec2d(-h, h), center), stroke(center + Vec2d(h, h), center), stroke(center, center + Vec2d(0., -h))};
    }
    return union_ex(strokes);
}

// The area extruded from z0 to z1.
void add_prism(indexed_triangle_set &out, const ExPolygons &area, double z0, double z1)
{
    if (area.empty() || z1 <= z0)
        return;
    Emboss::ProjectScale projection(std::make_unique<Emboss::ProjectZ>((z1 - z0) / SCALING_FACTOR), SCALING_FACTOR);
    indexed_triangle_set prism = Emboss::polygons2model(area, projection);
    if (its_volume(prism) < 0.f)
        its_flip_triangles(prism);
    for (stl_vertex &v : prism.vertices)
        v.z() += float(z0);
    its_merge(out, prism);
}

double round_half_mm(double v) { return std::round(v * 2.) / 2.; }

} // namespace

LinearFit fit_line(const std::vector<double> &nominal, const std::vector<double> &measured)
{
    LinearFit fit;
    const size_t n = std::min(nominal.size(), measured.size());
    if (n < 2)
        return fit;
    const double mx = std::accumulate(nominal.begin(), nominal.begin() + n, 0.) / n;
    const double my = std::accumulate(measured.begin(), measured.begin() + n, 0.) / n;
    double sxx = 0., sxy = 0.;
    for (size_t i = 0; i < n; ++i) {
        sxx += (nominal[i] - mx) * (nominal[i] - mx);
        sxy += (nominal[i] - mx) * (measured[i] - my);
    }
    if (sxx <= 0.)
        return fit;
    fit.valid     = true;
    fit.slope     = sxy / sxx;
    fit.intercept = my - fit.slope * mx;
    double sum_sq = 0.;
    for (size_t i = 0; i < n; ++i) {
        const double r = measured[i] - (fit.slope * nominal[i] + fit.intercept);
        sum_sq += r * r;
        fit.max_residual = std::max(fit.max_residual, std::abs(r));
    }
    fit.rms_residual = std::sqrt(sum_sq / n);
    return fit;
}

std::optional<Nominals> nominals_for_bed(const BoundingBoxf &bed, double max_print_height)
{
    Nominals n;
    const Vec2d size = bed.size();
    if (size.x() < kPadMaxX + kMargin || size.y() < kPadMaxY + kMargin)
        return std::nullopt;
    const double scale = std::min({1., (size.x() - kMargin) / n.x.back(), (size.y() - kMargin) / n.y.back()});
    if (scale < kMinScale)
        return std::nullopt;
    for (double &v : n.x) v = round_half_mm(v * scale);
    for (double &v : n.y) v = round_half_mm(v * scale);
    if (max_print_height > 0. && max_print_height < n.z.back() + kMargin / 2.) {
        const double zscale = (max_print_height - kMargin / 2.) / n.z.back();
        if (zscale < kMinScale)
            return std::nullopt;
        for (double &v : n.z) v = round_half_mm(v * zscale);
    }
    return n;
}

double nominal(const Nominals &n, Field f)
{
    switch (f) {
    case Field::X1: case Field::X2: case Field::X3: return n.x[int(f) - int(Field::X1)];
    case Field::Y1: case Field::Y2: case Field::Y3: return n.y[int(f) - int(Field::Y1)];
    case Field::Z1: case Field::Z2: case Field::Z3: return n.z[int(f) - int(Field::Z1)];
    case Field::HoleSmall: return n.hole_small;
    case Field::HoleLarge: return n.hole_large;
    case Field::Wall: return n.wall;
    default: return 0.;
    }
}

double measured(const Measurements &m, Field f)
{
    switch (f) {
    case Field::X1: case Field::X2: case Field::X3: return m.x[int(f) - int(Field::X1)];
    case Field::Y1: case Field::Y2: case Field::Y3: return m.y[int(f) - int(Field::Y1)];
    case Field::Z1: case Field::Z2: case Field::Z3: return m.z[int(f) - int(Field::Z1)];
    case Field::HoleSmall: return m.hole_small;
    case Field::HoleLarge: return m.hole_large;
    case Field::Wall: return m.wall;
    default: return 0.;
    }
}

std::vector<Field> out_of_range(const Nominals &n, const Measurements &m)
{
    std::vector<Field> out;
    for (int i = 0; i < int(Field::Count); ++i) {
        const Field  f = Field(i);
        const double v = measured(m, f), nom = nominal(n, f);
        if (!(v > 0.) || std::abs(v / nom - 1.) > kMaxDeviation)
            out.push_back(f);
    }
    return out;
}

Result analyse(const Nominals &n, const Measurements &m)
{
    auto axis = [](const std::array<double, 3> &nom, const std::array<double, 3> &meas) {
        AxisResult r;
        r.fit          = fit_line({nom.begin(), nom.end()}, {meas.begin(), meas.end()});
        r.percent      = r.fit.slope * 100.;
        r.inconsistent = r.fit.max_residual > kResidualLimitMm;
        return r;
    };
    Result r;
    r.x = axis(n.x, m.x);
    r.y = axis(n.y, m.y);
    r.z = axis(n.z, m.z);
    r.xy_percent   = (r.x.percent + r.y.percent) / 2.;
    r.anisotropy   = r.x.percent - r.y.percent;
    r.anisotropic  = std::abs(r.anisotropy) > kAnisotropyLimit + 1e-9;
    r.xy_offset    = (r.x.fit.intercept + r.y.fit.intercept) / 2.;
    r.large_offset = std::abs(r.xy_offset) > kLargeOffsetMm;
    // A constant offset b widens a part by b, so each side moves by b / 2; the compensation undoes it.
    r.suggested_contour_compensation = -r.xy_offset / 2.;
    const double scale      = r.xy_percent / 100.;
    const double hole_error = ((m.hole_small - scale * n.hole_small) + (m.hole_large - scale * n.hole_large)) / 2.;
    r.suggested_hole_compensation = -hole_error / 2.;
    r.wall_expected = scale * n.wall + r.xy_offset;
    r.wall_error    = m.wall - r.wall_expected;
    return r;
}

double corrected_percent(double printed_with_percent, double measured_percent)
{
    // Printed size = material shrinkage * (100 / printed_with) * nominal, so the material's own shrinkage is
    // measured * printed_with / 100.
    return printed_with_percent * measured_percent / 100.;
}

TriangleMesh make_test_part(const Nominals &n)
{
    const double w = kArmWidth;
    const double x1 = n.x[0], x2 = n.x[1], x3 = n.x[2];
    const double y1 = n.y[0], y2 = n.y[1], y3 = n.y[2];
    const double f  = kFeatures;
    const Polygon hole_small = circle(f + 7., f + 8., n.hole_small / 2.);
    const Polygon hole_large = circle(f + 21., f + 8., n.hole_large / 2.);
    const Polygons holes     = {hole_small, hole_large};

    indexed_triangle_set its;
    // Base plate: the L of the arms and the pad in its inner corner, inset from the measured faces, with the holes.
    const ExPolygons base = diff_ex(Polygons{rect(kInset, kInset, x3 - kInset, w - kInset), rect(kInset, kInset, w - kInset, y3 - kInset),
                                             rect(w - kInset, w - kInset, kPadMaxX, kPadMaxY)},
                                    holes);
    add_prism(its, base, 0., kBaseHeight);

    // Arms: three tiers each; length i is measured across tier 3 - i, from the datum face (x or y = 0).
    // The top of the bottom tier, where only it shows, carries the axis letter, engraved.
    const ExPolygons letter_x = letter('X', {(x2 + x3) / 2., w / 2.});
    const ExPolygons letter_y = letter('Y', {w / 2., (y2 + y3) / 2.});
    const double     z_a = kBaseHeight, z_b = z_a + kTierHeight, z_c = z_b + kTierHeight, z_top = z_c + kTierHeight;
    add_prism(its, union_ex(Polygons{rect(0, 0, x3, w), rect(0, 0, w, y3)}), z_a, z_b - kEngraving);
    Polygons letters = to_polygons(letter_x);
    append(letters, to_polygons(letter_y));
    add_prism(its, diff_ex(Polygons{rect(0, 0, x3, w), rect(0, 0, w, y3)}, letters), z_b - kEngraving, z_b);
    add_prism(its, union_ex(Polygons{rect(0, 0, x2, w), rect(0, 0, w, y2)}), z_b, z_c);
    add_prism(its, union_ex(Polygons{rect(0, 0, x1, w), rect(0, 0, w, y1)}), z_c, z_top);

    // Holes in a boss, a thin wall, and the Z staircase (heights measured from the bed).
    add_prism(its, diff_ex(Polygons{rect(f, f, f + 30., f + 16.)}, holes), 0., kBossHeight);
    add_prism(its, ExPolygons{ExPolygon(rect(f, f + 26., f + 30., f + 26. + n.wall))}, 0., kWallHeight);
    for (int i = 0; i < 3; ++i)
        add_prism(its, ExPolygons{ExPolygon(rect(f + 36., f + kStairDepth * i, f + 36. + kStairWidth, f + kStairDepth * (i + 1)))}, 0., n.z[i]);

    return TriangleMesh(std::move(its));
}

} // namespace Slic3r::shrinkage_calibration
