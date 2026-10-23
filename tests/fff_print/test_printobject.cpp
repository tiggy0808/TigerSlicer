#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <array>
#include <iterator>
#include <map>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Test;

// Two 20 mm cubes side by side, x 60..80 and 120..140, with `first` and `second` set on each cube.
static void two_cubes(Print &print, Model &model, std::initializer_list<ConfigBase::SetDeserializeItem> global,
                      std::vector<ConfigBase::SetDeserializeItem> first, std::vector<ConfigBase::SetDeserializeItem> second = {})
{
    TriangleMesh left = cube(20);
    left.translate(60, 80, 0);
    TriangleMesh right = cube(20);
    right.translate(120, 80, 0);
    std::vector<TriangleMesh> meshes;
    meshes.push_back(std::move(left));
    meshes.push_back(std::move(right));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2}, {"z_hop", 0}, {"skirt_loops", 0},
                                   {"brim_type", "no_brim"}, {"slow_down_for_layer_cooling", "0"}, {"filament_max_volumetric_speed", "100"}});
    config.set_deserialize_strict(global);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> per_object{std::move(first), std::move(second)};
    init_print(std::move(meshes), print, model, config, &per_object, /*arrange=*/false);
}

// Fastest wall extrusion in mm/s on each layer (0 = first) of the cube left of x = 100 ([0]) and right of it ([1]).
// Only walls: the first layer's bottom surface does not take the first layer speed.
static std::array<std::map<int, double>, 2> wall_speeds_by_layer(const std::string &gcode)
{
    std::array<std::map<int, double>, 2> speeds;
    bool wall = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&speeds, &wall](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0)
            wall = raw.find("wall") != std::string_view::npos;
        if (!wall || !line.extruding(self) || line.dist_XY(self) <= 0)
            return;
        const int    layer = int(std::lround(self.z() / 0.2)) - 1;
        const size_t cube  = 0.5 * (self.x() + line.new_X(self)) < 100. ? 0 : 1;
        double      &speed = speeds[cube][layer];
        speed              = std::max(speed, line.new_F(self) / 60.);
    });
    return speeds;
}

// TigerSlicer: these were PrintConfig (whole plate) options in OrcaSlicer.
TEST_CASE("First layer, travel and retraction settings belong to each object", "[PrintObject]")
{
    for (const char *key : {"initial_layer_line_width", "initial_layer_speed", "initial_layer_infill_speed", "slow_down_layers",
                            "reduce_crossing_wall", "max_travel_detour_distance", "reduce_infill_retraction"}) {
        CAPTURE(key);
        CHECK(PrintObjectConfig().has(key));
        CHECK_FALSE(PrintConfig().has(key));
    }
    CHECK(PrintRegionConfig().has("print_flow_ratio"));
    // What the whole plate shares stays global.
    for (const char *key : {"print_sequence", "initial_layer_print_height", "skirt_loops", "enable_prime_tower", "spiral_mode"}) {
        CAPTURE(key);
        CHECK(PrintConfig().has(key));
        CHECK_FALSE(PrintObjectConfig().has(key));
        CHECK_FALSE(PrintRegionConfig().has(key));
    }
}

TEST_CASE("Each object prints its first layer at its own speed", "[PrintObject]")
{
    Print print;
    Model model;
    two_cubes(print, model, {{"initial_layer_speed", "50"}, {"initial_layer_infill_speed", "50"}},
              {{"initial_layer_speed", "20"}, {"initial_layer_infill_speed", "20"}});
    const auto speeds = wall_speeds_by_layer(gcode(print));
    REQUIRE(speeds[0].count(0) == 1);
    REQUIRE(speeds[1].count(0) == 1);
    CHECK(speeds[0].at(0) <= 20. + EPSILON);
    CHECK(speeds[1].at(0) > 40.);
}

TEST_CASE("Each object slows down its own first layers", "[PrintObject]")
{
    Print print;
    Model model;
    two_cubes(print, model, {{"initial_layer_speed", "20"}, {"outer_wall_speed", "200"}, {"inner_wall_speed", "200"}, {"slow_down_layers", 0}},
              {{"slow_down_layers", "4"}});
    const auto speeds = wall_speeds_by_layer(gcode(print));
    REQUIRE(speeds[0].count(1) == 1);
    REQUIRE(speeds[1].count(1) == 1);
    // The second layer is a quarter of the way from the 20 mm/s first layer to the 200 mm/s walls: 65 mm/s.
    CHECK(speeds[0].at(1) < 0.5 * speeds[1].at(1));
}

