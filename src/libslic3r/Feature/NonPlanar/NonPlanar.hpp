// TigerSlicer: non-planar top layers (non_planar_mode on, experimental). Gently sloped tops of an object are printed in
// curved layers that follow the surface, instead of flat layers that leave steps on it.
#ifndef slic3r_NonPlanar_hpp_
#define slic3r_NonPlanar_hpp_

#include "../../ExPolygon.hpp"

#include <cstddef>

namespace Slic3r {

class PrintObject;
class TriangleMesh;

namespace NonPlanar {

// A gently sloped top of an object, printed in `shells` curved layers that follow it, one layer height apart, the top one
// on the surface. The flat layers under the zone stop `shells` layers below its surface.
struct Zone
{
    // Where the curved layers go: the surface seen from above (scaled, the object's layer coordinates).
    ExPolygons footprint;
    // The surface's lowest and highest points in the zone (mm, the object's Z: 0 at its bottom).
    double     bottom;
    double     top;
    // The flat layers' height, and how many curved layers.
    double     layer_height;
    int        shells;
    // The object's printing region whose settings the zone takes, and the layer that prints the curved layers: the
    // first that reaches the zone's top.
    size_t     region;
    size_t     layer;

    double thickness() const { return shells * layer_height; }
};

// The object's mesh where its layers are, as Z contouring casts rays on it: the layers' XY, the model's Z. Its Z is
// `ground_level` above the object's Z.
TriangleMesh object_mesh(const PrintObject &object, double &ground_level);

} // namespace NonPlanar
} // namespace Slic3r

#endif // slic3r_NonPlanar_hpp_
