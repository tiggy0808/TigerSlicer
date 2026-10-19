#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Print settings the assertions below are derived from.
constexpr double layer_height = 0.2;  // mm, also the first layer height
constexpr int    wall_loops   = 4;    // the outer wall and inner walls 1, 2 and 3; 1 and 3 are lifted
constexpr double cube_size    = 20.;  // mm, so 100 layers
constexpr double cube_top_z   = cube_size;
constexpr double lifted_z     = 0.5 * layer_height;

// Moves within this distance of a Z are at that Z (G-code Z is written with 3 decimals).
constexpr double z_tolerance = 0.002;
// Flow ratios are measured from E values rounded in the G-code and from walls Arachne may
// widen slightly at the corners, so they are compared to within a few percent.
constexpr double flow_tolerance = 0.05;
// Moves shorter than this are ignored for flow, where the rounding of E dominates.
constexpr double min_flow_move_length = 1.0; // mm

DynamicPrintConfig brick_config(const char *wall_generator, bool brick_layers,
                                 DynamicPrintConfig config = DynamicPrintConfig::full_print_config())
{
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               layer_height },
        { "initial_layer_print_height", layer_height },
        { "wall_loops",                 wall_loops },
        { "brick_layers",               brick_layers },
        { "only_one_wall_top",          false },
        { "only_one_wall_first_layer",  false },
        { "sparse_infill_density",      "15%" },
        { "top_shell_layers",           3 },
        { "bottom_shell_layers",        3 },
        // Plain straight moves at a fixed height, so Z and E per mm can be read off each line.
        { "seam_slope_type",            "none" },
        { "enable_arc_fitting",         false },
        { "use_relative_e_distances",   true },
    });
    return config;
}

struct WallMove
{
    bool   inner;   // inner wall, otherwise outer wall
    double z;
    double length;  // mm
    double e;       // mm of filament

    bool lifted(double layer_z) const { return z > layer_z + z_tolerance; }
};

struct PrintedLayer
{
    double                z = 0.;
    std::vector<WallMove> walls;
    // For every extrusion on the layer in print order: whether it is a lifted inner wall.
    std::vector<bool>     lifted_order;
    std::vector<bool>     inner_wall_order;
};

std::vector<PrintedLayer> printed_layers(const std::string &gcode, const DynamicPrintConfig &config)
{
    std::vector<PrintedLayer> layers;
    std::string               role;
    GCodeReader               reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&layers, &role](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (const size_t pos = comment.find("Z_HEIGHT:"); pos != std::string_view::npos) {
            layers.emplace_back().z = std::stod(std::string(comment.substr(pos + 9)));
            return;
        }
        if (comment.rfind("Z:", 0) == 0) {
            layers.emplace_back().z = std::stod(std::string(comment.substr(2)));
            return;
        }
        if (comment.find("TYPE:") != std::string_view::npos || comment.find("FEATURE:") != std::string_view::npos) {
            role = std::string(comment.substr(comment.find(':') + 1));
            role.erase(0, role.find_first_not_of(' '));
            return;
        }
        if (layers.empty() || !line.extruding(self) || line.dist_XY(self) <= 0.f)
            return;
        PrintedLayer &layer      = layers.back();
        const bool    inner_wall = role == "Inner wall";
        const double  z          = line.new_Z(self);
        layer.lifted_order.push_back(inner_wall && z > layer.z + z_tolerance);
        layer.inner_wall_order.push_back(inner_wall);
        if (inner_wall || role == "Outer wall")
            layer.walls.push_back({inner_wall, z, line.dist_XY(self), line.dist_E(self)});
    });
    return layers;
}

const PrintedLayer &layer_at(const std::vector<PrintedLayer> &layers, double z)
{
    const auto it = std::find_if(layers.begin(), layers.end(), [z](const PrintedLayer &l) { return std::abs(l.z - z) < z_tolerance; });
    REQUIRE(it != layers.end());
    return *it;
}

template<typename Keep> double e_per_mm(const PrintedLayer &layer, Keep keep)
{
    double e = 0., length = 0.;
    for (const WallMove &move : layer.walls)
        if (move.length >= min_flow_move_length && keep(move)) {
            e += move.e;
            length += move.length;
        }
    REQUIRE(length > 0.);
    return e / length;
}

