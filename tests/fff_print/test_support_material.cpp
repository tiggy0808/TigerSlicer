#include <catch2/catch_all.hpp>

#include <algorithm>
#include <memory>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp" // get access to init_print, etc
// After test_helpers.hpp: SupportParameters.hpp is not self-contained.
#include "libslic3r/Support/SupportFins.hpp"
#include "libslic3r/Support/SupportParameters.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}

// extrude_support once held a `static` lambda capturing `this`, so a second export in the
// same process dereferenced a returned stack frame (ASan: stack-use-after-return).
TEST_CASE("Support G-code emission survives a second slice in the same process", "[SupportMaterial][Regression]")
{
    const std::string first = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(first, "support").empty());

    const std::string second = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(second, "support").empty());
}

// One flush block and one flush multiplier per nozzle, as the GUI sizes them when the nozzle count
// changes; a single block leaves each nozzle's slice short.
static void size_flush_to_nozzles(DynamicPrintConfig& config)
{
    const size_t nozzles = config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    auto& flush = config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values;
    const std::vector<double> block = flush;
    for (size_t i = 1; i < nozzles; ++i)
        flush.insert(flush.end(), block.begin(), block.end());
    for (const char* key : {"flush_multiplier", "flush_multiplier_fast"})
        if (config.def()->get(key) != nullptr)
            config.option<ConfigOptionFloats>(key, true)->resize(nozzles);
}
// A 30 x 30 x 2 mm slab held 10 mm above the bed by a 10 x 10 mm stem: the slab hangs over the bed
// all around the stem, and fins crossing the stem end right at its walls.
static TriangleMesh support_table()
{
    TriangleMesh model = make_cube(30, 30, 2);
    model.translate(0, 0, 10); // slab z 10..12
    TriangleMesh stem = make_cube(10, 10, 11);
    stem.translate(10, 10, 0); // stem z 0..11
    model.merge(stem);
    return model;
}

static DynamicPrintConfig fin_multifilament_config(std::initializer_list<ConfigBase::SetDeserializeItem> options)
{
    DynamicPrintConfig config = Test::multifilament_config(2, options);
    config.set_num_extruders(2);
    size_flush_to_nozzles(config);
    return config;
}

// Y of the area centroid of `expolys`.
static double area_centroid_y(const ExPolygons& expolys, double& area)
{
    double sum = 0.;
    area       = 0.;
    for (const ExPolygon& expoly : expolys) {
        sum += expoly.area() * get_extents(expoly).center().y();
        area += expoly.area();
    }
    return area > 0. ? sum / area : 0.;
}

// A 20 mm cube tipped 45 degrees about X onto an edge: its lower faces are exactly 45 degrees.
static TriangleMesh cube_on_edge()
{
    TriangleMesh cube = make_cube(20, 20, 20);
    cube.translate(-10, -10, -10);
    cube.rotate_x(float(M_PI / 4));
    cube.translate(0, 0, float(-cube.bounding_box().min.z()));
    return cube;
}

TEST_CASE("Fin support is built from walls no thicker than the fin thickness", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({support_table()}, print,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"layer_height", 0.2},
                                             {"support_fin_spacing", 6},
                                             // Fins are whole two-line loops; with the 0.4 mm default line this is one loop, about 0.7 mm.
                                             {"support_fin_thickness", 0.7},
                                             {"support_fin_tine_spacing", 0},
                                         });
    size_t checked = 0;
    for (const SupportLayer* layer : print.objects().front()->support_layers()) {
        // The fin foot within 1 mm of the bed is wider by design.
        if (layer->print_z < 1.5)
            continue;
        // Any region wider than one loop (at most 0.8 mm) survives an opening by half of that width.
        CAPTURE(layer->print_z);
        CHECK(opening(layer->support_islands, float(scale_(0.5 * 0.8 + 0.05))).empty());
        ++checked;
    }
    REQUIRE(checked > 0);
}

