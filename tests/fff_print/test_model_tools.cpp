#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Feature/ModelTools/EdgeBlend.hpp"
#include "libslic3r/Feature/ModelTools/ScrewHole.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_helpers.hpp"

#include <cmath>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::ModelTools;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kCube = 20.;

// A 20 mm cube with `tool` (in the cube's coordinates, its corner at the origin) added as a volume of
// `type`, sliced at 0.2 mm layers.
void slice_cube_with(const indexed_triangle_set &tool, ModelVolumeType type, Model &model, Print &print)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(TriangleMesh(its_make_cube(kCube, kCube, kCube)));
    object->add_volume(TriangleMesh(tool), type);
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"nozzle_diameter", "0.4"}, {"layer_height", 0.2}, {"initial_layer_print_height", 0.2}});
    print.apply(model, config);
    print.process();
}

const Layer &layer_at(const Print &print, double slice_z)
{
    const Layer *best = nullptr;
    for (const Layer *layer : print.objects().front()->layers())
        if (best == nullptr || std::abs(layer->slice_z - slice_z) < std::abs(best->slice_z - slice_z))
            best = layer;
    REQUIRE(best != nullptr);
    return *best;
}

BoundingBox slices_bbox(const Layer &layer) { return get_extents(layer.lslices); }

} // namespace

TEST_CASE("A fillet cutter as a negative volume rounds the sliced cube's edge", "[ModelTools]")
{
    const indexed_triangle_set   cube   = its_make_cube(kCube, kCube, kCube);
    const std::vector<EdgeChain> chains = sharp_edges(cube);
    // The top edge along X at y = 0.
    const EdgeChain *edge = nullptr;
    for (const EdgeChain &chain : chains) {
        const Vec3f a = cube.vertices[chain.vertices.front()], b = cube.vertices[chain.vertices.back()];
        if (std::abs(a.y()) < 1e-3 && std::abs(b.y()) < 1e-3 && std::abs(a.z() - kCube) < 1e-3 && std::abs(b.z() - kCube) < 1e-3)
            edge = &chain;
    }
    REQUIRE(edge != nullptr);

    const double radius = 4.;
    Model        model;
    Print        print;
    slice_cube_with(edge_blend_mesh(cube, *edge, EdgeBlend::Fillet, radius), ModelVolumeType::NEGATIVE_VOLUME, model, print);

    // Under the fillet the front face is where it was; above 20 - r it steps back along the circle.
    const double front = unscale<double>(slices_bbox(layer_at(print, 10.)).min.y());
    for (const double z : {18.9, 19.9}) {
        const double dz       = z - (kCube - radius);
        const double expected = radius - std::sqrt(radius * radius - dz * dz);
        const BoundingBox bbox = slices_bbox(layer_at(print, z));
        CHECK_THAT(unscale<double>(bbox.min.y()) - front, WithinAbs(expected, 0.1));
        // Only the front edge is rounded: the back face stays put.
        CHECK_THAT(unscale<double>(bbox.max.y()) - front, WithinAbs(kCube, 0.05));
    }
}

TEST_CASE("A screw hole as a negative volume leaves a round hole of its diameter through every layer", "[ModelTools]")
{
    const ScrewSize *m3 = nullptr;
    for (const ScrewSize &size : screw_sizes())
        if (size.name == "M3")
            m3 = &size;
    REQUIRE(m3 != nullptr);
    const double diameter = hole_diameter(*m3, HoleFit::Through);

    // Drilled down from the middle of the top face, all the way through and 1 mm past.
    Model model;
    Print print;
    slice_cube_with(hole_mesh(Vec3d(kCube / 2., kCube / 2., kCube), Vec3d::UnitZ(), diameter, kCube + 1.), ModelVolumeType::NEGATIVE_VOLUME,
                    model, print);

    const double hole_area = M_PI * 0.25 * diameter * diameter;
    for (const double z : {1., 10., 19.9}) {
        const Layer &layer = layer_at(print, z);
        REQUIRE(layer.lslices.size() == 1);
        REQUIRE(layer.lslices.front().holes.size() == 1);
        const double area = std::abs(layer.lslices.front().holes.front().area()) * SCALING_FACTOR * SCALING_FACTOR;
        CHECK_THAT(area, WithinRel(hole_area, 0.03));
    }
}
