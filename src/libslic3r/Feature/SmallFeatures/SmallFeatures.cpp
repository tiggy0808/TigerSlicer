#include "SmallFeatures.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry/MedialAxis.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

#include <tbb/parallel_for.h>

namespace Slic3r::SmallFeatures {

Points path_points(const Polyline3 &polyline, double max_error)
{
    Points points;
    points.reserve(polyline.points.size());
    for (const Point3 &p : polyline.points)
        points.emplace_back(p.x(), p.y());
    if (polyline.fitting_result.empty())
        return points;

    Points out;
    for (const PathFittingData &fit : polyline.fitting_result) {
        if (fit.end_point_index >= points.size())
            break;
        if (out.empty())
            out.push_back(points[fit.start_point_index]);
        const bool arc = fit.path_type != EMovePathType::Linear_move && fit.arc_data.is_valid();
        if (arc) {
            const ArcSegment &segment = fit.arc_data;
            // The angle per segment that keeps the chord within max_error of the arc.
            const double step  = 2. * std::acos(std::max(-1., 1. - max_error / std::max(segment.radius, max_error)));
            const int    steps = std::max(1, int(std::ceil(std::abs(segment.angle_radians) / std::max(step, 1e-3))));
            for (int i = 1; i < steps; ++i) {
                const double angle = segment.polar_start_theta + segment.angle_radians * i / steps;
                out.emplace_back(segment.center.x() + coord_t(std::lround(segment.radius * std::cos(angle))),
                                 segment.center.y() + coord_t(std::lround(segment.radius * std::sin(angle))));
            }
            out.push_back(points[fit.end_point_index]);
        } else {
            for (size_t i = fit.start_point_index + 1; i <= fit.end_point_index; ++i)
                out.push_back(points[i]);
        }
    }
    return out;
}

static void append_coverage(const ExtrusionEntity &entity, Polygons &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            append_coverage(*child, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            append_coverage(path, out);
        for (const ExtrusionPath &path : loop->dead_ends)
            append_coverage(path, out);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multi->paths)
            append_coverage(path, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        if (path->mm3_per_mm <= 0. || path->polyline.points.size() < 2)
            return;
        const Polyline line(path_points(path->polyline, scaled<double>(0.002)));
        append(out, offset(line, float(scale_(0.5 * path->width)), ClipperLib::jtRound, DefaultLineMiterLimit, ClipperLib::etOpenRound));
    }
}

Polygons printed_coverage(const Layer &layer)
{
    Polygons covered;
    for (const LayerRegion *region : layer.regions()) {
        append_coverage(region->perimeters, covered);
        append_coverage(region->thin_fills, covered);
        append_coverage(region->fills, covered);
        append(covered, to_polygons(region->fill_expolygons));
    }
    return union_(covered);
}

static std::vector<DeviationArea> deviation_areas(const ExPolygons &areas, double tolerance)
{
    std::vector<DeviationArea> out;
    for (ExPolygon &area : opening_ex(areas, float(scale_(0.5 * tolerance)))) {
        const double perimeter = unscale<double>(area.contour.length());
        const double magnitude = perimeter > 0. ? 2. * unscale<double>(unscale<double>(area.area())) / perimeter : 0.;
        // The opening keeps a sharp corner the round end of a bead leaves, a speck thinner than the tolerance overall.
        if (magnitude >= tolerance)
            out.push_back({std::move(area), magnitude});
    }
    return out;
}

LayerDeviation layer_deviation(const Layer &layer, double tolerance)
{
    const Polygons covered = printed_coverage(layer);
    LayerDeviation deviation;
    deviation.undershoot = deviation_areas(diff_ex(layer.lslices, covered), tolerance);
    deviation.overshoot  = deviation_areas(diff_ex(covered, layer.lslices), tolerance);
    return deviation;
}

std::vector<PrintedLayerDeviation> print_deviation(const Print &print)
{
    std::vector<const Layer *> layers;
    for (const PrintObject *object : print.objects())
        for (const Layer *layer : object->layers())
            layers.push_back(layer);
    std::vector<LayerDeviation> per_layer(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&layers, &per_layer, &print](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end() && !print.canceled(); ++i)
            per_layer[i] = layer_deviation(*layers[i]);
    });
    if (print.canceled())
        return {};

    // Layers of different objects at the same height (to 0.1 um) share an entry.
    std::map<long long, PrintedLayerDeviation> by_height;
    for (size_t i = 0; i < layers.size(); ++i) {
        const Layer           &layer = *layers[i];
        PrintedLayerDeviation &out   = by_height[std::llround(layer.print_z * 1e4)];
        out.print_z                  = layer.print_z;
        for (const PrintInstance &instance : layer.object()->instances()) {
            auto place = [&instance](const std::vector<DeviationArea> &from, std::vector<DeviationArea> &to) {
                for (DeviationArea area : from) {
                    area.shape.translate(instance.shift);
                    to.push_back(std::move(area));
                }
            };
            place(per_layer[i].undershoot, out.deviation.undershoot);
            place(per_layer[i].overshoot, out.deviation.overshoot);
        }
    }
    std::vector<PrintedLayerDeviation> out;
    out.reserve(by_height.size());
    for (auto &[height, layer] : by_height)
        out.push_back(std::move(layer));
    return out;
}