TEST_CASE("Fin support touches the object only on tine layers", "[SupportMaterial][Fins]")
{
    const double tine_spacing = 3.;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({support_table()}, print,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"layer_height", 0.2},
                                             {"support_object_xy_distance", 0.3},
                                             {"support_fin_spacing", 4},
                                             {"support_fin_thickness", 1.2},
                                             {"support_fin_tine_spacing", tine_spacing},
                                             {"support_fin_tine_depth", 0.3},
                                         });
    const PrintObject& object        = *print.objects().front();
    const std::vector<bool> tine_row = fin_tine_layers(object);
    size_t tine_layers               = 0;
    for (const SupportLayer* layer : object.support_layers()) {
        const Layer* object_layer = object.get_layer_at_printz(layer->print_z, EPSILON);
        if (object_layer == nullptr)
            continue;
        // Ignore numerical slivers; a tine is about 0.8 x 0.3 mm = 0.24 mm^2.
        const bool touches = area(intersection_ex(layer->support_islands, object_layer->lslices)) > scale_(scale_(0.05));
        CAPTURE(layer->print_z);
        if (tine_row[object_layer->id()])
            tine_layers += touches;
        else
            CHECK_FALSE(touches);
    }
    REQUIRE(tine_layers > 0);
}

TEST_CASE("Fin support without tines never touches the object", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({support_table()}, print,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"layer_height", 0.2},
                                             {"support_fin_tine_spacing", 0},
                                         });
    const PrintObject& object = *print.objects().front();
    REQUIRE(!object.support_layers().empty());
    for (const SupportLayer* layer : object.support_layers())
        if (const Layer* object_layer = object.get_layer_at_printz(layer->print_z, EPSILON)) {
            CAPTURE(layer->print_z);
            CHECK(area(intersection_ex(layer->support_islands, object_layer->lslices)) < scale_(scale_(0.05)));
        }
}

// Normal support at a 30 degree threshold finds nothing on 45 degree faces; fins do not use the threshold.
TEST_CASE("Fin support holds a cube tipped onto an edge regardless of the overhang threshold", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({cube_on_edge()}, print,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"layer_height", 0.2},
                                             {"support_threshold_angle", 30},
                                         });
    const PrintObject& object = *print.objects().front();
    REQUIRE(!object.support_layers().empty());
    // The cube is widest at 14.1 mm (20 mm * sqrt(2) / 2); fins reach most of the way up.
    REQUIRE(object.support_layers().back()->print_z > 10.);

    // The fins cross the edge the cube rests on (along X), so each fin runs along Y.
    const SupportLayer* mid = nullptr;
    for (const SupportLayer* layer : object.support_layers())
        if (layer->print_z > 5.) {
            mid = layer;
            break;
        }
    REQUIRE(mid != nullptr);
    REQUIRE(!mid->support_islands.empty());
    for (const ExPolygon& fin : mid->support_islands) {
        const BoundingBox b = get_extents(fin);
        CHECK(b.size().y() > b.size().x());
    }
}

TEST_CASE("Fin support stops at the fin height", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({cube_on_edge()}, print,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"layer_height", 0.2},
                                             {"support_fin_height", 4},
                                         });
    const PrintObject& object = *print.objects().front();
    REQUIRE(!object.support_layers().empty());
    REQUIRE(object.support_layers().back()->print_z <= 4. + EPSILON);
}

TEST_CASE("Fin support on a raft is rejected", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube_on_edge()}, print, model,
                             {
                                 {"enable_support", 1},
                                 {"support_type", "fins"},
                                 {"support_fin_fallback", "none"},
                                 {"raft_layers", 2},
                             });
    const StringObjectException error = print.validate();
    CHECK(error.opt_key == "raft_layers");
    CHECK(error.string.find("Fin support") != std::string::npos);
}

TEST_CASE("Fin support on the lean side only keeps one side of a balanced part", "[SupportMaterial][Fins]")
{
    // The cube on its edge is balanced, so both sides hang over the bed; only the larger (here, either) side keeps fins.
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({cube_on_edge()}, print,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"layer_height", 0.2},
                                             {"support_fin_lean_side_only", 1},
                                         });
    const PrintObject& object = *print.objects().front();
    REQUIRE(!object.support_layers().empty());
    // The cube rests on an edge along X at the middle of its footprint in Y.
    double area;
    const double edge_y = area_centroid_y(object.layers().front()->lslices, area);
    size_t below = 0, above = 0;
    for (const SupportLayer* layer : object.support_layers())
        for (const ExPolygon& fin : layer->support_islands)
            ++(fin.contour.centroid().y() < edge_y ? below : above);
    CAPTURE(below, above);
    CHECK(below + above > 0);
    CHECK(std::min(below, above) == 0);
}

