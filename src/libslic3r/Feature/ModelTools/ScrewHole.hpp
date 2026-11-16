#ifndef slic3r_ModelTools_ScrewHole_hpp_
#define slic3r_ModelTools_ScrewHole_hpp_

// TigerSlicer: the geometry behind the hole tool, which drills a round hole for a metric screw into a model
// as a negative volume.

#include "libslic3r/Point.hpp"

#include <admesh/stl.h>

#include <string>
#include <vector>

namespace Slic3r::ModelTools {

// How the screw goes into the hole.
enum class HoleFit {
    ThreadIn, // a pilot hole the screw cuts its own thread into
    Through,  // a clearance hole the screw slides through
};

struct ScrewSize
{
    std::string name;      // "M3"
    double      pilot;     // mm, ISO tap drill
    double      clearance; // mm, ISO 273 normal fit
};

// M2 to M8.
const std::vector<ScrewSize> &screw_sizes();

double hole_diameter(const ScrewSize &size, HoleFit fit);

// "M3 thread-in hole", "M3 through hole".
std::string hole_name(const ScrewSize &size, HoleFit fit);

// A round hole as a closed mesh around the line through `point` along `normal` (the outward surface normal),
// from `above` over the surface down to `depth` under it. The polygon's flats touch the circle, so the
// hole is nowhere narrower than `diameter`. Coordinates are those of `point`.
indexed_triangle_set hole_mesh(const Vec3d &point, const Vec3d &normal, double diameter, double depth, double above = 0.5);

} // namespace Slic3r::ModelTools

#endif // slic3r_ModelTools_ScrewHole_hpp_
