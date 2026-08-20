#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "test_data.hpp"
#include "libslic3r/GCodeReader.hpp"

#include <set>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Approx;

namespace {

DynamicPrintConfig four_tool_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.2, 0.6, 0.8 });
    config.set_key_value("min_layer_height", new ConfigOptionFloats{ 0.05, 0.02, 0.10, 0.10 });
    config.set_key_value("max_layer_height", new ConfigOptionFloats{ 0.30, 0.15, 0.45, 0.60 });
    config.set_key_value("extruder_preferred_layer_height", new ConfigOptionFloats{ 0.20, 0.10, 0.30, 0.40 });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 0, 0, 0, 0 });
    config.set_key_value("multi_rate_layer_height_capable", new ConfigOptionBool{ true });
    config.set_key_value("independent_tool_layer_heights", new ConfigOptionBool{ true });
    config.set_key_value("first_layer_height", new ConfigOptionFloatOrPercent{ 0.2, false });
    config.set_key_value("layer_height", new ConfigOptionFloat{ 0.2 });
    config.set_key_value("support_material", new ConfigOptionBool{ false });
    config.set_key_value("raft_layers", new ConfigOptionInt{ 0 });
    return config;
}

void collect_path_heights(const ExtrusionEntity &entity, ExtrusionRole role, std::vector<double> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_path_heights(*child, role, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            if (path.role() == role)
                out.push_back(path.height());
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            if (path.role() == role)
                out.push_back(path.height());
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity); path != nullptr && path->role() == role) {
        out.push_back(path->height());
    }
}

bool has_nonbridging_path_above(const ExtrusionEntity &entity, ExtrusionRole role, double maximum_height)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        return std::any_of(collection->entities.begin(), collection->entities.end(),
            [role, maximum_height](const ExtrusionEntity *child) {
                return has_nonbridging_path_above(*child, role, maximum_height);
            });
    }
    auto exceeds = [role, maximum_height](const ExtrusionPath &path) {
        // Native bridge flow is circular (width == height == bridge diameter),
        // so its visualized height is not a scheduled planar layer height.
        return path.role() == role && path.width() > path.height() + EPSILON &&
               path.height() > maximum_height + EPSILON;
    };
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
        return exceeds(*path);
    if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        return std::any_of(multipath->paths.begin(), multipath->paths.end(), exceeds);
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        return std::any_of(loop->paths.begin(), loop->paths.end(), exceeds);
    return false;
}

void collect_path_flows(const ExtrusionEntity &entity, ExtrusionRole role,
                        std::vector<std::pair<double, double>> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_path_flows(*child, role, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            if (path.role() == role)
                out.emplace_back(path.height(), path.width());
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            if (path.role() == role)
                out.emplace_back(path.height(), path.width());
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity); path != nullptr && path->role() == role) {
        out.emplace_back(path->height(), path->width());
    }
}

}

TEST_CASE("A single-tool object uses that tool's preferred slicing grid", "[independent-tool-layers]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 3 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 3 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 3 });

    Print print;
    Model model;
    Test::init_print({ make_cube(2., 2., 2.) }, print, model, config);

    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().size() == 1);
    CHECK(print.objects().front()->slicing_parameters().first_print_layer_height == Approx(0.2));
    CHECK(print.objects().front()->slicing_parameters().layer_height == Approx(0.3));
}

TEST_CASE("An interacting multi-tool object uses synchronization planes", "[independent-tool-layers]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 15. });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 5.) }, print, model, config);

    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().size() == 1);
    CHECK(print.objects().front()->slicing_parameters().layer_height == Approx(0.2));

    print.process();
    bool found_combined_sparse_infill = false;
    std::ostringstream sparse_infill_heights;
    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<const ExtrusionEntity *> queue(layer_region->fills().entities.begin(), layer_region->fills().entities.end());
            while (! queue.empty()) {
                const ExtrusionEntity *entity = queue.back();
                queue.pop_back();
                if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
                    queue.insert(queue.end(), collection->entities.begin(), collection->entities.end());
                } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
                    if (path->role() == ExtrusionRole::InternalInfill)
                        sparse_infill_heights << path->height() << ',';
                    found_combined_sparse_infill |= path->role() == ExtrusionRole::InternalInfill && path->height() == Approx(0.4);
                } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
                    for (const ExtrusionPath &path : multipath->paths) {
                        if (path.role() == ExtrusionRole::InternalInfill)
                            sparse_infill_heights << path.height() << ',';
                        found_combined_sparse_infill |= path.role() == ExtrusionRole::InternalInfill && path.height() == Approx(0.4);
                    }
                } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
                    for (const ExtrusionPath &path : loop->paths) {
                        if (path.role() == ExtrusionRole::InternalInfill)
                            sparse_infill_heights << path.height() << ',';
                        found_combined_sparse_infill |= path.role() == ExtrusionRole::InternalInfill && path.height() == Approx(0.4);
                    }
                }
            }
        }
    }
    INFO(sparse_infill_heights.str());
    CHECK(found_combined_sparse_infill);
}