static double support_area_at(const PrintObject& object, double z)
{
    for (const SupportLayer* layer : object.support_layers())
        if (layer->print_z > z)
            return area(layer->support_islands);
    return 0.;
}

TEST_CASE("Fin support with cross fins adds bracing across the main fins", "[SupportMaterial][Fins]")
{
    auto slice_table = [](double cross) {
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({support_table()}, *print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", "none"},
                                                 {"layer_height", 0.2},
                                                 {"support_fin_tine_spacing", 0},
                                                 {"support_fin_cross_spacing", cross},
                                             });
        return print;
    };
    const auto plain  = slice_table(0.);
    const auto braced = slice_table(6.);
    // Halfway up the stem, the scaffold covers more area than the parallel fins alone.
    CHECK(support_area_at(*braced->objects().front(), 5.) > support_area_at(*plain->objects().front(), 5.));
}

// Support layers carrying interface extrusions.
static size_t fin_interface_layer_count(const PrintObject& object)
{
    size_t count = 0;
    for (const SupportLayer* layer : object.support_layers()) {
        const ExtrusionEntityCollection flat = layer->support_fills.flatten();
        count += std::any_of(flat.entities.begin(), flat.entities.end(),
                             [](const ExtrusionEntity* e) { return e->role() == ExtrusionRole::erSupportMaterialInterface; });
    }
    return count;
}

TEST_CASE("Fin support prints interface layers under the object only when asked", "[SupportMaterial][Fins]")
{
    auto interface_layers = [](int layers) {
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({support_table()}, print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", "none"},
                                                 {"support_fin_tine_spacing", 0},
                                                 {"layer_height", 0.2},
                                                 {"support_fin_interface_layers", layers},
                                             });
        return fin_interface_layer_count(*print.objects().front());
    };
    CHECK(interface_layers(0) == 0);
    // Two interface layers under the slab, above its Z gap.
    CHECK(interface_layers(2) >= 2);
}

// Measure the generated toolpaths, rather than only checking the configured density.
static double fin_interface_length(const PrintObject& object)
{
    double length = 0.;
    for (const SupportLayer* layer : object.support_layers()) {
        const ExtrusionEntityCollection flat = layer->support_fills.flatten();
        for (const ExtrusionEntity* entity : flat.entities)
            if (entity->role() == ExtrusionRole::erSupportMaterialInterface)
                length += entity->length();
    }
    return length;
}

TEST_CASE("fin interfaces use their own layer count and normal spacing", "[SupportMaterial][Fins]")
{
    auto slice = [](double spacing, const char* pattern, bool ironing) {
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({support_table()}, *print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", "none"},
                                                 {"layer_height", 0.2},
                                                 {"support_fin_tine_spacing", 0},
                                                 {"support_fin_interface_layers", 3},
                                                 // These normal-support settings are hidden when fins are selected.
                                                 {"support_interface_top_layers", 0},
                                                 {"support_interface_bottom_layers", 0},
                                                 {"support_interface_spacing", spacing},
                                                 {"support_interface_pattern", pattern},
                                                 {"support_ironing", ironing},
                                             });
        return print;
    };
    const auto solid          = slice(0., "rectilinear", false);
    const auto sparse         = slice(1., "rectilinear", false);
    const auto concentric     = slice(1., "concentric", false);
    const auto ironed         = slice(1., "rectilinear", true);
    const PrintObject& object = *solid->objects().front();
    REQUIRE(fin_interface_layer_count(object) == 3);
    REQUIRE(fin_interface_layer_count(*sparse->objects().front()) == 3);
    REQUIRE(fin_interface_layer_count(*concentric->objects().front()) == 3);
    CHECK(fin_interface_length(object) > 1.5 * fin_interface_length(*sparse->objects().front()));
    CHECK(fin_interface_length(*ironed->objects().front()) > 1.5 * fin_interface_length(*sparse->objects().front()));
    CHECK(fin_interface_length(*concentric->objects().front()) != fin_interface_length(*sparse->objects().front()));
    auto ironing_layers = [](const PrintObject& object) {
        size_t count = 0;
        for (const SupportLayer* layer : object.support_layers()) {
            const ExtrusionEntityCollection flat = layer->support_fills.flatten();
            count += std::any_of(flat.entities.begin(), flat.entities.end(),
                                 [](const ExtrusionEntity* entity) { return entity->role() == ExtrusionRole::erIroning; });
        }
        return count;
    };
    CHECK(ironing_layers(object) == 0);
    CHECK(ironing_layers(*ironed->objects().front()) == 1);
    // The table underside is at 10 mm; the interface must preserve the configured top gap.
    for (const SupportLayer* layer : object.support_layers())
        CHECK(layer->print_z <= 10. - object.config().support_top_z_distance.value + EPSILON);
}