double lifted_inner_e_per_mm(const PrintedLayer &layer)
{
    return e_per_mm(layer, [&layer](const WallMove &m) { return m.inner && m.lifted(layer.z); });
}

double level_inner_e_per_mm(const PrintedLayer &layer)
{
    return e_per_mm(layer, [&layer](const WallMove &m) { return m.inner && !m.lifted(layer.z); });
}

// Where a stack of lifted walls ends, every inner wall is at the layer's Z, so the walls that end
// the stack are told apart by flow: the flow of those (well under the others) over the rest.
double ending_to_full_flow_ratio(const PrintedLayer &layer)
{
    double max_e_per_mm = 0.;
    for (const WallMove &move : layer.walls)
        if (move.inner && move.length >= min_flow_move_length)
            max_e_per_mm = std::max(max_e_per_mm, move.e / move.length);
    REQUIRE(max_e_per_mm > 0.);
    const double split = 0.75 * max_e_per_mm;
    return e_per_mm(layer, [split](const WallMove &m) { return m.inner && m.e / m.length < split; }) /
           e_per_mm(layer, [split](const WallMove &m) { return m.inner && m.e / m.length >= split; });
}

} // namespace

TEST_CASE("Brick layers are off by default", "[BrickLayers]")
{
    CHECK_FALSE(DynamicPrintConfig::full_print_config().opt_bool("brick_layers"));

    DynamicPrintConfig config = brick_config("classic", false);
    config.erase("brick_layers");
    const std::vector<PrintedLayer> layers = printed_layers(slice({cube(cube_size)}, config), config);
    REQUIRE(layers.size() >= size_t(cube_size / layer_height));
    for (const PrintedLayer &layer : layers)
        for (const WallMove &move : layer.walls)
            CHECK_THAT(move.z, WithinAbs(layer.z, z_tolerance));
}

TEST_CASE("Brick layers stay off when the walls and the infill use different filaments", "[BrickLayers]")
{
    // Lifted walls go last in their extruder's pass, which another filament's pass may follow.
    const DynamicPrintConfig config = brick_config("classic", true,
        multifilament_config(2, {
            { "outer_wall_filament_id",     1 },
            { "inner_wall_filament_id",     1 },
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
        }));
    const std::vector<PrintedLayer> layers = printed_layers(slice({cube(cube_size)}, config), config);
    size_t inner_wall_moves = 0;
    for (const PrintedLayer &layer : layers)
        for (const WallMove &move : layer.walls)
            if (move.inner) {
                ++inner_wall_moves;
                CHECK_THAT(move.z, WithinAbs(layer.z, z_tolerance));
            }
    CHECK(inner_wall_moves > 0);
}

TEST_CASE("Brick layers lift every other inner wall by half a layer", "[BrickLayers]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator)
    {
        const DynamicPrintConfig        config = brick_config(wall_generator, true);
        const std::vector<PrintedLayer> layers = printed_layers(slice({cube(cube_size)}, config), config);
        const PrintedLayer             &middle = layer_at(layers, 0.5 * cube_size);

        double lifted_length = 0., inner_length = 0.;
        for (const WallMove &move : middle.walls) {
            if (!move.inner) {
                // The outer wall keeps the layer height, so the surface looks unchanged.
                CHECK_THAT(move.z, WithinAbs(middle.z, z_tolerance));
                continue;
            }
            inner_length += move.length;
            if (move.lifted(middle.z)) {
                lifted_length += move.length;
                CHECK_THAT(move.z, WithinAbs(middle.z + lifted_z, z_tolerance));
            } else {
                CHECK_THAT(move.z, WithinAbs(middle.z, z_tolerance));
            }
        }
        // Inner walls 1 and 3 of the three are lifted: about two thirds of the inner wall length,
        // a little less because wall 3 is the shortest.
        REQUIRE(inner_length > 0.);
        CHECK(lifted_length / inner_length > 0.55);
        CHECK(lifted_length / inner_length < 0.75);
    }
}