TEST_CASE("Unsafe modes produce clear independent-height validation errors", "[independent-tool-layers]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("support_material", new ConfigOptionBool{ true });
    config.set_key_value("raft_layers", new ConfigOptionInt{ 1 });

    Print print;
    Model model;
    Test::init_print({ make_cube(2., 2., 2.) }, print, model, config);

    const std::string validation_error = print.validate();
    INFO(validation_error);
    CHECK(validation_error.find("raft") != std::string::npos);
}

TEST_CASE("Stable T0 walls retain a 0.20 mm grid beside 0.10 mm T1 features", "[independent-tool-layers][multi-rate-walls]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("extruder_independent_walls", new ConfigOptionBools{ true, false, false, false });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 2, 1, 0, 0 });
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 2 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 15. });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 2.) }, print, model, config);
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().front()->slicing_parameters().layer_height == Approx(0.1));

    print.process();
    bool found_coarse_wall_above_first_layer = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<double> heights;
            collect_path_heights(layer_region->perimeters(), ExtrusionRole::ExternalPerimeter, heights);
            for (double height : heights)
                if (layer->id() > 0 && height == Approx(0.2))
                    found_coarse_wall_above_first_layer = true;
        }
    CHECK(found_coarse_wall_above_first_layer);
}

TEST_CASE("Wall permission disabled preserves fine synchronization walls", "[independent-tool-layers][multi-rate-walls]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("extruder_independent_walls", new ConfigOptionBools{ false, false, false, false });
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 2 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 15. });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 2.) }, print, model, config);
    REQUIRE(print.validate().empty());
    print.process();

    bool found_fine_wall = false;
    for (const Layer *layer : print.objects().front()->layers())
        if (layer->id() > 0)
            for (const LayerRegion *layer_region : layer->regions()) {
                std::vector<double> heights;
                collect_path_heights(layer_region->perimeters(), ExtrusionRole::ExternalPerimeter, heights);
                found_fine_wall |= std::any_of(heights.begin(), heights.end(), [](double height) { return height == Approx(0.1); });
            }
    CHECK(found_fine_wall);
}

TEST_CASE("Coarse internal solid infill stops below fine top surfaces", "[independent-tool-layers][multi-rate-solid]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("extruder_independent_solid_infill", new ConfigOptionBools{ false, false, false, true });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 0, 1, 0, 4 });
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 2 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 15. });
    config.set_key_value("top_solid_layers", new ConfigOptionInt{ 10 });
    config.set_key_value("top_solid_min_thickness", new ConfigOptionFloat{ 1.0 });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 3.) }, print, model, config);
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().front()->slicing_parameters().layer_height == Approx(0.1));
    print.process();

    bool found_coarse_internal_solid = false;
    bool found_top_surface = false;
    bool top_surface_exceeded_sync_height = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<double> solid_heights;
            collect_path_heights(layer_region->fills(), ExtrusionRole::SolidInfill, solid_heights);
            found_coarse_internal_solid |= std::any_of(solid_heights.begin(), solid_heights.end(),
                [](double height) { return height == Approx(0.4); });

            std::vector<double> top_heights;
            collect_path_heights(layer_region->fills(), ExtrusionRole::TopSolidInfill, top_heights);
            found_top_surface |= ! top_heights.empty();
            top_surface_exceeded_sync_height |= std::any_of(top_heights.begin(), top_heights.end(),
                [](double height) { return height > 0.1 + EPSILON; });
        }
    CHECK(found_coarse_internal_solid);
    CHECK(found_top_surface);
    CHECK_FALSE(top_surface_exceeded_sync_height);
}