TEST_CASE("fin interface flow uses the interface nozzle with normal interfaces disabled", "[SupportMaterial][Fins]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({support_table()}, print,
                                         fin_multifilament_config({
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"support_fin_tine_spacing", 0},
                                             {"layer_height", 0.2},
                                             {"support_fin_interface_layers", 2},
                                             {"support_interface_top_layers", 0},
                                             {"support_filament", 1},
                                             {"support_interface_filament", 2},
                                             {"nozzle_diameter", "0.4,0.6"},
                                             {"filament_map", "1,2"},
                                             {"filament_diameter", "1.75,1.75"},
                                         }));
    const PrintObject& object = *print.objects().front();
    CAPTURE(print.config().nozzle_diameter.values, object.config().support_interface_filament.value);
    const SupportParameters params(object);
    CHECK(params.support_material_flow.nozzle_diameter() == Catch::Approx(0.4));
    CHECK(params.support_material_interface_flow.nozzle_diameter() == Catch::Approx(0.6));
    CHECK(fin_interface_layer_count(object) == 2);
}

TEST_CASE("supplemental supports reach above a fin height limit", "[SupportMaterial][Fins]")
{
    for (const char* fallback : {"none", "normal", "tree"}) {
        CAPTURE(fallback);
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({support_table()}, print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", fallback},
                                                 {"support_fin_height", 4},
                                                 {"support_fin_interface_layers", 2},
                                                 {"support_interface_top_layers", 0},
                                                 {"support_threshold_angle", 45},
                                                 {"support_on_build_plate_only", true},
                                                 {"layer_height", 0.2},
                                             });
        const PrintObject& object = *print.objects().front();
        REQUIRE(!object.support_layers().empty());
        CHECK(object.config().support_type.value == stFins);
        CHECK(object.config().support_interface_top_layers.value == 0);
        CHECK(object.fin_support_reserved().empty());
        if (std::string(fallback) == "none") {
            CHECK(object.support_layers().back()->print_z <= 4. + EPSILON);
        } else {
            CHECK(object.support_layers().back()->print_z > 9.);
            CHECK(fin_interface_layer_count(object) > 0);
        }
        for (size_t i = 1; i < object.support_layers().size(); ++i) {
            CHECK(object.support_layers()[i]->print_z > object.support_layers()[i - 1]->print_z + EPSILON);
            if (std::string(fallback) != "none")
                CHECK(object.support_layers()[i]->lower_layer == object.support_layers()[i - 1]);
        }
    }
}

TEST_CASE("supplemental supports preserve a fin-covered region", "[SupportMaterial][Fins]")
{
    auto slice = [](const char* fallback) {
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({support_table()}, *print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", fallback},
                                                 {"support_fin_interface_layers", 2},
                                                 {"support_fin_tine_spacing", 0},
                                                 {"layer_height", 0.2},
                                             });
        return print;
    };
    const auto fins = slice("none");
    for (const char* fallback : {"normal", "tree"}) {
        CAPTURE(fallback);
        const auto combined = slice(fallback);
        const auto& object  = *combined->objects().front();
        CHECK(fin_interface_layer_count(object) >= 2);
        // No extra towers/branches in columns already supported by fins.
        CHECK(support_area_at(object, 5.) == Catch::Approx(support_area_at(*fins->objects().front(), 5.)).margin(1e-3));
    }
}