ExPolygons features_below_width(const ExPolygons &slices, coord_t min_width)
{
    ExPolygons features;
    for (ExPolygon &thin : diff_ex(slices, opening_ex(slices, 0.5f * float(min_width)))) {
        const BoundingBox box = get_extents(thin);
        if (std::max(box.size().x(), box.size().y()) >= min_width)
            features.push_back(std::move(thin));
    }
    return features;
}

namespace {

// A line to print a thin part on, out to the part's outline, with the part's width across it in mm: the widest, the
// mean, and the narrowest that the model allows. A line that starts on a wall loop names it.
struct FeatureLine
{
    Points         points;
    double         max_width;
    double         mean_width;
    double         min_width;
    ExtrusionLoop *wall = nullptr;
};

} // namespace

// The longest line along the middle of `uncovered`, for a part no wall reaches. ExPolygon::medial_axis() is not used:
// it drops lines shorter than twice their width.
static std::optional<FeatureLine> center_line(const ExPolygon &uncovered, coord_t max_width)
{
    // What the beads leave uncovered is outlined by their round ends, points a few micrometres apart that the Voronoi
    // diagram cannot always resolve.
    ExPolygons simplified = uncovered.simplify(scaled<double>(0.005));
    if (simplified.empty())
        return std::nullopt;
    const ExPolygon &piece = *std::max_element(simplified.begin(), simplified.end(),
                                               [](const ExPolygon &a, const ExPolygon &b) { return a.area() < b.area(); });
    ThickPolylines lines;
    try {
        Geometry::MedialAxis axis(scaled<double>(kDeviationTolerance), double(max_width), piece);
        axis.build(&lines);
    } catch (const Slic3r::Exception &error) {
        BOOST_LOG_TRIVIAL(warning) << "Small features: no line along a feature at " << unscale(piece.contour.first_point()).transpose()
                                   << ": " << error.what();
        return std::nullopt;
    }
    const ThickPolyline *longest = nullptr;
    for (const ThickPolyline &line : lines)
        if (line.points.size() >= 2 && line.width.size() + 2 == 2 * line.points.size() && (longest == nullptr || line.length() > longest->length()))
            longest = &line;
    if (longest == nullptr)
        return std::nullopt;

    FeatureLine out{longest->points, 0., 0., std::numeric_limits<double>::max()};
    // Free ends run on along their last segment to the outline.
    auto extend = [&piece, max_width](Point &end, const Point &inner) {
        if (piece.on_boundary(end, SCALED_EPSILON))
            return;
        const Vec2d a = inner.cast<double>(), b = end.cast<double>();
        const Vec2d past = b + (b - a).normalized() * double(max_width);
        Point       hit;
        if (piece.contour.intersection(Line(end, past.cast<coord_t>()), &hit))
            end = hit;
    };
    if (longest->endpoints.first)
        extend(out.points.front(), out.points[1]);
    if (longest->endpoints.second)
        extend(out.points.back(), out.points[out.points.size() - 2]);

    // Widths come two per segment, at its start and end.
    double weighted = 0., length = 0.;
    for (size_t i = 0; i + 1 < longest->points.size(); ++i) {
        const double segment = (longest->points[i + 1] - longest->points[i]).cast<double>().norm();
        weighted += segment * 0.5 * (longest->width[2 * i] + longest->width[2 * i + 1]);
        length += segment;
    }
    out.mean_width = length > 0. ? unscale<double>(weighted / length) : 0.;
    for (size_t i = 1; i + 1 < longest->width.size(); ++i)
        out.min_width = std::min(out.min_width, unscale<double>(longest->width[i]));
    if (out.min_width == std::numeric_limits<double>::max())
        out.min_width = out.mean_width;
    out.max_width = unscale<double>(*std::max_element(longest->width.begin(), longest->width.end()));
    return out;
}

