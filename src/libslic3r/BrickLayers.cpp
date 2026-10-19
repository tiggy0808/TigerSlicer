#include "BrickLayers.hpp"

#include "AABBTreeLines.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {

namespace {

// The bottom layer sits on the bed or raft and is never lifted.
constexpr size_t FIRST_BRICK_LAYER = 1;
// A lifted wall continues on the next layer when that layer has a lifted wall
// within this fraction of the wall spacing. Vertical walls match almost
// exactly. On a slope the walls drift sideways, and past this distance the
// lifted wall would press into a wall above that is not lifted, so it ends.
constexpr double NEIGHBOUR_TOLERANCE_RATIO = 0.25;
// Distance between the points at which a wall is compared with its neighbours.
constexpr double SAMPLE_STEP_MM = 0.25;
// Shorter pieces take the class of a neighbouring piece, so a wall does not
// hop up and down over a corner or a short gap.
constexpr double MIN_PIECE_LENGTH_MM = 2.0;
// Where a piece ends just inside a path, the sliver left on that path joins its neighbour.
constexpr double MIN_SLIVER_LENGTH_MM = 0.1;

// Whether a piece of wall has a lifted wall right below and right above it.
enum BrickNeighbours : unsigned char {
    bnNone  = 0,
    bnBelow = 1,
    bnAbove = 2,
};

struct Piece
{
    // Distances along the path, in scaled units.
    double        from;
    double        to;
    unsigned char neighbours;

    double length() const { return to - from; }
};

// Centrelines of the walls on one layer that may be lifted.
struct LayerWalls
{
    bool                                empty = true;
    AABBTreeLines::LinesDistancer<Line> lines;

    bool has_wall_near(const Point &point, double tolerance) const
    {
        return !empty && lines.distance_from_lines<false>(point) <= tolerance;
    }
};

bool region_uses_brick_layers(const PrintRegionConfig &config)
{
    // Z contouring sets wall Z and flow itself, which lifted walls would break.
    if (!config.brick_layers.value || config.zaa_enabled.value)
        return false;
    // Lifted walls go last in their extruder's pass over an island. If another
    // filament prints the outer wall or the infill, that pass may come later
    // and its nozzle would run into the lifted walls.
    const int wall_filament = config.inner_wall_filament_id.value;
    for (const int filament : {config.outer_wall_filament_id.value, config.sparse_infill_filament_id.value,
                               config.internal_solid_filament_id.value, config.top_surface_filament_id.value,
                               config.bottom_surface_filament_id.value})
        if (filament != wall_filament)
            return false;
    return true;
}

// The odd inner walls: inset 1, 3, 5, ... counted from the outer wall.
bool is_lift_candidate(const ExtrusionEntity &entity)
{
    return entity.inset_idx > 0 && entity.inset_idx % 2 == 1;
}

// Overhanging pieces keep their normal height.
bool is_liftable(const ExtrusionPath &path)
{
    return path.role() == erPerimeter;
}

// Calls fn(paths) with the paths of every odd inner wall in the collection.
template<typename Fn>
void for_each_candidate(ExtrusionEntityCollection &collection, Fn &fn)
{
    for (ExtrusionEntity *entity : collection.entities) {
        if (auto *child = dynamic_cast<ExtrusionEntityCollection *>(entity))
            for_each_candidate(*child, fn);
        else if (!is_lift_candidate(*entity))
            continue;
        else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity))
            fn(loop->paths);
        else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(entity))
            fn(multipath->paths);
    }
}

LayerWalls collect_candidate_walls(Layer &layer)
{
    Lines lines;
    auto  collect = [&lines](ExtrusionPaths &paths) {
        for (const ExtrusionPath &path : paths)
            if (is_liftable(path))
                append(lines, path.polyline.to_polyline().lines());
    };
    for (LayerRegion *layerm : layer.regions())
        if (region_uses_brick_layers(layerm->region().config()))
            for_each_candidate(layerm->perimeters, collect);

    LayerWalls walls;
    if (!lines.empty()) {
        walls.empty = false;
        walls.lines = AABBTreeLines::LinesDistancer<Line>(std::move(lines));
    }
    return walls;
}

// Distance from the first point to each point, in scaled units.
std::vector<double> distances_along(const Points3 &points)
{
    std::vector<double> along(points.size(), 0.);
    for (size_t i = 1; i < points.size(); ++i)
        along[i] = along[i - 1] + (points[i].head<2>() - points[i - 1].head<2>()).cast<double>().norm();
    return along;
}

Point3 point_along(const Points3 &points, const std::vector<double> &along, double distance)
{
    if (distance <= 0.)
        return points.front();
    if (distance >= along.back())
        return points.back();
    // along[i - 1] < distance <= along[i], so the segment has a length.
    const size_t i = size_t(std::lower_bound(along.begin(), along.end(), distance) - along.begin());
    if (along[i] == distance)
        return points[i];
    const double t = (distance - along[i - 1]) / (along[i] - along[i - 1]);
    const Vec3d  a = points[i - 1].cast<double>();
    const Vec3d  p = a + (points[i].cast<double>() - a) * t;
    return Point3(p.x(), p.y(), p.z());
}