TEST_CASE("fins and supplemental supports coexist on separate overhangs", "[SupportMaterial][Fins]")
{
    TriangleMesh low = support_table();
    low.scale(Vec3f(1.f, 1.f, 0.3f));
    TriangleMesh high = support_table();
    high.translate(45., 0., 0.);
    low.merge(high);
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({low}, baseline,
                                         {
                                             {"enable_support", 1},
                                             {"support_type", "fins"},
                                             {"support_fin_fallback", "none"},
                                             {"support_fin_height", 4},
                                             {"support_fin_interface_layers", 2},
                                             {"support_fin_tine_spacing", 0},
                                             {"layer_height", 0.2},
                                         });
    auto low_fin_area = [](const PrintObject& object) {
        const Polygons left{BoundingBox(Point::new_scale(-100., -100.), Point::new_scale(0., 100.)).polygon()};
        for (const SupportLayer* layer : object.support_layers())
            if (layer->print_z > 2.)
                return area(intersection_ex(layer->support_islands, left));
        return 0.;
    };
    const double expected_fin_area = low_fin_area(*baseline.objects().front());
    REQUIRE(expected_fin_area > 0.);
    for (const char* fallback : {"normal", "tree"}) {
        CAPTURE(fallback);
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({low}, print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", fallback},
                                                 {"support_fin_height", 4},
                                                 {"support_fin_interface_layers", 2},
                                                 {"support_fin_tine_spacing", 0},
                                                 {"layer_height", 0.2},
                                             });
        const PrintObject& object = *print.objects().front();
        bool low_interface = false, high_interface = false;
        for (const SupportLayer* layer : object.support_layers()) {
            const ExtrusionEntityCollection flat = layer->support_fills.flatten();
            for (const ExtrusionEntity* entity : flat.entities)
                if (entity->role() == ExtrusionRole::erSupportMaterialInterface) {
                    low_interface |= layer->print_z > 2. && layer->print_z < 3.;
                    high_interface |= layer->print_z > 9.;
                }
        }
        CHECK(low_interface);
        CHECK(high_interface);
        CHECK(low_fin_area(object) == Catch::Approx(expected_fin_area));
        CHECK_NOTHROW(Slic3r::Test::gcode(print));
        for (size_t i = 1; i < object.support_layers().size(); ++i)
            CHECK(object.support_layers()[i]->print_z > object.support_layers()[i - 1]->print_z + EPSILON);
        // Merged layers keep their paths at the top level, each with its own role: a nested collection
        // of mixed role is skipped by the G-code's support and interface passes.
        for (size_t i = 0; i < object.support_layers().size(); ++i) {
            const SupportLayer& layer = *object.support_layers()[i];
            CAPTURE(layer.print_z);
            CHECK(layer.id() == i);
            CHECK(layer.lower_layer == (i > 0 ? object.support_layers()[i - 1] : nullptr));
            CHECK(layer.upper_layer == (i + 1 < object.support_layers().size() ? object.support_layers()[i + 1] : nullptr));
            for (const ExtrusionEntity* entity : layer.support_fills.entities)
                CHECK(entity->role() != ExtrusionRole::erMixed);
        }
    }
}

TEST_CASE("fin support tines are packed near the bed, then follow the tine spacing", "[SupportMaterial][Fins]")
{
    // The cube on its edge is 28 mm tall, sliced at 0.2 mm.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube_on_edge()}, print, model,
                             {
                                 {"layer_height", 0.2},
                                 {"initial_layer_print_height", 0.2},
                                 {"support_fin_tine_spacing", 5},
                             });
    print.process();
    const PrintObject& object    = *print.objects().front();
    const auto layers            = object.layers();
    const std::vector<bool> rows = fin_tine_layers(object);
    std::vector<double> z;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i])
            z.push_back(layers[i]->print_z);
    REQUIRE(z.size() >= 6);
    // The first tine sits just above the 1 mm foot; the gaps widen, then settle at 5 mm.
    CHECK(z.front() > 1.0);
    CHECK(z.front() < 1.5);
    CHECK(z[1] - z[0] < 1.5);
    CHECK(std::abs(z.back() - z[z.size() - 2] - 5.) < 0.25);
    // Without the extra rows near the bed, every gap is the tine spacing.
    const_cast<PrintObjectConfig&>(object.config()).support_fin_tine_base_rows.value = false;
    const std::vector<bool> plain                                                    = fin_tine_layers(object);
    CHECK(std::count(plain.begin(), plain.end(), true) < std::count(rows.begin(), rows.end(), true));
}

