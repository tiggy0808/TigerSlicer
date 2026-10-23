// Breakaway fin support (support type "fins").
//
// A fin is a thin solid wall standing on the bed beside the object, a small XY gap away from it,
// with a one-layer "tine" every few layers that bridges the gap and fuses into the object. The
// wall props the object up; the tines stop the object from leaning away from the wall, which
// matters most for parts printed tilted onto an edge. Because a tine is a single horizontal bead
// in the plane of the layer, it snaps off cleanly when the fin is bent away.
// The technique and its dimensions (0.2 mm standoff, one-layer tines about 0.5 mm wide,
// a 1 mm base at the bed) follow Slant3D's designed-in support fins (YouTube @slant3d,
// "How to Design Better Support Fins"). CNC Kitchen (YouTube @CNCKitchen, "Stop Printing Flat:
// The 45 Degree Secret for Stronger Parts") measured why printing tilted pays off: PLA printed at
// 45 degrees tested 27% stronger than upright. Fins are what make that orientation printable.
// The slicer-side design draws on Support Fins by Matthew Trahan (gittrahan,
// https://github.com/gittrahan/support-fins, MIT), which adds these fins to STL files;
// the default fin thickness and tine depth come from its geometry spec.
//
// Fins hold the object up rather than an overhang surface, so they do not use overhang
// detection. For every object layer up to support_fin_height:
//  1. The fin region is the area under the object (the projection of all layers above) that
//     is clear of the object down to the bed, kept support_object_xy_distance off the object
//     and support_top_z_distance below it. Support blockers are subtracted. Fins (manual)
//     (TigerSlicer) keeps only the part under support enforcers: the projection of the enforcer
//     volumes and painted enforcer facets at or above the layer.
//  2. The region is clipped to parallel strips (the fins), support_fin_thickness wide and one
//     every support_fin_spacing (or, as a percentage, filling that share of the area). The strips
//     are fixed in XY for the whole object, so each fin is one continuous wall. Under an object that
//     would tip over, they run the way it leans (TigerSlicer), so each fin is a wall in the plane it
//     would tip in; under one that stands on its own they run across the long axis of the fin regions. One strip runs down the middle of the fin regions, so a
//     narrow part gets one fin down its middle (TigerSlicer). A column between two strips gets its own fin.
//     With support_fin_lean_side_only, only the fin columns on the side the object leans toward
//     are kept (see keep_lean_side()).
//     With support_fin_cross_spacing, fins also run across the main fins, bracing them into a scaffold.
//  3. Within the first kBaseHeight above the bed the strips are widened into a foot.
//     With support_fin_holes (TigerSlicer), a diamond lattice is cut through the middle of each
//     main fin, see fin_hole_bands(). The foot, the fin ends and the top edge stay solid.
//  4. On tine layers, each fin that ends at the object is extended across the XY gap and
//     support_fin_tine_depth into the object slice. Tine rows are support_fin_tine_spacing apart; with
//     support_fin_tine_base_rows, extra rows are packed just above the foot, where the object is least
//     stable (Slant3D). Spacing by distance keeps the number of tines proportional to the fin height.
//  5. With support_fin_interface_layers, the fin region right under the object is printed as
//     interface layers across the fin tops.
// The result is printed through the common support toolpath generator.

#include "SupportFins.hpp"
#include "SupportCommon.hpp"
#include "../ClipperUtils.hpp"
#include "../Geometry/ConvexHull.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