TEST_CASE("Lifted walls get the flow of the height they fill", "[BrickLayers]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator)
    {
        const DynamicPrintConfig        config = brick_config(wall_generator, true);
        const std::vector<PrintedLayer> layers = printed_layers(slice({cube(cube_size)}, config), config);

        // The first layer sits on the bed and is never lifted.
        for (const WallMove &move : layer_at(layers, layer_height).walls)
            CHECK_THAT(move.z, WithinAbs(layer_height, z_tolerance));

        // The second layer starts the stack: its lifted walls reach down to the first layer's top, 1.5 layers.
        const PrintedLayer &second = layer_at(layers, 2. * layer_height);
        CHECK_THAT(lifted_inner_e_per_mm(second) / level_inner_e_per_mm(second), WithinRel(1.5, flow_tolerance));

        // In the middle a lifted wall fills one layer height, half in this layer and half in the next.
        const PrintedLayer &middle = layer_at(layers, 0.5 * cube_size);
        CHECK_THAT(lifted_inner_e_per_mm(middle) / level_inner_e_per_mm(middle), WithinRel(1.0, flow_tolerance));

        // The top layer ends the stack flush with the top of the part: nothing above it, and the
        // walls that would be lifted only fill the half layer left above the wall below.
        const PrintedLayer &top = layer_at(layers, cube_top_z);
        for (const WallMove &move : top.walls)
            CHECK_THAT(move.z, WithinAbs(cube_top_z, z_tolerance));
        CHECK_THAT(ending_to_full_flow_ratio(top), WithinRel(0.5, flow_tolerance));
    }
}

TEST_CASE("Lifted walls are printed after the rest of their layer", "[BrickLayers]")
{
    const DynamicPrintConfig        config = brick_config("classic", true);
    const std::vector<PrintedLayer> layers = printed_layers(slice({cube(cube_size)}, config), config);
    size_t                          layers_with_lifted_walls = 0;
    for (const PrintedLayer &layer : layers) {
        const auto first_lifted = std::find(layer.lifted_order.begin(), layer.lifted_order.end(), true);
        if (first_lifted == layer.lifted_order.end())
            continue;
        ++layers_with_lifted_walls;
        // Anything printed at the layer height after a lifted wall would drag the nozzle across it.
        const size_t first = size_t(first_lifted - layer.lifted_order.begin());
        for (size_t i = first; i < layer.inner_wall_order.size(); ++i)
            CHECK(layer.inner_wall_order[i]);
    }
    // Every layer but the first and the top one has lifted walls.
    CHECK(layers_with_lifted_walls == size_t(std::lround(cube_size / layer_height)) - 2);
}

TEST_CASE("Lifted walls end flush under a ledge and restart above it", "[BrickLayers]")
{
    // TestMesh::step scaled 3x in X/Y: a 60x60 base up to z=5 carrying a 54x54 column up to z=10,
    // so the base's walls stop at a 3mm ledge in the middle of the part.
    constexpr double ledge_z  = 5.0;
    constexpr double column_z = 10.0;
    TriangleMesh     step     = mesh(TestMesh::step);
    step.scale(Vec3f(3.f, 3.f, 1.f));

    const DynamicPrintConfig        config = brick_config("classic", true);
    const std::vector<PrintedLayer> layers = printed_layers(slice({step}, config), config);

    // The base's last layer: no wall continues above the ledge, so none is lifted, and the walls
    // that were lifted below it fill the remaining half layer.
    const PrintedLayer &ledge = layer_at(layers, ledge_z);
    for (const WallMove &move : ledge.walls)
        CHECK_THAT(move.z, WithinAbs(ledge_z, z_tolerance));
    CHECK_THAT(ending_to_full_flow_ratio(ledge), WithinRel(0.5, flow_tolerance));

    // The column's first layer stands on the ledge's top surface, so its lifted walls start a new stack.
    const PrintedLayer &column_start = layer_at(layers, ledge_z + layer_height);
    CHECK_THAT(lifted_inner_e_per_mm(column_start) / level_inner_e_per_mm(column_start), WithinRel(1.5, flow_tolerance));

    // Nothing is printed above the top of the part.
    for (const PrintedLayer &layer : layers)
        for (const WallMove &move : layer.walls)
            CHECK(move.z < column_z + z_tolerance);
}