TEST_CASE("fin attachments use interface material without top decks", "[SupportMaterial][Fins]")
{
    for (const char* fallback : {"none", "tree"}) {
        CAPTURE(fallback);
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({support_table()}, print,
                                             fin_multifilament_config({
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", fallback},
                                                 {"layer_height", 0.2},
                                                 {"support_fin_interface_layers", 0},
                                                 {"support_interface_top_layers", 0},
                                                 {"support_fin_tine_spacing", 3},
                                                 {"support_fin_tine_depth", 0.3},
                                                 {"support_object_xy_distance", 0.3},
                                                 {"support_fin_spacing", 4},
                                                 {"support_fin_thickness", 1.6},
                                                 {"support_filament", 1},
                                                 {"support_interface_filament", 2},
                                                 {"nozzle_diameter", "0.4,0.6"},
                                                 {"filament_map", "1,2"},
                                                 {"filament_diameter", "1.75,1.75"},
                                             }));
        const PrintObject& object = *print.objects().front();
        CHECK(SupportParameters(object).support_material_interface_flow.nozzle_diameter() == Catch::Approx(0.6));
        double interface_contact_area = 0.;
        double base_contact_area      = 0.;
        for (const SupportLayer* layer : object.support_layers()) {
            const Layer* model_layer = object.get_layer_at_printz(layer->print_z, EPSILON);
            if (!model_layer)
                continue;
            const auto flat = layer->support_fills.flatten();
            for (const auto* entity : flat.entities) {
                const double contact = area(intersection_ex(entity->polygons_covered_by_width(), to_polygons(model_layer->lslices)));
                if (entity->role() == ExtrusionRole::erSupportMaterialInterface)
                    interface_contact_area += contact;
                else if (entity->role() == ExtrusionRole::erSupportMaterial)
                    base_contact_area += contact;
            }
        }
        CHECK(interface_contact_area > scale_(scale_(0.1)));
        CHECK(base_contact_area < scale_(scale_(0.01)));
        CHECK_NOTHROW(Slic3r::Test::gcode(print));
    }
}

TEST_CASE("Fin holes open the middle of the fins and keep their foot and top edge solid", "[SupportMaterial][Fins]")
{
    // A 40 mm cube tipped onto an edge stands on two big triangular fins, one under each sloping face.
    auto slice = [](bool holes) {
        TriangleMesh cube = make_cube(40, 40, 40);
        cube.translate(-20, -20, -20);
        cube.rotate_x(float(M_PI / 4));
        cube.translate(0, 0, float(-cube.bounding_box().min.z()));
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({cube}, *print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", "none"},
                                                 {"layer_height", 0.2},
                                                 {"support_fin_spacing", 8},
                                                 {"support_fin_thickness", 1.6},
                                                 {"support_fin_tine_spacing", 5},
                                                 {"support_fin_holes", holes},
                                             });
        return print;
    };
    CHECK_FALSE(DynamicPrintConfig::full_print_config().opt_bool("support_fin_holes"));

    const auto solid = slice(false);
    const auto holed = slice(true);
    const auto solid_layers = solid->objects().front()->support_layers();
    const auto holed_layers = holed->objects().front()->support_layers();
    REQUIRE(!solid_layers.empty());
    REQUIRE(solid_layers.size() == holed_layers.size());

    const double top_z = solid_layers.back()->print_z;
    double solid_area = 0., holed_area = 0.;
    size_t split_layers = 0;
    for (size_t i = 0; i < solid_layers.size(); ++i) {
        const SupportLayer& s = *solid_layers[i];
        const SupportLayer& h = *holed_layers[i];
        CAPTURE(s.print_z);
        const double a_s = area(s.support_islands), a_h = area(h.support_islands);
        solid_area += a_s;
        holed_area += a_h;
        // Holes only take material away.
        CHECK(a_h <= a_s * (1. + 1e-9));
        // The 1 mm foot and the 2.5 mm frame above it, and the top edge of the fins under the cube, stay whole.
        if (s.print_z < 1. + 2.5 || s.print_z > top_z - 2.)
            CHECK_THAT(a_h, Catch::Matchers::WithinRel(a_s, 1e-9));
        if (h.support_islands.size() > s.support_islands.size())
            ++split_layers;
        // No sliver of fin is left between holes: every piece is at least about two lines wide. (The apex
        // of each triangular fin, in its top 2 mm, is narrow in the solid fins too.)
        if (s.print_z < top_z - 2.)
            for (const ExPolygon& piece : h.support_islands)
                CHECK_FALSE(opening(ExPolygons{piece}, float(scale_(0.35))).empty());
    }
    // A good share of each fin is open, cut into struts on many layers.
    CAPTURE(holed_area / solid_area);
    CHECK(holed_area < 0.85 * solid_area);
    CHECK(holed_area > 0.3 * solid_area);
    CHECK(split_layers > 20);
}