TEST_CASE("Traditional support bases coarsen while interfaces align to fine object planes", "[independent-tool-layers][multi-rate-support]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 2 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("support_material", new ConfigOptionBool{ true });
    config.set_key_value("support_material_auto", new ConfigOptionBool{ true });
    config.set_key_value("support_material_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("support_material_interface_extruder", new ConfigOptionInt{ 2 });
    config.set_key_value("support_material_interface_layers", new ConfigOptionInt{ 3 });
    config.set_key_value("extruder_independent_support", new ConfigOptionBools{ false, false, false, true });
    config.set_key_value("extruder_independent_support_interface", new ConfigOptionBools{ false, true, false, false });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 0, 1, 0, 4 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 15. });

    Print print;
    Model model;
    TriangleMesh supported_mesh = Test::mesh(TestMesh::cube_with_hole);
    supported_mesh.rotate_x(float(M_PI / 2.));
    config.set_key_value("dont_support_bridges", new ConfigOptionBool{ false });
    Test::init_print({ std::move(supported_mesh) }, print, model, config);
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    print.process();

    bool found_coarse_support = false;
    bool found_fine_interface = false;
    bool interface_exceeded_tool_maximum = false;
    std::ostringstream support_schedule;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        std::vector<double> support_heights;
        collect_path_heights(layer->support_fills, ExtrusionRole::SupportMaterial, support_heights);
        found_coarse_support |= std::any_of(support_heights.begin(), support_heights.end(),
            [](double height) { return height > 0.1 + EPSILON; });

        std::vector<double> interface_heights;
        collect_path_heights(layer->support_fills, ExtrusionRole::SupportMaterialInterface, interface_heights);
        found_fine_interface |= std::any_of(interface_heights.begin(), interface_heights.end(),
            [](double height) { return height <= 0.1 + EPSILON; });
        interface_exceeded_tool_maximum |= has_nonbridging_path_above(
            layer->support_fills, ExtrusionRole::SupportMaterialInterface, 0.15);
        if (! support_heights.empty() || ! interface_heights.empty()) {
            support_schedule << "z=" << layer->print_z << " layer=" << layer->height << " base=";
            for (double height : support_heights)
                support_schedule << height << ',';
            support_schedule << " interface=";
            for (double height : interface_heights)
                support_schedule << height << ',';
            support_schedule << ';';
        }
    }
    INFO(support_schedule.str());
    CHECK(found_coarse_support);
    CHECK(found_fine_interface);
    CHECK_FALSE(interface_exceeded_tool_maximum);
}

TEST_CASE("T3 sparse infill uses a native 0.60 mm three-to-one slab", "[independent-tool-layers][multi-rate-infill]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("max_layer_height", new ConfigOptionFloats{ 0.30, 0.15, 0.45, 0.80 });
    config.set_key_value("extruder_preferred_layer_height", new ConfigOptionFloats{ 0.20, 0.10, 0.30, 0.60 });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 1, 0, 0, 3 });
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 15. });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 3.) }, print, model, config);
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    print.process();

    bool found_native_slab = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<double> heights;
            collect_path_heights(layer_region->fills(), ExtrusionRole::InternalInfill, heights);
            found_native_slab |= std::any_of(heights.begin(), heights.end(),
                [](double height) { return height == Approx(0.60); });
        }
    CHECK(found_native_slab);
}

TEST_CASE("Separated objects retain independent physical tool grids", "[independent-tool-layers][multi-rate-objects]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("extruder_clearance_radius", new ConfigOptionFloat{ 1.0 });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 2, 1, 3, 4 });

    Model model;
    ModelObject *fine_object = model.add_object();
    fine_object->name = "fine-object";
    ModelVolume *fine_volume = fine_object->add_volume(make_cube(10., 10., 2.));
    fine_volume->config.set("extruder", 2);
    fine_object->add_instance()->set_offset(Vec3d(40., 100., 0.));
    fine_object->ensure_on_bed();

    ModelObject *coarse_object = model.add_object();
    coarse_object->name = "coarse-object";
    ModelVolume *coarse_volume = coarse_object->add_volume(make_cube(10., 10., 2.));
    coarse_volume->config.set("extruder", 4);
    coarse_object->add_instance()->set_offset(Vec3d(160., 100., 0.));
    coarse_object->ensure_on_bed();

    Print print;
    print.apply(model, config);
    print.set_status_silent();
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().size() == 2);

    std::set<coord_t> native_heights;
    for (const PrintObject *object : print.objects())
        native_heights.insert(scaled<coord_t>(object->slicing_parameters().layer_height));
    CHECK(native_heights == std::set<coord_t>{ scaled<coord_t>(0.10), scaled<coord_t>(0.40) });
    for (const PrintObject *object : print.objects()) {
        const unsigned int tool_id = object->object_extruders().front();
        const double first_height = object->slicing_parameters().first_print_layer_height;
        CHECK(first_height >= config.option<ConfigOptionFloats>("min_layer_height")->get_at(tool_id) - EPSILON);
        CHECK(first_height <= config.option<ConfigOptionFloats>("max_layer_height")->get_at(tool_id) + EPSILON);
    }

    print.process();
    const auto fine_it = std::find_if(print.objects().begin(), print.objects().end(),
        [](const PrintObject *object) { return object->model_object()->name == "fine-object"; });
    const auto coarse_it = std::find_if(print.objects().begin(), print.objects().end(),
        [](const PrintObject *object) { return object->model_object()->name == "coarse-object"; });
    REQUIRE(fine_it != print.objects().end());
    REQUIRE(coarse_it != print.objects().end());
    CHECK((*fine_it)->layer_count() > (*coarse_it)->layer_count());
}