TEST_CASE("Each object can have its own first layer line width", "[PrintObject]")
{
    Print print;
    Model model;
    two_cubes(print, model, {{"initial_layer_line_width", 0.5}}, {{"initial_layer_line_width", "0.8"}});
    const PrintObject &wide = *print.objects().front(), &normal = *print.objects().back();
    const PrintRegion &region = wide.printing_region(0);
    CHECK_THAT(region.flow(wide, frPerimeter, 0.2, true).width(), Catch::Matchers::WithinAbs(0.8, 1e-6));
    CHECK_THAT(region.flow(normal, frPerimeter, 0.2, true).width(), Catch::Matchers::WithinAbs(0.5, 1e-6));
}

// The skirt takes the global first layer width; no object's own settings change when only the global one does.
TEST_CASE("Changing the global first layer width redoes the skirt when every object has its own", "[PrintObject]")
{
    Print print;
    Model model;
    two_cubes(print, model, {{"initial_layer_line_width", 0.5}, {"skirt_loops", 1}}, {{"initial_layer_line_width", "0.8"}},
              {{"initial_layer_line_width", "0.8"}});
    print.process();
    REQUIRE(print.is_step_done(psSkirtBrim));
    CHECK_THAT(print.skirt_flow().width(), Catch::Matchers::WithinAbs(0.5, 1e-6));

    DynamicPrintConfig config = print.full_print_config();
    config.set_deserialize_strict("initial_layer_line_width", "0.6");
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(psSkirtBrim));
    CHECK_THAT(print.skirt_flow().width(), Catch::Matchers::WithinAbs(0.6, 1e-6));
}

SCENARIO("Object layer heights", "[PrintObject]") {
    GIVEN("A 20mm cube") {
        WHEN("sliced with a 2mm layer height and a 3mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               2 },
                { "nozzle_diameter",            3 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
            THEN("The output vector has 10 entries") {
                REQUIRE(layers.size() == 10);
            }
            AND_THEN("Each layer is approximately 2mm above the previous Z") {
                coordf_t last = 0.0;
                for (size_t i = 0; i < layers.size(); ++ i) {
                    REQUIRE_THAT(layers[i]->print_z - last, Catch::Matchers::WithinAbs(2.0, 1e-4));
                    last = layers[i]->print_z;
                }
            }
        }
        WHEN("sliced with a 10mm layer height and an 11mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               10 },
                { "nozzle_diameter",            11 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 3 entries") {
                REQUIRE(layers.size() == 3);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE_THAT(layers.front()->print_z, Catch::Matchers::WithinAbs(2.0, 1e-4));
            }
            AND_THEN("Layer 1 is at 12mm") {
                REQUIRE_THAT(layers[1]->print_z, Catch::Matchers::WithinAbs(12.0, 1e-4));
            }
        }
        WHEN("sliced with a 15mm layer height and a 16mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               15 },
                { "nozzle_diameter",            16 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 2 entries") {
                REQUIRE(layers.size() == 2);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE_THAT(layers[0]->print_z, Catch::Matchers::WithinAbs(2.0, 1e-4));
            }
            AND_THEN("Layer 1 is at 17mm") {
                REQUIRE_THAT(layers[1]->print_z, Catch::Matchers::WithinAbs(17.0, 1e-4));
            }
        }
        WHEN("layer height exceeds the nozzle diameter") {
            // Orca does not clamp an over-large layer height to the nozzle; it
            // rejects the slice during flow computation. Pin that behavior.
            THEN("Slicing is rejected") {
                Slic3r::Print print;
                REQUIRE_THROWS(Slic3r::Test::init_and_process_print({cube(20)}, print, {
                    { "initial_layer_print_height", 0.3 },
                    { "layer_height",               0.5 },
                    { "nozzle_diameter",            0.4 }
                }));
            }
        }
    }
}

SCENARIO("Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, { { "sparse_infill_density", 0 } });
			const PrintObject &object = *print.objects().front();
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
        }
        WHEN("wall_loops is set to 3")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "sparse_infill_density", 0 },
                { "wall_loops",            3 }
            });
            const PrintObject &object = *print.objects().front();
            THEN("Every layer in region 0 has 3 perimeter loops") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

TEST_CASE("Initial layer height is honored", "[PrintObject]")
{
    const std::string gcode = Slic3r::Test::slice({cube(20)}, {
        { "initial_layer_print_height", 0.3 },
        { "layer_height",               0.2 },
        { "z_hop",                      0 } // keep recorded Z equal to the printed layer height
    });

    std::set<double> layer_zs;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&layer_zs] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (line.extruding(self) && line.dist_XY(self) > 0)
            layer_zs.insert(self.z());
    });

    REQUIRE(layer_zs.size() > 1);
    REQUIRE_THAT(*layer_zs.begin(),            Catch::Matchers::WithinAbs(0.3, 1e-4));
    REQUIRE_THAT(*std::next(layer_zs.begin()), Catch::Matchers::WithinAbs(0.5, 1e-4));
}