TEST_CASE("Fin hole lattice repeats every pitch and is about half open", "[SupportMaterial][Fins]")
{
    const double pitch = 8., strut = 1.6;
    auto open_length = [&](double z) {
        double len = 0.;
        for (const auto& [start, end] : fin_hole_intervals(z, pitch, strut)) {
            CHECK(start >= 0.);
            CHECK(end <= pitch);
            CHECK(start < end);
            len += end - start;
        }
        return len;
    };
    double sum = 0.;
    const int samples = 400;
    for (int k = 0; k < samples; ++k) {
        const double z = -20. + 40. * k / samples;
        CAPTURE(z);
        // The lattice is the same one pitch higher or lower, below z = 0 too.
        CHECK_THAT(open_length(z), Catch::Matchers::WithinAbs(open_length(z + pitch), 1e-9));
        CHECK_THAT(open_length(z), Catch::Matchers::WithinAbs(open_length(z - 2. * pitch), 1e-9));
        sum += open_length(z);
    }
    // Two strut families each leave (pitch - strut * sqrt(2)) / pitch open; their crossing leaves the product.
    const double expected = std::pow((pitch - strut * std::sqrt(2.)) / pitch, 2.);
    CHECK_THAT(sum / (samples * pitch), Catch::Matchers::WithinAbs(expected, 0.03));
    // Struts wider than half the pitch leave nothing open.
    CHECK(fin_hole_intervals(1., pitch, 6.).empty());
}

TEST_CASE("Fin holes stay clear of a flat underside", "[SupportMaterial][Fins]")
{
    // A 50 x 50 x 2 mm slab held 20 mm up by a 10 x 10 mm stem: the fins under it are rectangles.
    auto slice = [](bool holes) {
        TriangleMesh table = make_cube(50, 50, 2);
        table.translate(0, 0, 20);
        TriangleMesh stem = make_cube(10, 10, 21);
        stem.translate(20, 20, 0);
        table.merge(stem);
        auto print = std::make_unique<Slic3r::Print>();
        Slic3r::Test::init_and_process_print({table}, *print,
                                             {
                                                 {"enable_support", 1},
                                                 {"support_type", "fins"},
                                                 {"support_fin_fallback", "none"},
                                                 {"layer_height", 0.2},
                                                 {"support_top_z_distance", 0.2},
                                                 {"support_fin_spacing", 8},
                                                 {"support_fin_thickness", 1.6},
                                                 {"support_fin_tine_spacing", 0},
                                                 {"support_fin_holes", holes},
                                             });
        return print;
    };
    const auto solid = slice(false);
    const auto holed = slice(true);
    const auto solid_layers = solid->objects().front()->support_layers();
    const auto holed_layers = holed->objects().front()->support_layers();
    REQUIRE(!solid_layers.empty());
    REQUIRE(solid_layers.size() == holed_layers.size());
    double solid_area = 0., holed_area = 0.;
    for (size_t i = 0; i < solid_layers.size(); ++i) {
        const double z = solid_layers[i]->print_z;
        CAPTURE(z);
        const double a_s = area(solid_layers[i]->support_islands), a_h = area(holed_layers[i]->support_islands);
        solid_area += a_s;
        holed_area += a_h;
        // The 2.5 mm under the slab (below its 0.2 mm gap) carries it and stays whole.
        if (z > 20. - 0.2 - 2.5 + EPSILON)
            CHECK_THAT(a_h, Catch::Matchers::WithinRel(a_s, 1e-9));
    }
    CHECK(holed_area < 0.9 * solid_area);
}