TEST_CASE("Touching multi-material volumes share a fine interface grid", "[independent-tool-layers][multi-rate-material-interface]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("extruder_independent_walls", new ConfigOptionBools{ true, false, false, false });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 2, 1, 0, 0 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 0. });

    Model model;
    ModelObject *object = model.add_object();
    object->name = "stacked-material-interface";
    ModelVolume *coarse_volume = object->add_volume(make_cube(10., 10., 2.));
    coarse_volume->set_material_id("coarse-material");
    coarse_volume->config.set("extruder", 1);
    ModelVolume *fine_volume = object->add_volume(make_cube(10., 10., 2.));
    fine_volume->set_material_id("fine-material");
    fine_volume->translate(0., 0., 2.);
    fine_volume->config.set("extruder", 2);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    Print print;
    print.apply(model, config);
    print.set_status_silent();
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().front()->slicing_parameters().layer_height == Approx(0.10));
    print.process();

    bool found_coarse_wall = false;
    bool found_fine_wall = false;
    bool interface_plane_present = false;
    for (const Layer *layer : print.objects().front()->layers()) {
        interface_plane_present |= layer->print_z == Approx(2.0);
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<double> heights;
            collect_path_heights(layer_region->perimeters(), ExtrusionRole::ExternalPerimeter, heights);
            const int tool = layer_region->region().config().perimeter_extruder.value;
            if (tool == 1)
                found_coarse_wall |= std::any_of(heights.begin(), heights.end(),
                    [](double height) { return height == Approx(0.20); });
            if (tool == 2)
                found_fine_wall |= std::any_of(heights.begin(), heights.end(),
                    [](double height) { return height == Approx(0.10); });
        }
    }
    CHECK(interface_plane_present);
    CHECK(found_coarse_wall);
    CHECK(found_fine_wall);
}

