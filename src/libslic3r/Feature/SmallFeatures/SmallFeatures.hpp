// TigerSlicer: features narrower than an extrusion line, and how far a printed layer strays from the model.
#ifndef slic3r_SmallFeatures_hpp_
#define slic3r_SmallFeatures_hpp_

#include "../../ExPolygon.hpp"
#include "../../Polygon.hpp"

#include <vector>

namespace Slic3r {

class Layer;
class LayerRegion;
class Polyline3;
class Print;
class SurfaceCollection;

namespace SmallFeatures {

// Deviations thinner than this (mm) are rounding noise between neighbouring extrusions, not missing features.
constexpr double kDeviationTolerance = 0.02;

// An area where the print strays from the model, and about how wide it is in mm: twice its area over its
// perimeter, which is the width of a long thin strip.
struct DeviationArea
{
    ExPolygon shape;
    double    magnitude;
};

struct LayerDeviation
{
    // Model area that no extrusion covers.
    std::vector<DeviationArea> undershoot;
    // Extrusion outside the model.
    std::vector<DeviationArea> overshoot;
};

// The path's points in 2D, with its fitted arcs (arc fitting keeps only their end points) expanded back into
// segments no more than `max_error` (scaled) from the arc.
Points path_points(const Polyline3 &polyline, double max_error);

// What the layer's object extrusions cover: every path buffered by half its width, with round ends like a bead.
// Paths without flow (moves inside a wall) cover nothing. The area left for infill counts as covered: sparse
// infill leaves gaps on purpose.
Polygons printed_coverage(const Layer &layer);

// The layer's printed coverage against its slices. Areas thinner than `tolerance` (mm), anywhere or overall, are dropped.
LayerDeviation layer_deviation(const Layer &layer, double tolerance = kDeviationTolerance);

// One height of the print, with each object layer's deviation placed where its instances print (scaled print
// coordinates, as in the G-code preview).
struct PrintedLayerDeviation
{
    double         print_z;
    LayerDeviation deviation;
};

// The deviation of every object layer of the print, merged by print height and sorted by it.
std::vector<PrintedLayerDeviation> print_deviation(const Print &print);

// The parts of `slices` narrower than `min_width` (scaled), one ExPolygon per feature. The slivers that rounding a
// sharp corner leaves, shorter than they are wide, are not features.
ExPolygons features_below_width(const ExPolygons &slices, coord_t min_width);

// Prints what the walls leave uncovered of the region's thin parts, narrower than two outer walls, by its
// small_feature_policy (experimental). Each such part gets one line through its middle, its width and free end set by
// the policy. A line that starts at a wall loop
// becomes that loop's dead end, printed out and back as the loop passes it; any other is an open path of its own.
// `slices` are the region's slices the walls were made from, and `fill_area` the area left for infill, which counts
// as covered.
void add_small_feature_lines(LayerRegion &layerm, const SurfaceCollection &slices, const ExPolygons &fill_area);

// Narrows the region's walls where they spill into a gap of the layer narrower than two lines, so that the gap keeps
// its size (narrow_gap_policy keep_open, experimental). A bead gets no wider than twice its distance from the gaps, and
// no narrower than small_feature_min_width. `layer_slices` are all of the layer's slices.
void keep_narrow_gaps_open(LayerRegion &layerm, const ExPolygons &layer_slices);

} // namespace SmallFeatures
} // namespace Slic3r

#endif // slic3r_SmallFeatures_hpp_
