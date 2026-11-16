#include <catch2/catch_all.hpp>

#include <libslic3r/Feature/ModelTools/EdgeBlend.hpp>
#include <libslic3r/Feature/ModelTools/ScrewHole.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <algorithm>
#include <cmath>

using namespace Slic3r;
using namespace Slic3r::ModelTools;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const ScrewSize &size_named(const std::string &name)
{
    const std::vector<ScrewSize> &sizes = screw_sizes();
    auto it = std::find_if(sizes.begin(), sizes.end(), [&name](const ScrewSize &s) { return s.name == name; });
    REQUIRE(it != sizes.end());
    return *it;
}

// The volume of what is left of `body` once `cutter` is subtracted.
double volume_after_cut(const indexed_triangle_set &body, const indexed_triangle_set &cutter)
{
    indexed_triangle_set result = body;
    MeshBoolean::cgal::minus(result, cutter);
    return its_volume(result);
}

double volume_after_union(const indexed_triangle_set &body, const indexed_triangle_set &addition)
{
    indexed_triangle_set result = body;
    MeshBoolean::cgal::plus(result, addition);
    return its_volume(result);
}

// An L-shaped prism, 20 mm long in X: a 20 x 20 mm square in YZ with its 10 x 10 mm corner at +Y +Z taken out.
// Its one concave edge runs along X at y = 10, z = 10.
indexed_triangle_set l_prism()
{
    indexed_triangle_set l = its_make_cube(20., 20., 20.);
    indexed_triangle_set notch = its_make_cube(30., 15., 15.);
    its_translate(notch, Vec3f(-5.f, 10.f, 10.f));
    MeshBoolean::cgal::minus(l, notch);
    return l;
}

// The chain with a vertex closest to `through`.
const EdgeChain *chain_near(const indexed_triangle_set &its, const std::vector<EdgeChain> &chains, const Vec3d &through)
{
    const EdgeChain *best      = nullptr;
    double           best_dist = std::numeric_limits<double>::max();
    for (const EdgeChain &chain : chains)
        for (int v : chain.vertices) {
            const double d = (its.vertices[v].cast<double>() - through).norm();
            if (d < best_dist) {
                best_dist = d;
                best      = &chain;
            }
        }
    return best;
}

double chain_length(const indexed_triangle_set &its, const EdgeChain &chain)
{
    double length = 0.;
    for (size_t k = 0; k < chain.segments(); ++k) {
        const int a = chain.vertices[k];
        const int b = chain.vertices[(k + 1) % chain.vertices.size()];
        length += (its.vertices[b] - its.vertices[a]).cast<double>().norm();
    }
    return length;
}

} // namespace

TEST_CASE("Screw sizes give ISO tap drill and clearance diameters", "[ModelTools]")
{
    const ScrewSize &m3 = size_named("M3");
    CHECK_THAT(hole_diameter(m3, HoleFit::ThreadIn), WithinAbs(2.5, 1e-9));
    CHECK_THAT(hole_diameter(m3, HoleFit::Through), WithinAbs(3.4, 1e-9));
    for (const ScrewSize &size : screw_sizes())
        CHECK(hole_diameter(size, HoleFit::ThreadIn) < hole_diameter(size, HoleFit::Through));
    CHECK(hole_name(m3, HoleFit::ThreadIn) == "M3 thread-in hole");
    CHECK(hole_name(m3, HoleFit::Through) == "M3 through hole");
}

