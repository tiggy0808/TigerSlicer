#include "SmallFeatures.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"

#include <algorithm>
#include <cmath>
#include <map>

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

} // namespace Slic3r::SmallFeatures