// The part of the polyline between two distances along it.
Points3 sub_polyline(const Points3 &points, const std::vector<double> &along, double from, double to)
{
    Points3 out;
    out.emplace_back(point_along(points, along, from));
    for (size_t i = 1; i + 1 < points.size(); ++i)
        if (along[i] > from && along[i] < to && points[i] != out.back())
            out.emplace_back(points[i]);
    const Point3 end = point_along(points, along, to);
    if (end != out.back())
        out.emplace_back(end);
    return out;
}

// Consecutive liftable paths of one wall, measured as one line. Arachne splits
// a wall into many short paths where its width changes, and the whole wall
// has to be smoothed together, not each short path on its own.
struct WallRun
{
    std::vector<const Points3 *>     points;
    std::vector<std::vector<double>> along; // for each path
    std::vector<double>              start; // distance of each path's first point along the run
    double                           total = 0.;

    void add(const Points3 &path_points)
    {
        points.push_back(&path_points);
        along.push_back(distances_along(path_points));
        start.push_back(total);
        total += along.back().back();
    }

    Point3 point_at(double distance) const
    {
        // The last path that starts at or before the distance.
        const size_t next = size_t(std::upper_bound(start.begin(), start.end(), distance) - start.begin());
        const size_t k    = next == 0 ? 0 : next - 1;
        return point_along(*points[k], along[k], distance - start[k]);
    }
};

// Splits the run into pieces by whether there is a lifted wall below and above.
std::vector<Piece> classify(const WallRun &run, const LayerWalls &below, const LayerWalls &above, double tolerance)
{
    const size_t       steps = std::max<size_t>(1, size_t(std::ceil(run.total / scaled<double>(SAMPLE_STEP_MM))));
    std::vector<Piece> pieces;
    for (size_t k = 0; k < steps; ++k) {
        const double  from   = run.total * double(k) / double(steps);
        const double  to     = k + 1 == steps ? run.total : run.total * double(k + 1) / double(steps);
        const Point3  middle = run.point_at(0.5 * (from + to));
        const Point   point(middle.x(), middle.y());
        unsigned char neighbours = bnNone;
        if (below.has_wall_near(point, tolerance))
            neighbours |= bnBelow;
        if (above.has_wall_near(point, tolerance))
            neighbours |= bnAbove;
        if (!pieces.empty() && pieces.back().neighbours == neighbours)
            pieces.back().to = to;
        else
            pieces.push_back({from, to, neighbours});
    }
    return pieces;
}

// Gives each piece shorter than min_length the class of its longer neighbour.
void merge_short_pieces(std::vector<Piece> &pieces, double min_length)
{
    while (pieces.size() > 1) {
        const auto shortest = std::min_element(pieces.begin(), pieces.end(),
                                               [](const Piece &a, const Piece &b) { return a.length() < b.length(); });
        if (shortest->length() >= min_length)
            break;
        const size_t i        = size_t(shortest - pieces.begin());
        const bool   use_prev = i > 0 && (i + 1 == pieces.size() || pieces[i - 1].length() >= pieces[i + 1].length());
        pieces[i].neighbours  = pieces[use_prev ? i - 1 : i + 1].neighbours;

        std::vector<Piece> merged;
        merged.reserve(pieces.size());
        for (const Piece &piece : pieces) {
            if (!merged.empty() && merged.back().neighbours == piece.neighbours)
                merged.back().to = piece.to;
            else
                merged.push_back(piece);
        }
        pieces = std::move(merged);
    }
}

// A lifted wall reaches from the middle of its own layer (or from the top of
// the layer below where a stack starts) up to the middle of the layer above
// (or to the top of its own layer where the stack ends). With a constant
// layer height h that is height h at print_z + h/2, 1.5h at the start and
// 0.5h at print_z at the end. A piece with no lifted wall below or above is
// left as it is.
void lift(ExtrusionPath &path, unsigned char neighbours, double layer_height, double upper_layer_height)
{
    if (neighbours == bnNone || path.height <= 0.f)
        return;
    const double bottom = (neighbours & bnBelow) ? -0.5 * layer_height : -layer_height;
    const double top    = (neighbours & bnAbove) ? 0.5 * upper_layer_height : 0.;
    const double height = top - bottom;
    path.mm3_per_mm *= height / double(path.height);
    path.height   = float(height);
    path.z_offset = top;
}