TEST_CASE("A hole mesh is a closed cylinder along the surface normal, no narrower than its diameter", "[ModelTools]")
{
    const double diameter = 3.4;
    const double depth    = 8.;
    const double above    = 0.5;
    const Vec3d  point(5., 6., 7.);
    const Vec3d  normal   = GENERATE(Vec3d(0., 0., 1.), Vec3d(1., 0., 0.), Vec3d(0., -1., 1.).normalized());

    const indexed_triangle_set hole = hole_mesh(point, normal, diameter, depth, above);
    REQUIRE(its_num_open_edges(hole) == 0);
    REQUIRE(its_volume(hole) > 0.f);

    // Every vertex lies between `above` over the surface and `depth` under it, and the rim vertices (all
    // but the two cap centers on the axis) no closer to the axis than the radius: the flats of the polygon
    // touch the circle.
    double min_along = std::numeric_limits<double>::max(), max_along = -min_along, min_radial = min_along;
    for (const Vec3f &v : hole.vertices) {
        const Vec3d  d      = v.cast<double>() - point;
        const double along  = d.dot(normal);
        const double radial = (d - along * normal).norm();
        min_along           = std::min(min_along, along);
        max_along           = std::max(max_along, along);
        if (radial > 1e-3)
            min_radial = std::min(min_radial, radial);
    }
    CHECK_THAT(min_along, WithinAbs(-depth, 1e-4));
    CHECK_THAT(max_along, WithinAbs(above, 1e-4));
    CHECK(min_radial >= 0.5 * diameter - 1e-4);
    // The volume is that of a cylinder a hair wider than the diameter.
    const double cylinder = M_PI * 0.25 * diameter * diameter * (depth + above);
    CHECK(its_volume(hole) >= cylinder * (1. - 1e-4));
    CHECK_THAT(its_volume(hole), WithinRel(cylinder, 0.01));
}

TEST_CASE("A cube has twelve straight convex sharp edges", "[ModelTools]")
{
    const indexed_triangle_set cube   = its_make_cube(20., 20., 20.);
    const std::vector<EdgeChain> chains = sharp_edges(cube);
    REQUIRE(chains.size() == 12);
    for (const EdgeChain &chain : chains) {
        CHECK_FALSE(chain.closed);
        CHECK(chain.convex);
        CHECK_THAT(chain_length(cube, chain), WithinAbs(20., 1e-4));
    }
}

TEST_CASE("A cylinder's sharp edges are its two rims, as closed loops", "[ModelTools]")
{
    const indexed_triangle_set cylinder = its_make_cylinder(10., 20.);
    const std::vector<EdgeChain> chains  = sharp_edges(cylinder);
    REQUIRE(chains.size() == 2);
    for (const EdgeChain &chain : chains) {
        CHECK(chain.closed);
        CHECK(chain.convex);
        CHECK_THAT(chain_length(cylinder, chain), WithinRel(2. * M_PI * 10., 0.01));
    }
}

TEST_CASE("The inside corner of an L shape is one concave sharp edge", "[ModelTools]")
{
    const indexed_triangle_set   l      = l_prism();
    const std::vector<EdgeChain> chains = sharp_edges(l);
    const size_t concave = std::count_if(chains.begin(), chains.end(), [](const EdgeChain &c) { return !c.convex; });
    REQUIRE(concave == 1);
    const EdgeChain &inside = *std::find_if(chains.begin(), chains.end(), [](const EdgeChain &c) { return !c.convex; });
    CHECK_FALSE(inside.closed);
    CHECK_THAT(chain_length(l, inside), WithinAbs(20., 1e-3));
}

TEST_CASE("A fillet on a cube edge removes the corner outside the radius", "[ModelTools]")
{
    const indexed_triangle_set   cube   = its_make_cube(20., 20., 20.);
    const std::vector<EdgeChain> chains = sharp_edges(cube);
    const EdgeChain *top_front = chain_near(cube, chains, Vec3d(10., 0., 20.));
    REQUIRE(top_front != nullptr);

    const double radius = 4.;
    const indexed_triangle_set cutter = edge_blend_mesh(cube, *top_front, EdgeBlend::Fillet, radius);
    REQUIRE_FALSE(cutter.indices.empty());
    REQUIRE(its_num_open_edges(cutter) == 0);
    REQUIRE(its_volume(cutter) > 0.f);

    // What goes is the square r x r corner less a quarter circle, all along the 20 mm edge.
    const double removed = (1. - M_PI / 4.) * radius * radius * 20.;
    CHECK_THAT(8000. - volume_after_cut(cube, cutter), WithinRel(removed, 0.01));
}

