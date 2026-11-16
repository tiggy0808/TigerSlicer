#ifndef slic3r_ModelTools_EdgeBlend_hpp_
#define slic3r_ModelTools_EdgeBlend_hpp_

// TigerSlicer: the geometry behind the fillet and chamfer tool. It finds a mesh's sharp edges and builds,
// for a chosen one, the solid that rounds or bevels it: a negative volume to cut a convex edge, or a part
// to fill a concave one. The model's own mesh is never changed.

#include <admesh/stl.h>

#include <cstddef>
#include <vector>

namespace Slic3r::ModelTools {

// A run of sharp mesh edges. Segment k joins vertices[k] and vertices[k + 1], and the last vertex back to
// the first when the chain is closed (a closed chain does not repeat its first vertex).
struct EdgeChain
{
    std::vector<int> vertices;   // indices into its.vertices
    std::vector<int> face_left;  // per segment: the face that holds the segment in the chain's direction
    std::vector<int> face_right; // per segment: the face across it
    bool             closed = false;
    bool             convex = true; // the faces meet around material (an outside edge), not around air

    size_t segments() const { return face_left.size(); }
};

// The chains of edges whose two faces turn by more than min_angle (degrees). A chain ends where three or
// more sharp edges meet, where the edge changes between convex and concave, and where it turns by more
// than 45 degrees. Needs a consistently oriented mesh; open and non-manifold edges are skipped.
std::vector<EdgeChain> sharp_edges(const indexed_triangle_set &its, double min_angle = 30.);

enum class EdgeBlend { Fillet, Chamfer };

// The closed solid that rounds (a fillet of radius `size`) or bevels (a chamfer `size` along each face)
// the chain's edge, in the mesh's coordinates. For a convex edge it is the material to remove, and
// reaches a little past the faces; for a concave edge it is the material to add, and reaches a little
// into them. Empty if the faces meet at less than 20 or more than 160 degrees anywhere along the chain, or
// if the blend is too big for the chain's curve (it would reach past the centre of a small round rim).
// Where chains meet at a corner, each ends square: corners are not blended.
indexed_triangle_set edge_blend_mesh(const indexed_triangle_set &its, const EdgeChain &chain, EdgeBlend type, double size);

} // namespace Slic3r::ModelTools

#endif // slic3r_ModelTools_EdgeBlend_hpp_