namespace Slic3r {

// Height of the widened fin foot on the bed, and its width as a multiple of the fin thickness.
static constexpr double kBaseHeight     = 1.0;
static constexpr double kBaseWidthRatio = 3.0;
// A fin taller than this many times its length is reported as slender.
static constexpr double kSlenderRatio = 10.0;
// An object whose center of mass is less than this far (mm) from the center of its bed contact is balanced.
static constexpr double kMinLean = 0.5;

// Extra rows near the bed: the first just above the foot, then gaps from kTineGapMin growing by
// kTineGapGrowth until they reach the regular tine spacing.
static constexpr double kTineGapMin    = 0.8;
static constexpr double kTineGapGrowth = 1.6;

// Fin holes: lattice struts kHolePitch apart and kHoleStrut wide, crossing at 45 degrees in the
// fin's plane, and a solid frame kHoleFrame wide along every edge of the fin.
static constexpr double kHolePitch = 8.0;
static constexpr double kHoleStrut = 1.6;
static constexpr double kHoleFrame = 2.5;

// Where a fin is open at height z: intervals of [0, pitch) along the fin, repeating every pitch.
// The struts are the lines u + z = k * pitch and u - z = k * pitch of the fin's plane (u along the
// fin), so the holes are diamonds that line up from layer to layer.
std::vector<std::pair<double, double>> fin_hole_intervals(double z, double pitch, double strut)
{
    // Half a strut, measured along u: a 45 degree strut is sqrt(2) times wider along u than across.
    const double w   = 0.5 * strut * std::sqrt(2.);
    const double len = pitch - 2. * w;
    std::vector<std::pair<double, double>> result;
    if (len <= 0.)
        return result;
    // The u in [0, pitch) clear of one strut family: (u + s) mod pitch in (w, pitch - w).
    auto clear_of = [pitch, w, len](double s) {
        double a = std::fmod(w - s, pitch);
        if (a < 0.)
            a += pitch;
        std::vector<std::pair<double, double>> out;
        if (a + len <= pitch)
            out.emplace_back(a, a + len);
        else {
            out.emplace_back(a, pitch);
            out.emplace_back(0., a + len - pitch);
        }
        return out;
    };
    for (const auto& [a0, a1] : clear_of(z))
        for (const auto& [b0, b1] : clear_of(-z))
            if (std::max(a0, b0) < std::min(a1, b1))
                result.emplace_back(std::max(a0, b0), std::min(a1, b1));
    return result;
}

// Bands across the fins over the holes at height z. `rbox` is the extent in the frame where the
// fins run along X; the bands are rotated back to run across fins running along `angle`.
static Polygons fin_hole_bands(const BoundingBox& rbox, double angle, double z)
{
    const auto intervals = fin_hole_intervals(z, kHolePitch, kHoleStrut);
    Polygons   bands;
    const double x_min = unscale<double>(rbox.min.x()), x_max = unscale<double>(rbox.max.x());
    for (double base = std::floor(x_min / kHolePitch) * kHolePitch; base < x_max; base += kHolePitch)
        for (const auto& [start, end] : intervals) {
            const coord_t x0 = scaled<coord_t>(base + start), x1 = scaled<coord_t>(base + end);
            Polygon band({{x0, rbox.min.y()}, {x1, rbox.min.y()}, {x1, rbox.max.y()}, {x0, rbox.max.y()}});
            band.rotate(angle);
            bands.emplace_back(std::move(band));
        }
    return bands;
}

// Height between tine rows in mm, 0 for none. As a percentage (TigerSlicer), support_fin_tine_spacing is
// the share of a fin's height taken by its one-layer tines, like a density: 4% at 0.2 mm layers is a
// row every 5 mm.
static double tine_row_spacing(const PrintObjectConfig& config)
{
    const ConfigOptionFloatOrPercent& spacing = config.support_fin_tine_spacing;
    if (spacing.value <= 0.)
        return 0.;
    return spacing.percent ? config.layer_height.value * 100. / std::min(spacing.value, 100.) : spacing.value;
}

// Distance between the centers of neighboring fins. support_fin_spacing in mm is the gap between them;
// as a percentage (TigerSlicer) it is the share of the area the fins fill, like an infill density.
static coord_t fin_pitch(const ConfigOptionFloatOrPercent& spacing, coord_t thickness)
{
    if (spacing.percent)
        return coord_t(double(thickness) * 100. / std::clamp(spacing.value, 1., 100.));
    return std::max<coord_t>(0, scaled<coord_t>(spacing.value)) + thickness;
}

std::vector<bool> fin_tine_layers(const PrintObject& object)
{
    const PrintObjectConfig& config = object.config();
    const auto layers               = object.layers();
    const double spacing            = tine_row_spacing(config);
    std::vector<bool> rows(layers.size(), false);
    if (spacing <= 0.)
        return rows;
    // Regular rows, starting just above the foot.
    double next = kBaseHeight;
    double gap  = config.support_fin_tine_base_rows.value ? std::min(kTineGapMin, spacing) : spacing;
    for (size_t i = 1; i < layers.size(); ++i)
        if (layers[i]->bottom_z() > next - EPSILON) {
            rows[i] = true;
            next    = layers[i]->print_z + gap;
            gap     = std::min(gap * kTineGapGrowth, spacing);
        }
    return rows;
}

// Strips are built horizontal in a frame rotated by -angle, then rotated back to run along `angle`.
static BoundingBox rotated_extents(const BoundingBox& bbox, double angle, coord_t margin)
{
    Polygon frame = bbox.polygon();
    frame.rotate(-angle);
    BoundingBox rbox = frame.bounding_box();
    rbox.offset(margin);
    return rbox;
}

static Polygon fin_strip(const BoundingBox& rbox, double angle, coord_t y, coord_t width)
{
    const coord_t half = width / 2;
    Polygon strip({{rbox.min.x(), y - half}, {rbox.max.x(), y - half}, {rbox.max.x(), y + half}, {rbox.min.x(), y + half}});
    strip.rotate(angle);
    return strip;
}

// Parallel strips `width` wide with centers `pitch` apart and one at `offset`, running along
// `angle` and covering `bbox`. `offset` is measured across the strips, in the frame rotated by -angle.
static Polygons fin_strips(const BoundingBox& bbox, double angle, coord_t pitch, coord_t width, coord_t offset)
{
    const BoundingBox rbox = rotated_extents(bbox, angle, width);
    Polygons strips;
    for (coord_t k = coord_t(std::floor(double(rbox.min.y() - offset) / pitch)); offset + k * pitch <= rbox.max.y() + pitch; ++k)
        strips.emplace_back(fin_strip(rbox, angle, offset + k * pitch, width));
    return strips;
}

// Area-weighted centroid of `expolys` (holes subtract) in scaled coordinates; `area` receives the total area.
static Vec2d area_centroid(const ExPolygons& expolys, double& area)
{
    Vec2d sum = Vec2d::Zero();
    area      = 0.;
    for (const ExPolygon& expoly : expolys) {
        // Polygon::area() is signed: contours are CCW (positive) and holes CW (negative).
        sum += expoly.contour.area() * expoly.contour.centroid().cast<double>();
        area += expoly.contour.area();
        for (const Polygon& hole : expoly.holes) {
            sum += hole.area() * hole.centroid().cast<double>();
            area += hole.area();
        }
    }
    return area > 0. ? Vec2d(sum / area) : sum;
}

// Where to put the strips of fin_strips() across `footprint`: one down its middle, through its
// area centroid, and the rest every pitch either side of it. A part narrower than two pitches gets
// one fin, down its middle, which is where a round or tilted part hangs lowest.
static coord_t fin_strips_offset(const Polygons& footprint, double angle)
{
    Polygons across = footprint;
    for (Polygon& poly : across)
        poly.rotate(-angle);
    double area;
    return coord_t(std::lround(area_centroid(union_ex(across), area).y()));
}

// The center of the object's contact with the bed, and the way the object leans: from that center
// to its center of mass. Both in XY, scaled. `tips_over` when the center of mass is outside the
// convex hull of the contact, so the object would fall over without support.
struct FinLean
{
    Vec2d base;
    Vec2d lean;
    bool  tips_over;
};

// Empty when the object or its bed contact has no area.
static std::optional<FinLean> object_lean(const LayerPtrs& layers)
{
    // Center of mass in XY: slice centroids weighted by slice area times layer height.
    Vec2d com   = Vec2d::Zero();
    double mass = 0.;
    for (const Layer* layer : layers) {
        double area;
        const Vec2d c = area_centroid(layer->lslices, area);
        com += c * area * layer->height;
        mass += area * layer->height;
    }
    double base_area;
    const Vec2d base = area_centroid(layers.front()->lslices, base_area);
    if (mass <= 0. || base_area <= 0.)
        return std::nullopt;
    com /= mass;
    const Point com_point(coord_t(std::lround(com.x())), coord_t(std::lround(com.y())));
    return FinLean{base, com - base, !Geometry::convex_hull(layers.front()->lslices).contains(com_point)};
}

// Direction the fins run, in radians. A fin is stiff only in its own plane, so under an object that
// would tip over the fins run the way it leans, where they hold it up: a long part tilted along its
// length gets fins down its length, not a row of short fins across it. Under an object that stands
// on its own they run across the long axis of the fin regions. Second moments are summed per island
// about the island's own centroid, so islands on opposite sides of the object do not read as one
// region spread across the object.
static double fin_angle(const LayerPtrs& layers, const std::vector<Polygons>& regions)
{
    if (const std::optional<FinLean> lean = object_lean(layers); lean && lean->tips_over && lean->lean.norm() >= scale_(kMinLean))
        return std::atan2(lean->lean.y(), lean->lean.x());
    double sxx = 0., syy = 0., sxy = 0.;
    for (const Polygons& region : regions)
        for (const ExPolygon& island : union_ex(region)) {
            double a = 0., cx = 0., cy = 0., ixx = 0., iyy = 0., ixy = 0.;
            auto add = [&](const Polygon& poly) {
                const Points& pts = poly.points;
                for (size_t i = 0; i < pts.size(); ++i) {
                    const Point &p0 = pts[i], &p1 = pts[(i + 1) % pts.size()];
                    const double x0 = unscale<double>(p0.x()), y0 = unscale<double>(p0.y());
                    const double x1 = unscale<double>(p1.x()), y1 = unscale<double>(p1.y());
                    const double c = x0 * y1 - x1 * y0;
                    a += c;
                    cx += (x0 + x1) * c;
                    cy += (y0 + y1) * c;
                    ixx += (x0 * x0 + x0 * x1 + x1 * x1) * c;
                    iyy += (y0 * y0 + y0 * y1 + y1 * y1) * c;
                    ixy += (x0 * y1 + 2. * x0 * y0 + 2. * x1 * y1 + x1 * y0) * c;
                }
            };
            // Contours are CCW and holes CW, so holes subtract.
            add(island.contour);
            for (const Polygon& hole : island.holes)
                add(hole);
            a *= 0.5;
            if (a <= 0.)
                continue;
            cx /= 6. * a;
            cy /= 6. * a;
            sxx += ixx / 12. - a * cx * cx;
            syy += iyy / 12. - a * cy * cy;
            sxy += ixy / 24. - a * cx * cy;
        }
    // A round or square footprint has no long axis; keep a fixed direction instead of rounding noise.
    if (std::hypot(sxx - syy, 2. * sxy) < 1e-3 * (sxx + syy))
        return 0.;
    return 0.5 * std::atan2(2. * sxy, sxx - syy) + 0.5 * M_PI;
}

// Keeps only the fin columns on the side the object leans toward: the side of its bed contact
// that its center of mass lies on. Those fins hold the object up in compression; a tine is too
// thin to hold it in tension from the other side. A balanced object keeps its largest column's side.
static void keep_lean_side(const LayerPtrs& layers, std::vector<Polygons>& regions)
{
    const std::optional<FinLean> object = object_lean(layers);
    if (!object)
        return;

    // Fin regions only narrow going up, so the lowest one holds every fin column.
    auto lowest = std::find_if(regions.begin(), regions.end(), [](const Polygons& region) { return !region.empty(); });
    if (lowest == regions.end())
        return;
    const ExPolygons columns = union_ex(*lowest);
    if (columns.empty())
        return;
    auto column_offset       = [&base = object->base](const ExPolygon& column) {
        double area;
        return Vec2d(area_centroid({column}, area) - base);
    };

    Vec2d lean = object->lean;
    if (lean.norm() < scale_(kMinLean)) {
        const auto largest = std::max_element(columns.begin(), columns.end(),
                                              [](const ExPolygon& a, const ExPolygon& b) { return a.area() < b.area(); });
        lean               = column_offset(*largest);
    }
    Polygons keep;
    for (const ExPolygon& column : columns)
        if (column_offset(column).dot(lean) > 0.)
            append(keep, to_polygons(column));
    if (keep.empty())
        return;
    // Upper regions sit inside the bed-level columns; allow for rounding at their edges.
    keep = offset(keep, float(scale_(0.1)));
    for (Polygons& region : regions)
        if (!region.empty())
            region = intersection(region, keep);
}

// How far a layer may reach past the one below before tree and normal supports support it: the
// lower layer offset they use for the support threshold angle, or for the threshold overlap when the
// angle is 0 (auto).
static float overhang_threshold_offset(const PrintObjectConfig& config, const Layer& lower_layer)
{
    const int threshold = config.support_threshold_angle.value;
    if (threshold > 0)
        // +1 makes the threshold inclusive, as in tree and normal supports.
        return scaled<float>(lower_layer.height / std::tan(std::min(threshold + 1, 89) * M_PI / 180.));
    float width = 0.f;
    for (const LayerRegion* layerm : lower_layer.regions())
        width += layerm->flow(frExternalPerimeter).scaled_width();
    width /= float(std::max<size_t>(1, lower_layer.region_count()));
    return width - float(scale_(config.support_threshold_overlap.get_abs_value(unscale_(width))));
}

bool generate_fin_support(PrintObject& object, Polygons* reserved_columns)
{
    const PrintObjectConfig& config         = object.config();
    const SlicingParameters& slicing_params = object.slicing_parameters();
    SupportParameters support_params(object);
    const LayerPtrs& layers = object.layers();
    const size_t num_layers = layers.size();
    if (num_layers < 2)
        return false;

    BOOST_LOG_TRIVIAL(info) << "Fin support - start";

    // Object outlines with holes filled: fins never stand inside the object.
    std::vector<Polygons> outlines(num_layers);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
            for (const ExPolygon& expoly : layers[i]->lslices)
                outlines[i].emplace_back(expoly.contour);
    });

    std::vector<Polygons> blockers = object.slice_support_blockers();
    object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers);
    const bool manual = config.support_type.value == stFinsManual;
    // Enforced areas: where Fins (manual) stand, and overhangs the supplemental supports take at any slope.
    std::vector<Polygons> enforcers;
    if (manual || reserved_columns != nullptr) {
        enforcers = object.slice_support_enforcers();
        object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers);
    }
    if (manual && std::all_of(enforcers.begin(), enforcers.end(), [](const Polygons& layer) { return layer.empty(); }))
        return false;

    // 1. Fin regions, only for the layers up to the fin height.
    const coordf_t fin_height = config.support_fin_height.value;
    const coordf_t top_gap    = config.support_top_z_distance.value;
    size_t num_fin_layers     = num_layers;
    if (fin_height > 0.)
        while (num_fin_layers > 0 && layers[num_fin_layers - 1]->print_z > fin_height + EPSILON)
            --num_fin_layers;
    // `shadow` is the projection of everything above a layer.
    std::vector<Polygons> shadow(num_fin_layers);
    {
        Polygons above;
        for (size_t i = num_layers; i-- > 0;) {
            if (i < num_fin_layers)
                shadow[i] = above;
            if (!outlines[i].empty())
                above = union_(above, outlines[i]);
            if (object.print()->canceled())
                return false;
        }
    }
    // A fin stands on the bed, so it may not pass through anything at or under its layer:
    // the object (`below`) or a support blocker (`blocked`).
    std::vector<Polygons> regions(num_layers);
    {
        Polygons below, blocked;
        for (size_t i = 0; i < num_fin_layers; ++i) {
            below = union_(below, outlines[i]);
            if (i < blockers.size() && !blockers[i].empty())
                blocked = union_(blocked, blockers[i]);
            const Layer& layer = *layers[i];
            if (!shadow[i].empty()) {
                Polygons keep_out = below;
                // Stop fins support_top_z_distance below any part of the object that hangs over them.
                for (size_t j = i + 1; j < num_layers && layers[j]->bottom_z() < layer.print_z + top_gap - EPSILON; ++j)
                    append(keep_out, outlines[j]);
                const float gap = float(scale_(i == 0 ? support_params.gap_xy_first_layer : support_params.gap_xy));
                regions[i]      = diff(shadow[i], offset(union_(keep_out), gap));
                if (!blocked.empty())
                    regions[i] = diff(regions[i], blocked);
            }
            Polygons().swap(shadow[i]);
            if (object.print()->canceled())
                return false;
        }
    }

    // Fins (manual): only under the enforced areas at or above each layer, projected down to the bed.
    if (manual) {
        Polygons enforced;
        for (size_t i = num_layers; i-- > 0;) {
            if (i < enforcers.size() && !enforcers[i].empty())
                enforced = union_(enforced, enforcers[i]);
            if (!regions[i].empty())
                regions[i] = enforced.empty() ? Polygons() : intersection(regions[i], enforced);
            if (object.print()->canceled())
                return false;
        }
    }

    // With Fins (manual) the enforcers already choose the side.
    if (config.support_fin_lean_side_only.value && !manual)
        keep_lean_side(layers, regions);

    // Give supplemental supports complete vertical columns wherever a real overhang
    // cannot be reached by a fin. Reserving whole columns keeps both generators from
    // cutting the other one's load-bearing path to the bed. A real overhang is one the
    // supplemental supports would support (TigerSlicer): steeper slopes, such as the 40 degree
    // sides of a part tilted 40 degrees, would get no support from them, only a gap cut through the fins.
    if (reserved_columns != nullptr) {
        reserved_columns->clear();
        Polygons uncovered;
        size_t support_idx        = 0;
        const float bead          = float(support_params.support_material_flow.scaled_spacing());
        const float any_slope     = 0.5f * bead;
        const int   forced_layers = config.enforce_support_layers.value;
        for (size_t i = 1; i < num_layers; ++i) {
            const double support_z = layers[i]->bottom_z() - top_gap;
            while (support_idx + 1 < i && layers[support_idx + 1]->print_z <= support_z + EPSILON)
                ++support_idx;
            const bool enforced = i < enforcers.size() && !enforcers[i].empty();
            // Manual supplemental supports only support enforced overhangs; leave the rest to the fins.
            if (manual && !enforced)
                continue;
            // Enforced overhangs, and all overhangs in the first enforce_support_layers layers, are
            // supported whatever their slope, as tree and normal supports do.
            const float lower_offset = manual || int(i) < forced_layers ? any_slope : overhang_threshold_offset(config, *layers[i - 1]);
            Polygons overhang        = diff(outlines[i], offset(outlines[i - 1], lower_offset));
            if (manual)
                overhang = intersection(overhang, offset(enforcers[i], bead));
            else if (enforced && lower_offset > any_slope)
                append(overhang, intersection(diff(outlines[i], offset(outlines[i - 1], any_slope)), offset(enforcers[i], bead)));
            if (layers[support_idx]->print_z <= support_z + EPSILON)
                overhang = diff(overhang, offset(regions[support_idx], float(scale_(support_params.gap_xy)) + bead));
            append(uncovered, overhang);
        }
        uncovered = offset(union_(uncovered), 0.5f * bead);
        for (Polygons& region : regions)
            region = diff(region, uncovered);
    }

    BoundingBox bbox;
    for (const Polygons& region : regions)
        if (!region.empty())
            bbox.merge(get_extents(region));
    if (!bbox.defined)
        return false;

    // 2.-3. Clip the regions to fins.
    const coord_t spacing = support_params.support_material_flow.scaled_spacing();
    // Fins are filled with concentric loops, two lines per loop. Round the thickness to whole loops:
    // a leftover narrower than a line is not filled and leaves a slot down the middle of the fin.
    const coord_t thickness = 2 * spacing *
                              std::max<coord_t>(1,
                                                coord_t(std::lround(scaled<double>(config.support_fin_thickness.value) / (2. * spacing))));
    const coord_t pitch     = fin_pitch(config.support_fin_spacing, thickness);
    // A tine is one bead out and one bead back: two extrusions wide, never wider than the fin.
    const coord_t tine_width = std::min(thickness, 2 * spacing);
    const double angle       = fin_angle(layers, regions);
    // Regions only narrow going up, so the lowest one holds every fin column.
    const Polygons footprint = *std::find_if(regions.begin(), regions.end(), [](const Polygons& region) { return !region.empty(); });
    const coord_t fin_offset  = fin_strips_offset(footprint, angle);
    Polygons fins             = fin_strips(bbox, angle, pitch, thickness, fin_offset);
    Polygons feet             = fin_strips(bbox, angle, pitch, coord_t(kBaseWidthRatio * thickness), fin_offset);
    Polygons tine_strips      = fin_strips(bbox, angle, pitch, tine_width, fin_offset);
    const BoundingBox rbox   = rotated_extents(bbox, angle, thickness);
    // Pieces narrower than half an extrusion cannot be printed; drop them.
    const float min_width = 0.25f * float(spacing);
    // Scaffold: cross fins at right angles to the main fins. Tines stay on the main fins only.
    const double cross_angle     = angle + 0.5 * M_PI;
    const coord_t cross_pitch    = config.support_fin_cross_spacing.value > 0. ?
                                       scaled<coord_t>(config.support_fin_cross_spacing.value) + thickness :
                                       0;
    const BoundingBox cross_rbox = rotated_extents(bbox, cross_angle, thickness);
    const Polygons main_fins     = fins;
    // Holes go through the main fins only: cross fins are the bracing.
    Polygons holed_fins = main_fins;
    Polygons cross_fins;
    if (cross_pitch > 0) {
        const coord_t cross_offset = fin_strips_offset(footprint, cross_angle);
        cross_fins                 = fin_strips(bbox, cross_angle, cross_pitch, thickness, cross_offset);
        append(fins, cross_fins);
        append(feet, fin_strips(bbox, cross_angle, cross_pitch, coord_t(kBaseWidthRatio * thickness), cross_offset));
    }
    // A column narrower than the fin spacing may fall between two grid fins, or be grazed by one too
    // thin to print: give it its own fin through its center, fixed for all layers so the fin stands on
    // itself all the way down to the bed.
    for (const ExPolygon& island : union_ex(footprint))
        if (opening(intersection(to_polygons(island), main_fins), min_width).empty()) {
            Point c = island.contour.centroid();
            c.rotate(-angle);
            fins.emplace_back(fin_strip(rbox, angle, c.y(), thickness));
            feet.emplace_back(fin_strip(rbox, angle, c.y(), coord_t(kBaseWidthRatio * thickness)));
            tine_strips.emplace_back(fin_strip(rbox, angle, c.y(), tine_width));
            holed_fins.emplace_back(fin_strip(rbox, angle, c.y(), thickness));
            if (cross_pitch > 0) {
                // Brace it with a cross fin through the same center.
                Point cc = island.contour.centroid();
                cc.rotate(-cross_angle);
                fins.emplace_back(fin_strip(cross_rbox, cross_angle, cc.y(), thickness));
                cross_fins.emplace_back(fin_strip(cross_rbox, cross_angle, cc.y(), thickness));
                feet.emplace_back(fin_strip(cross_rbox, cross_angle, cc.y(), coord_t(kBaseWidthRatio * thickness)));
            }
        }
    fins                             = union_(fins);
    feet                             = union_(feet);
    tine_strips                      = union_(tine_strips);
    holed_fins                       = union_(holed_fins);
    cross_fins                       = union_(cross_fins);
    const bool holes                 = config.support_fin_holes.value;
    const std::vector<bool> tine_row = fin_tine_layers(object);
    const float depth                = float(scale_(config.support_fin_tine_depth.value));

    const int interface_layers = config.support_fin_interface_layers.value;
    // Orient line-based interfaces across the fins so they can bridge from fin to fin.
    // Keep the normal interface filament, pattern, spacing and ironing settings.
    support_params.support_style   = smsGrid; // grid style prints interfaces at interface_angle
    support_params.interface_angle = float(angle + 0.5 * M_PI);
    const coordf_t top_gap_below   = config.support_top_z_distance.value;

    std::vector<Polygons> shaped(num_layers);
    std::vector<Polygons> interfaces(num_layers);
    std::vector<Polygons> tine_contacts(num_layers);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            if (object.print()->canceled())
                return;
            if (regions[i].empty())
                continue;
            const Layer& layer = *layers[i];
            const bool foot    = layer.bottom_z() < kBaseHeight - EPSILON;
            Polygons out       = opening(intersection(regions[i], foot ? feet : fins), min_width);

            // 3b. Holes, above the foot and its frame.
            if (holes && !out.empty() && layer.bottom_z() > kBaseHeight + kHoleFrame - EPSILON &&
                (fin_height <= 0. || layer.print_z < fin_height - kHoleFrame + EPSILON)) {
                const float frame = float(scale_(kHoleFrame));
                // The ends of each fin stay solid: erode the region along the fins only, so a fin running
                // close to the side of the region keeps its holes.
                Polygons interior = regions[i];
                for (const double f : {-1., -0.5, 0.5, 1.}) {
                    Polygons shifted = regions[i];
                    const Point shift(coord_t(f * frame * std::cos(angle)), coord_t(f * frame * std::sin(angle)));
                    for (Polygon& poly : shifted)
                        poly.translate(shift);
                    interior = intersection(interior, shifted);
                }
                // ...and so does its top edge, for kHoleFrame under the object (and under an interface deck).
                if (!interior.empty()) {
                    Polygons above;
                    const double reach = layer.print_z + top_gap_below + interface_layers * layer.height + kHoleFrame;
                    for (size_t j = i + 1; j < num_layers && layers[j]->bottom_z() < reach - EPSILON; ++j)
                        append(above, outlines[j]);
                    if (!above.empty())
                        // Only the XY gap sideways: reaching kHoleFrame up already keeps a sloping top edge
                        // that wide, and fin ends are kept by the erosion above.
                        interior = diff(interior, offset(union_(above), float(scale_(support_params.gap_xy))));
                }
                if (!interior.empty()) {
                    Polygons cut = intersection(intersection(fin_hole_bands(rbox, angle, layer.print_z - 0.5 * layer.height), holed_fins),
                                                interior);
                    if (!cross_fins.empty())
                        cut = diff(cut, cross_fins);
                    // No hairline slots: near a diamond's tip or beside the frame a hole is too narrow to
                    // print as a gap, and would only split the fin into more pieces.
                    cut = opening(cut, float(spacing));
                    if (!cut.empty())
                        out = opening(diff(out, cut), min_width);
                }
            }

            // 4. Tines. The first object layer, which sits on the bed, never gets them.
            if (!out.empty() && tine_row[i]) {
                const float gap              = float(scale_(support_params.gap_xy));
                const Polygons object_slices = to_polygons(layer.lslices);
                // Ring from `depth` inside the object surface out to just past the XY gap.
                const Polygons ring      = diff(offset(object_slices, gap + float(spacing)), offset(object_slices, -depth));
                Polygons tines           = intersection(intersection(tine_strips, ring), offset(out, gap + depth + float(spacing)));
                const Polygons fin_touch = offset(out, float(SCALED_EPSILON));
                tines.erase(std::remove_if(tines.begin(), tines.end(),
                                           [&](const Polygon& tine) {
                                               // A tine must bridge from its fin into the object; one that misses either end
                                               // (e.g. a short piece cut off at a corner of the ring) prints in mid-air.
                                               if (intersection(Polygons{tine}, fin_touch).empty() ||
                                                   intersection(Polygons{tine}, object_slices).empty())
                                                   return true;
                                               // A fin running along the object surface would leave a long strip inside the ring:
                                               // a welded seam, not a tine. Crossings down to about 20 degrees are kept.
                                               Polygon along = tine;
                                               along.rotate(-angle);
                                               const BoundingBox b = along.bounding_box();
                                               return double(b.max.x() - b.min.x()) > 3. * (gap + depth + float(spacing));
                                           }),
                            tines.end());
                if (!tines.empty()) {
                    out              = union_(out, tines);
                    tine_contacts[i] = std::move(tines);
                }
            }

            // 5. Interface: the part of the fin region within interface_layers layers under the object
            // (below the top Z gap) becomes an interface deck; the fins stop under it.
            if (interface_layers > 0) {
                Polygons over;
                for (size_t j = i + 1;
                     j < num_layers && layers[j]->bottom_z() < layer.print_z + top_gap_below + interface_layers * layer.height - EPSILON;
                     ++j)
                    append(over, outlines[j]);
                Polygons deck = opening(intersection(regions[i], over), min_width);
                if (!deck.empty()) {
                    out           = diff(out, deck);
                    interfaces[i] = std::move(deck);
                }
            }
            shaped[i] = std::move(out);
        }
    });
    if (object.print()->canceled())
        return false;
    if (reserved_columns != nullptr) {
        for (size_t i = 0; i < num_layers; ++i)
            if (!shaped[i].empty() || !interfaces[i].empty()) {
                append(*reserved_columns, regions[i]);
                append(*reserved_columns, shaped[i]);
            }
        *reserved_columns = offset(union_(*reserved_columns), 0.5f * float(spacing));
    }
    regions.clear();

    // A fin much taller than it is long is a free-standing blade that can wobble or snap mid-print,
    // e.g. under a small feature high above the bed. Measure each fin above the foot.
    bool slender = false;
    {
        size_t first = 0;
        while (first < num_layers && (shaped[first].empty() || layers[first]->bottom_z() < kBaseHeight - EPSILON))
            ++first;
        if (first < num_layers)
            for (const ExPolygon& fin : union_ex(shaped[first])) {
                Polygon along = fin.contour;
                along.rotate(-angle);
                const BoundingBox b = along.bounding_box();
                // A braced fin is stiff in both directions, so its shorter side is the one that counts.
                const coord_t xlen    = b.max.x() - b.min.x();
                const coord_t ylen    = b.max.y() - b.min.y();
                const coord_t length  = cross_pitch > 0 && ylen > 2 * thickness ? std::min(xlen, ylen) : xlen;
                const Polygons column = offset(fin, float(spacing));
                size_t top            = first;
                while (top + 1 < num_layers && !intersection(shaped[top + 1], column).empty())
                    ++top;
                if (layers[top]->print_z > kSlenderRatio * unscale<double>(length))
                    slender = true;
            }
    }

    // Hand the fins to the common support pipeline as base layers, one per object layer.
    SupportGeneratorLayerStorage layer_storage;
    SupportGeneratorLayersPtr base_layers;
    SupportGeneratorLayersPtr interface_layers_out;
    for (size_t i = 0; i < num_layers; ++i)
        if (!interfaces[i].empty()) {
            SupportGeneratorLayer& support_layer = layer_storage.allocate_unguarded(SupporLayerType::TopInterface);
            support_layer.print_z                = layers[i]->print_z;
            support_layer.bottom_z               = layers[i]->bottom_z();
            support_layer.height                 = layers[i]->height;
            support_layer.polygons               = std::move(interfaces[i]);
            // The lowest deck layer spans the gaps between fins.
            support_layer.bridging = i == 0 || interface_layers_out.empty() ||
                                     interface_layers_out.back()->print_z < layers[i - 1]->print_z - EPSILON;
            interface_layers_out.push_back(&support_layer);
        }
    for (size_t i = 0; i < num_layers; ++i)
        if (!shaped[i].empty()) {
            SupportGeneratorLayer& support_layer = layer_storage.allocate_unguarded(SupporLayerType::Base);
            support_layer.print_z                = layers[i]->print_z;
            support_layer.bottom_z               = layers[i]->bottom_z();
            support_layer.height                 = layers[i]->height;
            support_layer.polygons               = std::move(shaped[i]);
            if (!tine_contacts[i].empty())
                support_layer.fin_contact_polygons = std::make_unique<Polygons>(std::move(tine_contacts[i]));
            base_layers.push_back(&support_layer);
        }
    if (base_layers.empty() && interface_layers_out.empty())
        return false;

    const SupportGeneratorLayersPtr empty;
    generate_support_layers(object, empty, empty, empty, base_layers, interface_layers_out, empty);
    generate_support_toolpaths(object.support_layers(), config, support_params, slicing_params, empty, empty, empty, base_layers,
                               interface_layers_out, empty);

    BOOST_LOG_TRIVIAL(info) << "Fin support - end";
    return slender;
}

} // namespace Slic3r