TEST_CASE("A chamfer on a cube edge removes a triangular prism", "[ModelTools]")
{
    const indexed_triangle_set   cube   = its_make_cube(20., 20., 20.);
    const std::vector<EdgeChain> chains = sharp_edges(cube);
    const EdgeChain *edge = chain_near(cube, chains, Vec3d(20., 10., 0.));
    REQUIRE(edge != nullptr);

    const double size = 3.;
    const indexed_triangle_set cutter = edge_blend_mesh(cube, *edge, EdgeBlend::Chamfer, size);
    REQUIRE(its_num_open_edges(cutter) == 0);
    CHECK_THAT(8000. - volume_after_cut(cube, cutter), WithinRel(0.5 * size * size * 20., 1e-3));
}

TEST_CASE("A fillet on a cylinder's rim rounds it all the way around", "[ModelTools]")
{
    const double cylinder_radius = 10.;
    const indexed_triangle_set   cylinder = its_make_cylinder(cylinder_radius, 20.);
    const std::vector<EdgeChain> chains   = sharp_edges(cylinder);
    const EdgeChain *top = chain_near(cylinder, chains, Vec3d(0., cylinder_radius, 20.));
    REQUIRE(top != nullptr);
    REQUIRE(top->closed);

    const double radius = 2.;
    const indexed_triangle_set cutter = edge_blend_mesh(cylinder, *top, EdgeBlend::Fillet, radius);
    REQUIRE(its_num_open_edges(cutter) == 0);

    // Pappus: the corner's area turned around the axis at the distance of its centroid, which sits
    // (10 - 3 pi) / (12 - 3 pi) r in from each face.
    const double area     = (1. - M_PI / 4.) * radius * radius;
    const double centroid = cylinder_radius - (10. - 3. * M_PI) / (12. - 3. * M_PI) * radius;
    const double removed  = 2. * M_PI * centroid * area;
    CHECK_THAT(its_volume(cylinder) - volume_after_cut(cylinder, cutter), WithinRel(removed, 0.02));
}

TEST_CASE("A fillet on a concave edge adds material into the corner", "[ModelTools]")
{
    const indexed_triangle_set   l      = l_prism();
    const std::vector<EdgeChain> chains = sharp_edges(l);
    const EdgeChain &inside = *std::find_if(chains.begin(), chains.end(), [](const EdgeChain &c) { return !c.convex; });

    const double radius = 3.;
    const indexed_triangle_set addition = edge_blend_mesh(l, inside, EdgeBlend::Fillet, radius);
    REQUIRE(its_num_open_edges(addition) == 0);
    REQUIRE(its_volume(addition) > 0.f);

    const double added = (1. - M_PI / 4.) * radius * radius * 20.;
    CHECK_THAT(volume_after_union(l, addition) - its_volume(l), WithinRel(added, 0.01));
}

TEST_CASE("A fillet too big for a rim's curve gives no mesh", "[ModelTools]")
{
    // A 3 mm boss: a 5 mm fillet would reach past its axis.
    const indexed_triangle_set   boss   = its_make_cylinder(3., 10.);
    const std::vector<EdgeChain> chains = sharp_edges(boss);
    REQUIRE(chains.size() == 2);
    CHECK(edge_blend_mesh(boss, chains.front(), EdgeBlend::Fillet, 5.).indices.empty());
    // A 1 mm one fits.
    CHECK_FALSE(edge_blend_mesh(boss, chains.front(), EdgeBlend::Fillet, 1.).indices.empty());
}

TEST_CASE("A blend whose faces meet almost flat gives no mesh", "[ModelTools]")
{
    // Two triangles folded 10 degrees: not a sharp edge, and no blend for it either.
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(10.f, 0.f, 0.f), Vec3f(5.f, 10.f, 0.f),
                    Vec3f(5.f, -10.f, float(-10. * std::tan(10. * M_PI / 180.)))};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(1, 0, 3)};
    CHECK(sharp_edges(its).empty());

    EdgeChain chain;
    chain.vertices   = {0, 1};
    chain.face_left  = {0};
    chain.face_right = {1};
    CHECK(edge_blend_mesh(its, chain, EdgeBlend::Fillet, 1.).indices.empty());
}