// The length (scaled) of the chord across the area outlined by `edges` through `at`, along `dir` (a unit vector), at
// most `reach` either side of `at`.
static double chord(const Lines &edges, const Vec2d &at, const Vec2d &dir, double reach)
{
    double below = reach, above = reach;
    for (const Line &edge : edges) {
        const Vec2d  a     = edge.a.cast<double>();
        const Vec2d  along = edge.b.cast<double>() - a;
        const double denom = cross2(dir, along);
        if (std::abs(denom) < EPSILON)
            continue;
        const Vec2d  to_edge = a - at;
        const double t       = cross2(to_edge, along) / denom;
        const double s       = cross2(to_edge, dir) / denom;
        if (s < 0. || s > 1.)
            continue;
        if (t >= 0.)
            above = std::min(above, t);
        else
            below = std::min(below, -t);
    }
    return below + above;
}

// A part that hangs off a wall loop gets a straight line from the loop (at `foot`, its point nearest the part) out
// through the part's middle to where it leaves the part: from where a wall turns back below a tooth tip, out to the
// tip. Its widths are the part's chords across the line, and the model's for the narrowest. None when the line enters
// the part further than `max_lead` (scaled) from the loop.
static std::optional<FeatureLine> wall_line(const ExPolygon &piece, const ExPolygons &model, ExtrusionLoop &wall, const Point &foot,
                                            coord_t max_width, double max_lead)
{
    const Vec2d  from      = foot.cast<double>();
    const Vec2d  through   = piece.contour.centroid().cast<double>();
    const double to_middle = (through - from).norm();
    if (to_middle < SCALED_EPSILON)
        return std::nullopt;
    const Vec2d dir = (through - from) / to_middle;

    // The stretch of the line inside the part: the one through its middle, or else the longest.
    const Polyline ray(Points{foot, (from + dir * (to_middle + 4. * double(max_width))).cast<coord_t>()});
    double         enter = -1., leave = -1.;
    for (const Polyline &inside : intersection_pl(Polylines{ray}, ExPolygons{piece})) {
        const double a  = (inside.first_point().cast<double>() - from).dot(dir);
        const double b  = (inside.last_point().cast<double>() - from).dot(dir);
        const double lo = std::min(a, b), hi = std::max(a, b);
        if ((lo <= to_middle && to_middle <= hi) || (!(enter <= to_middle && to_middle <= leave) && hi - lo > leave - enter)) {
            enter = lo;
            leave = hi;
        }
    }
    if (leave <= enter || enter > max_lead)
        return std::nullopt;

    // Chords across the line, every 20 um along the part.
    BoundingBox box(foot, foot);
    box.merge((from + dir * leave).cast<coord_t>());
    box.offset(2 * max_width);
    const Lines piece_edges = piece.lines();
    const Lines model_edges = to_lines(intersection_ex(model, ExPolygons{ExPolygon(box.polygon())}));
    const Vec2d across(-dir.y(), dir.x());
    const int   samples = std::clamp(int(std::ceil(unscale<double>(leave - enter) / 0.02)), 5, 100);
    FeatureLine out{Points{foot, (from + dir * leave).cast<coord_t>()}, 0., 0., std::numeric_limits<double>::max(), &wall};
    int         counted = 0;
    for (int i = 0; i < samples; ++i) {
        const Vec2d at = from + dir * (enter + (leave - enter) * (i + 0.5) / samples);
        if (!piece.contains(at.cast<coord_t>()))
            continue;
        const double width = unscale<double>(chord(piece_edges, at, across, double(max_width)));
        out.max_width      = std::max(out.max_width, width);
        out.mean_width += width;
        out.min_width = std::min(out.min_width, unscale<double>(chord(model_edges, at, across, double(max_width))));
        ++counted;
    }
    if (counted == 0)
        return std::nullopt;
    out.mean_width /= counted;
    return out;
}

// The loops of `collection` and of the collections in it.
static void collect_loops(ExtrusionEntityCollection &collection, std::vector<ExtrusionLoop *> &loops)
{
    for (ExtrusionEntity *entity : collection.entities) {
        if (auto *child = dynamic_cast<ExtrusionEntityCollection *>(entity))
            collect_loops(*child, loops);
        else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity))
            loops.push_back(loop);
    }
}

// Moves the end of `points` (the back, or else the front) `pull` (scaled) back along its last segment, or on past it
// when `pull` is negative. False when the segment is too short to pull back that far.
static bool pull_end(Points &points, bool back, double pull)
{
    Point       &end   = back ? points.back() : points.front();
    const Point &inner = back ? points[points.size() - 2] : points[1];
    const Vec2d  dir   = (end - inner).cast<double>();
    const double len   = dir.norm();
    if (len <= 0. || len <= pull)
        return false;
    end = (end.cast<double>() - dir / len * pull).cast<coord_t>();
    return true;
}