// Lifts one path by the pieces of its run that it overlaps.
void lift_path(ExtrusionPath &path, const std::vector<double> &along, double path_start, const std::vector<Piece> &pieces,
               double layer_height, double upper_layer_height, ExtrusionPaths &out)
{
    const double       path_end = path_start + along.back();
    std::vector<Piece> parts;
    for (const Piece &piece : pieces) {
        const double from = std::max(piece.from, path_start);
        const double to   = std::min(piece.to, path_end);
        if (to > from)
            parts.push_back({from - path_start, to - path_start, piece.neighbours});
    }
    if (parts.empty()) {
        // A path of zero length takes the class of the piece it sits in, so Z does not hop for it.
        const auto it = std::find_if(pieces.begin(), pieces.end(), [path_start](const Piece &p) { return path_start < p.to; });
        lift(path, (it == pieces.end() ? pieces.back() : *it).neighbours, layer_height, upper_layer_height);
        out.emplace_back(std::move(path));
        return;
    }
    // A piece boundary just inside this path would leave a sliver; it joins its neighbour.
    merge_short_pieces(parts, scaled<double>(MIN_SLIVER_LENGTH_MM));
    if (parts.size() == 1) {
        lift(path, parts.front().neighbours, layer_height, upper_layer_height);
        out.emplace_back(std::move(path));
        return;
    }
    const Points3 &points = path.polyline.points;
    for (const Piece &part : parts) {
        ExtrusionPath &lifted = out.emplace_back(Polyline3(sub_polyline(points, along, part.from, part.to)), path);
        lift(lifted, part.neighbours, layer_height, upper_layer_height);
    }
}

void lift_walls(ExtrusionPaths &paths, const LayerWalls &below, const LayerWalls &above, double tolerance,
                double layer_height, double upper_layer_height)
{
    ExtrusionPaths out;
    out.reserve(paths.size());
    for (size_t first = 0; first < paths.size();) {
        if (!is_liftable(paths[first]) || paths[first].polyline.size() < 2) {
            out.emplace_back(std::move(paths[first++]));
            continue;
        }
        size_t  last = first;
        WallRun run;
        for (; last < paths.size() && is_liftable(paths[last]) && paths[last].polyline.size() >= 2; ++last)
            run.add(paths[last].polyline.points);
        if (run.total <= 0.) {
            for (; first < last; ++first)
                out.emplace_back(std::move(paths[first]));
            continue;
        }
        std::vector<Piece> pieces = classify(run, below, above, tolerance);
        merge_short_pieces(pieces, scaled<double>(MIN_PIECE_LENGTH_MM));
        // The run points into the paths, so they are moved out only after classify() is done with it.
        for (size_t k = 0; first + k < last; ++k)
            lift_path(paths[first + k], run.along[k], run.start[k], pieces, layer_height, upper_layer_height, out);
        first = last;
    }
    paths = std::move(out);
}

} // namespace

void apply_brick_layers(PrintObject &object)
{
    const PrintConfig &print_config = object.print()->config();
    // A vase has a single wall, and tools with their own layer heights merge
    // walls across layers, which leaves no layer-by-layer stack to stagger.
    if (print_config.spiral_mode.value || print_config.independent_tool_layer_heights.value)
        return;

    bool any_region = false;
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++region_id)
        any_region |= region_uses_brick_layers(object.printing_region(region_id).config());
    LayerPtrs &layers = object.layers();
    if (!any_region || layers.size() <= FIRST_BRICK_LAYER)
        return;

    // Collect every layer's candidates first, so each layer can then be
    // changed while reading its neighbours.
    std::vector<LayerWalls> walls(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(FIRST_BRICK_LAYER, layers.size()),
                      [&layers, &walls](const tbb::blocked_range<size_t> &range) {
                          for (size_t idx = range.begin(); idx < range.end(); ++idx)
                              walls[idx] = collect_candidate_walls(*layers[idx]);
                      });
    if (object.print()->canceled())
        throw CanceledException();

    const LayerWalls no_walls;
    tbb::parallel_for(tbb::blocked_range<size_t>(FIRST_BRICK_LAYER, layers.size()),
                      [&layers, &walls, &no_walls](const tbb::blocked_range<size_t> &range) {
                          for (size_t idx = range.begin(); idx < range.end(); ++idx) {
                              if (walls[idx].empty)
                                  continue;
                              Layer            &layer        = *layers[idx];
                              const bool        has_upper    = idx + 1 < layers.size();
                              const LayerWalls &below        = walls[idx - 1];
                              const LayerWalls &above        = has_upper ? walls[idx + 1] : no_walls;
                              const double      upper_height = has_upper ? layers[idx + 1]->height : 0.;
                              for (LayerRegion *layerm : layer.regions()) {
                                  if (!region_uses_brick_layers(layerm->region().config()))
                                      continue;
                                  const double tolerance = NEIGHBOUR_TOLERANCE_RATIO *
                                                           double(layerm->flow(frPerimeter).scaled_spacing());
                                  auto lift_candidate = [&](ExtrusionPaths &paths) {
                                      lift_walls(paths, below, above, tolerance, layer.height, upper_height);
                                  };
                                  for_each_candidate(layerm->perimeters, lift_candidate);
                              }
                          }
                      });
}

} // namespace Slic3r