TEST_CASE("Generated G-code is monotonic and uses active-tool flow limits", "[independent-tool-layers][multi-rate-gcode]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 1, 0, 0, 2 });
    config.set_key_value("extruder_independent_walls", new ConfigOptionBools{ true, false, false, false });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 20. });
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{ 50., 50., 50., 1.0 });
    config.set_key_value("perimeter_speed", new ConfigOptionFloat{ 120. });
    config.set_key_value("infill_speed", new ConfigOptionFloat{ 120. });
    config.set_key_value("start_gcode", new ConfigOptionString{ "" });
    config.set_key_value("end_gcode", new ConfigOptionString{ "" });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 3.) }, print, model, config);
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    print.process();

    const MultiRateSchedule &schedule = print.multi_rate_schedule();
    REQUIRE_FALSE(schedule.operations().empty());
    std::string schedule_error;
    const std::vector<size_t> operation_order = schedule.ordered_operation_ids(&schedule_error);
    INFO(schedule_error);
    REQUIRE(schedule_error.empty());
    REQUIRE(operation_order.size() == schedule.operations().size());
    bool schedule_has_t0_walls = false;
    bool schedule_has_t3_infill = false;
    double preceding_operation_z = 0.;
    for (size_t operation_id : operation_order) {
        const MultiRateOperation &operation = schedule.operations()[operation_id];
        CHECK(operation.print_z + EPSILON >= preceding_operation_z);
        preceding_operation_z = operation.print_z;
        schedule_has_t0_walls |= operation.tool_id == 0 && operation.feature == MultiRateFeature::Walls &&
                                 operation.extrusion_height == Approx(0.20);
        schedule_has_t3_infill |= operation.tool_id == 3 && operation.feature == MultiRateFeature::SparseInfill &&
                                  operation.extrusion_height == Approx(0.40);
    }
    CHECK(schedule_has_t0_walls);
    CHECK(schedule_has_t3_infill);

    bool found_t0_wall_width = false;
    bool found_t3_infill_width = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<std::pair<double, double>> walls;
            collect_path_flows(layer_region->perimeters(), ExtrusionRole::ExternalPerimeter, walls);
            found_t0_wall_width |= std::any_of(walls.begin(), walls.end(),
                [](const auto &flow) { return flow.second >= 0.35 && flow.second <= 0.60; });
            std::vector<std::pair<double, double>> infill;
            collect_path_flows(layer_region->fills(), ExtrusionRole::InternalInfill, infill);
            found_t3_infill_width |= std::any_of(infill.begin(), infill.end(),
                [](const auto &flow) { return flow.first > 0.20 && flow.second >= 0.75; });
        }
    CHECK(found_t0_wall_width);
    CHECK(found_t3_infill_width);

    const std::string gcode = Test::gcode(print);
    GCodeReader reader;
    reader.apply_config(config);
    int active_tool = -1;
    size_t tool_commands = 0;
    size_t t3_extrusions = 0;
    bool downward_extrusion = false;
    bool t3_exceeded_volumetric_limit = false;
    double last_extrusion_z = 0.;
    const double filament_area = M_PI * 1.75 * 1.75 / 4.;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.cmd().empty() && line.cmd().front() == 'T') {
            active_tool = atoi(line.cmd().data() + 1);
            ++ tool_commands;
        } else if (line.extruding(self) && line.dist_XY(self) > 0.) {
            const double extrusion_z = line.new_Z(self);
            downward_extrusion |= extrusion_z + EPSILON < last_extrusion_z;
            last_extrusion_z = extrusion_z;
            if (active_tool == 3) {
                ++ t3_extrusions;
                const double speed = line.new_F(self) / 60.;
                const double volumetric_rate = line.dist_E(self) / line.dist_XY(self) * filament_area * speed;
                t3_exceeded_volumetric_limit |= volumetric_rate > 1.01;
            }
        }
    });
    INFO("tool commands=" << tool_commands);
    CHECK_FALSE(downward_extrusion);
    CHECK(t3_extrusions > 0);
    CHECK_FALSE(t3_exceeded_volumetric_limit);
    CHECK(tool_commands < print.objects().front()->layer_count());
}

TEST_CASE("Unsupported multi-rate feature combinations fail explicitly", "[independent-tool-layers][multi-rate-validation]")
{
    SECTION("non-integral interacting heights") {
        DynamicPrintConfig config = four_tool_config();
        config.set_key_value("extruder_preferred_layer_height", new ConfigOptionFloats{ 0.20, 0.12, 0.30, 0.40 });
        config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
        config.set_key_value("infill_extruder", new ConfigOptionInt{ 2 });
        config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });

        Print print;
        Model model;
        Test::init_print({ make_cube(10., 10., 2.) }, print, model, config);
        const std::string error = print.validate();
        INFO(error);
        CHECK(error.find("integer multiples") != std::string::npos);
    }

    SECTION("variable layer-height profile") {
        DynamicPrintConfig config = four_tool_config();
        Print print;
        Model model;
        Test::init_print({ make_cube(10., 10., 2.) }, print, model, config);
        model.objects.front()->layer_height_profile.set({ 0., 0.10, 2., 0.15 });
        print.apply(model, config);
        const std::string error = print.validate();
        INFO(error);
        CHECK(error.find("variable layer height") != std::string::npos);
    }

    SECTION("coarse top surfaces") {
        DynamicPrintConfig config = four_tool_config();
        config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
        config.set_key_value("infill_extruder", new ConfigOptionInt{ 2 });
        config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
        config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 2, 1, 0, 0 });
        config.set_key_value("extruder_independent_top_surfaces", new ConfigOptionBools{ true, false, false, false });

        Print print;
        Model model;
        Test::init_print({ make_cube(10., 10., 2.) }, print, model, config);
        const std::string error = print.validate();
        INFO(error);
        CHECK(error.find("top-surface") != std::string::npos);
    }

    SECTION("soluble support interfaces") {
        DynamicPrintConfig config = four_tool_config();
        config.set_key_value("support_material", new ConfigOptionBool{ true });
        config.set_key_value("support_material_extruder", new ConfigOptionInt{ 4 });
        config.set_key_value("support_material_interface_extruder", new ConfigOptionInt{ 2 });
        config.set_key_value("filament_soluble", new ConfigOptionBools{ false, true, false, false });

        Print print;
        Model model;
        Test::init_print({ make_cube(10., 10., 2.) }, print, model, config);
        const std::string error = print.validate();
        INFO(error);
        CHECK(error.find("soluble support interfaces") != std::string::npos);
    }

    SECTION("organic supports") {
        DynamicPrintConfig config = four_tool_config();
        config.set_key_value("support_material", new ConfigOptionBool{ true });
        config.set_key_value("support_material_extruder", new ConfigOptionInt{ 4 });
        config.set_key_value("support_material_interface_extruder", new ConfigOptionInt{ 2 });
        config.set_key_value("support_material_style", new ConfigOptionEnum<SupportMaterialStyle>{ smsOrganic });

        Print print;
        Model model;
        Test::init_print({ make_cube(10., 10., 2.) }, print, model, config);
        const std::string error = print.validate();
        INFO(error);
        CHECK(error.find("tree and organic") != std::string::npos);
    }
}