void add_small_feature_lines(LayerRegion &layerm, const SurfaceCollection &slices, const ExPolygons &fill_area)
{
    const PrintRegionConfig &config = layerm.region().config();
    const SmallFeaturePolicy policy = config.small_feature_policy.value;
    if (policy == sfpDrop)
        return;

    const Flow   ext_flow  = layerm.flow(frExternalPerimeter);
    const double nozzle    = ext_flow.nozzle_diameter();
    const double min_width = config.small_feature_min_width.get_abs_value(nozzle);
    const double height    = layerm.layer()->height;

    const ExPolygons model = to_expolygons(slices.surfaces);
    Polygons         covered;
    append_coverage(layerm.perimeters, covered);
    append_coverage(layerm.thin_fills, covered);
    append(covered, to_polygons(fill_area));
    const ExPolygons uncovered = opening_ex(diff_ex(model, union_(covered)), float(scale_(0.5 * kDeviationTolerance)));
    if (uncovered.empty())
        return;
    // Only parts of the model narrower than two walls have features: an opening with mitered corners takes them away
    // and gives back every corner, so neither the corner a wall rounds off nor a sliver the walls leave between each
    // other is one.
    const ExPolygons thin = diff_ex(model, opening_ex(model, float(scale_(ext_flow.width()))));
    if (thin.empty())
        return;

    std::vector<ExtrusionLoop *> loops;
    collect_loops(layerm.perimeters, loops);
    ExtrusionEntityCollection standalone;
    const coord_t             max_width = scaled<coord_t>(2. * ext_flow.width());
    const double              max_lead  = scaled<double>(ext_flow.width() + kDeviationTolerance);

    for (const ExPolygon &piece : uncovered) {
        if (area(intersection_ex(ExPolygons{piece}, thin)) < 0.5 * piece.area())
            continue;
        // The wall loop nearest the part's middle.
        const Point    middle  = piece.contour.centroid();
        ExtrusionLoop *nearest = nullptr;
        Point          foot;
        double         best = std::numeric_limits<double>::max();
        for (ExtrusionLoop *loop : loops) {
            const auto   closest = loop->get_closest_path_and_point(middle, false);
            const double d       = (closest.foot_pt - middle).cast<double>().norm();
            if (d < best) {
                best    = d;
                nearest = loop;
                foot    = closest.foot_pt;
            }
        }
        std::optional<FeatureLine> line;
        if (nearest != nullptr)
            line = wall_line(piece, model, *nearest, foot, max_width, max_lead);
        if (!line)
            line = center_line(piece, max_width);
        // A part narrower than half the narrowest line would mostly be printed over the walls around it.
        if (!line || line->max_width < 0.5 * min_width)
            continue;
        // Inside: no wider than the model anywhere along the line, or the bead would cross its edge. Outside: as wide
        // as the part's widest, to cover all of it.
        const double width = policy == sfpInside   ? line->min_width :
                             policy == sfpBalanced ? std::max(line->mean_width, min_width) :
                                                     std::max(line->max_width, min_width);
        if (width < min_width)
            continue;
        // How far the free end stops short of the outline (mm; negative is past it), the bead reaching w/2 beyond.
        const double pull = policy == sfpInside   ? 0.5 * width + 0.5 * kDeviationTolerance :
                            // A round end of radius r leaves the least area wrong with its center r * sqrt(3) / 2 short.
                            policy == sfpBalanced ? 0.25 * std::sqrt(3.) * width :
                                                    0.5 * width - config.tip_overshoot.value;

        Points points = std::move(line->points);
        if (!pull_end(points, true, scaled<double>(pull)) || (line->wall == nullptr && !pull_end(points, false, scaled<double>(pull))))
            continue;

        ExtrusionPath path(line->wall != nullptr ? line->wall->paths.front().role() : erExternalPerimeter,
                           Flow(float(width), float(height), float(nozzle)).mm3_per_mm() * config.thin_wall_flow_ratio.value,
                           float(width), float(height));
        path.polyline = Polyline3(Polyline(std::move(points)));
        if (line->wall != nullptr)
            line->wall->dead_ends.push_back(std::move(path));
        else
            standalone.append(path);
    }
    if (!standalone.empty())
        layerm.perimeters.append(standalone);
}

} // namespace Slic3r::SmallFeatures