TEST_CASE("Modifier-owned fine regions remain on synchronization planes", "[independent-tool-layers][multi-rate-modifier]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("extruder_independent_walls", new ConfigOptionBools{ true, false, false, false });
    config.set_key_value("extruder_layer_height_ratio", new ConfigOptionInts{ 2, 1, 0, 0 });

    Model model;
    ModelObject *object = model.add_object();
    object->name = "modifier-owned-fine-region";
    ModelVolume *base = object->add_volume(make_cube(20., 20., 3.));
    base->config.set("extruder", 1);
    TriangleMesh modifier_mesh = make_cube(8., 8., 2.);
    modifier_mesh.translate(Vec3f(6.f, 6.f, 0.5f));
    ModelVolume *modifier = object->add_volume(std::move(modifier_mesh), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set("extruder", 2);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    Print print;
    print.apply(model, config);
    print.set_status_silent();
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE(print.objects().front()->slicing_parameters().layer_height == Approx(0.10));
    print.process();

    std::set<int> perimeter_tools;
    for (const std::reference_wrapper<const PrintRegion> region : print.objects().front()->all_regions())
        perimeter_tools.insert(region.get().config().perimeter_extruder.value);
    CHECK(perimeter_tools.count(1) == 1);
    CHECK(perimeter_tools.count(2) == 1);
}

TEST_CASE("Feature disabled preserves the shared-height slicing path", "[independent-tool-layers][multi-rate-compatibility]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("independent_tool_layer_heights", new ConfigOptionBool{ false });
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 20. });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 2.) }, print, model, config);
    REQUIRE(print.validate().empty());
    REQUIRE(print.objects().front()->slicing_parameters().layer_height == Approx(0.20));
    print.process();

    bool found_infill = false;
    bool found_coarse_infill = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<double> heights;
            collect_path_heights(layer_region->fills(), ExtrusionRole::InternalInfill, heights);
            found_infill |= ! heights.empty();
            found_coarse_infill |= std::any_of(heights.begin(), heights.end(),
                [](double height) { return height > 0.20 + EPSILON; });
        }
    CHECK(found_infill);
    CHECK_FALSE(found_coarse_infill);
}

TEST_CASE("Ironing skirt and brim remain on synchronized planes", "[independent-tool-layers][multi-rate-adhesion]")
{
    DynamicPrintConfig config = four_tool_config();
    config.set_key_value("perimeter_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("infill_extruder", new ConfigOptionInt{ 4 });
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt{ 1 });
    config.set_key_value("fill_density", new ConfigOptionPercent{ 20. });
    config.set_key_value("skirts", new ConfigOptionInt{ 1 });
    config.set_key_value("brim_width", new ConfigOptionFloat{ 2. });
    config.set_key_value("ironing", new ConfigOptionBool{ true });

    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 2.) }, print, model, config);
    const std::string validation_error = print.validate();
    INFO(validation_error);
    REQUIRE(validation_error.empty());
    REQUIRE_NOTHROW(print.process());
    CHECK_FALSE(print.skirt().empty());
    CHECK_FALSE(print.brim().empty());

    bool found_ironing = false;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layer_region : layer->regions()) {
            std::vector<double> heights;
            collect_path_heights(layer_region->fills(), ExtrusionRole::Ironing, heights);
            found_ironing |= ! heights.empty();
        }
    CHECK(found_ironing);
}
