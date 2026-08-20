#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/MultiRateLayerSchedule.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

#include <algorithm>

using namespace Slic3r;
using Catch::Approx;

namespace {

PrintConfig four_tool_config()
{
    PrintConfig config;
    config.nozzle_diameter.values                  = { 0.4, 0.2, 0.6, 0.8 };
    config.min_layer_height.values                 = { 0.05, 0.02, 0.10, 0.10 };
    config.max_layer_height.values                 = { 0.30, 0.15, 0.45, 0.60 };
    config.extruder_preferred_layer_height.values  = { 0.20, 0.10, 0.30, 0.40 };
    config.extruder_layer_height_ratio.values      = { 0, 0, 0, 0 };
    config.extruder_independent_walls.values       = { 1, 1, 1, 1 };
    config.extruder_independent_infill.values      = { 1, 1, 1, 1 };
    config.extruder_independent_solid_infill.values = { 0, 0, 0, 0 };
    config.extruder_independent_top_surfaces.values = { 0, 0, 0, 0 };
    config.extruder_independent_support.values      = { 0, 0, 0, 0 };
    config.extruder_independent_support_interface.values = { 0, 0, 0, 0 };
    config.multi_rate_layer_height_capable.value   = true;
    return config;
}

}

TEST_CASE("Independent tool layer scheduling is opt-in", "[independent-tool-layers]")
{
    PrintConfig config = four_tool_config();
    config.independent_tool_layer_heights.value = false;

    CHECK(Slicing::preferred_layer_height(config, 2, 0.2) == Approx(0.2));
    CHECK(Slicing::object_layer_height(config, { 0, 2 }, 0.2) == Approx(0.2));
}

TEST_CASE("Four different nozzle sizes resolve preferred and synchronized heights", "[independent-tool-layers]")
{
    PrintConfig config = four_tool_config();
    config.independent_tool_layer_heights.value = true;

    CHECK(Slicing::object_layer_height(config, { 0 }, 0.2) == Approx(0.20));
    CHECK(Slicing::object_layer_height(config, { 1 }, 0.2) == Approx(0.10));
    CHECK(Slicing::object_layer_height(config, { 2 }, 0.2) == Approx(0.30));
    CHECK(Slicing::object_layer_height(config, { 3 }, 0.2) == Approx(0.40));

    const double synchronized = Slicing::object_layer_height(config, { 1, 2 }, 0.2);
    CHECK(synchronized == Approx(0.10));
    CHECK(Slicing::layer_height_ratio_is_integral(0.30, synchronized));
    CHECK_FALSE(Slicing::layer_height_ratio_is_integral(0.25, synchronized));
}

TEST_CASE("A zero preferred height inherits the ordinary print height", "[independent-tool-layers]")
{
    PrintConfig config = four_tool_config();
    config.independent_tool_layer_heights.value = true;
    config.extruder_preferred_layer_height.values[1] = 0.;

    config.extruder_layer_height_ratio.values[1] = 0;
    CHECK(Slicing::preferred_layer_height(config, 1, 0.12) == Approx(0.12));
    CHECK(Slicing::object_layer_height(config, { 1, 3 }, 0.12) == Approx(0.12));
}

TEST_CASE("Preferred layer-height vector follows physical extruder count", "[independent-tool-layers]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);

    REQUIRE(config.option<ConfigOptionFloats>("extruder_preferred_layer_height") != nullptr);
    CHECK(config.option<ConfigOptionFloats>("extruder_preferred_layer_height")->values.size() == 4);
    REQUIRE(config.option<ConfigOptionInts>("extruder_layer_height_ratio") != nullptr);
    CHECK(config.option<ConfigOptionInts>("extruder_layer_height_ratio")->values.size() == 4);
    REQUIRE(config.option<ConfigOptionBools>("extruder_independent_infill") != nullptr);
    CHECK(config.option<ConfigOptionBools>("extruder_independent_infill")->values.size() == 4);
}

TEST_CASE("Independent layer-height settings are stored in their preset categories", "[independent-tool-layers]")
{
    const std::vector<std::string> &print_options = Preset::print_options();
    CHECK(std::find(print_options.begin(), print_options.end(), "independent_tool_layer_heights") != print_options.end());

    const std::vector<std::string> &printer_options = Preset::printer_options();
    CHECK(std::find(printer_options.begin(), printer_options.end(), "extruder_preferred_layer_height") != printer_options.end());
    CHECK(std::find(printer_options.begin(), printer_options.end(), "multi_rate_layer_height_capable") != printer_options.end());
    CHECK(std::find(printer_options.begin(), printer_options.end(), "extruder_layer_height_ratio") != printer_options.end());
}

TEST_CASE("Four-tool policies preserve nozzle limits and feature permissions", "[independent-tool-layers][scheduler]")
{
    PrintConfig config = four_tool_config();
    config.extruder_layer_height_ratio.values = { 2, 1, 3, 4 };
    const ToolLayerPolicy t0 = make_tool_layer_policy(config, 0, 0.2);
    const ToolLayerPolicy t3 = make_tool_layer_policy(config, 3, 0.2);

    CHECK(t0.nozzle_diameter == Approx(0.4));
    CHECK(t0.target_height(0.1) == Approx(0.2));
    CHECK(t0.allows(MultiRateFeature::Walls));
    CHECK(t3.nozzle_diameter == Approx(0.8));
    CHECK(t3.target_height(0.1) == Approx(0.4));
    CHECK(t3.maximum_height == Approx(0.6));
    CHECK(t3.allows(MultiRateFeature::SparseInfill));
    CHECK_FALSE(t3.allows(MultiRateFeature::TopSurface));
    CHECK(t3.validate(0.1, SCALING_FACTOR).empty());
}

TEST_CASE("A profile ratio is not applied twice to an isolated tool grid", "[independent-tool-layers][scheduler]")
{
    PrintConfig config = four_tool_config();
    config.extruder_layer_height_ratio.values = { 2, 1, 3, 4 };
    const ToolLayerPolicy t3 = make_tool_layer_policy(config, 3, 0.2);

    CHECK(t3.target_height(0.1) == Approx(0.4));
    CHECK(t3.target_height(0.4) == Approx(0.4));
    CHECK(t3.validate(0.4, SCALING_FACTOR).empty());
}

TEST_CASE("Two-to-one wall and infill runs produce the required synchronization planes", "[independent-tool-layers][scheduler]")
{
    PrintConfig config = four_tool_config();
    config.extruder_layer_height_ratio.values[3] = 2;
    const ToolLayerPolicy infill_policy = make_tool_layer_policy(config, 3, 0.2);
    const std::vector<MultiRatePlane> planes {
        { 0.0, 0.2, false }, { 0.2, 0.4, false },
        { 0.4, 0.6, false }, { 0.6, 0.8, true }
    };
    std::string error;
    const std::vector<MultiRateRun> runs = build_multi_rate_runs(planes, infill_policy, 0.2, &error);
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].bottom_z == Approx(0.0));
    CHECK(runs[0].print_z == Approx(0.4));
    CHECK(runs[0].height() == Approx(0.4));
    CHECK(runs[1].bottom_z == Approx(0.4));
    CHECK(runs[1].print_z == Approx(0.8));

    MultiRateSchedule schedule;
    for (size_t layer = 0; layer < 4; ++ layer)
        schedule.add_operation({ 0, 0, 0, 0, MultiRateFeature::Walls,
                                 0.2 * layer, 0.2 * (layer + 1), 0.2, layer, layer });
    schedule.add_operation({ 0, 0, 0, 3, MultiRateFeature::SparseInfill, 0.0, 0.4, 0.4, 0, 1 });
    schedule.add_operation({ 0, 0, 0, 3, MultiRateFeature::SparseInfill, 0.4, 0.8, 0.4, 2, 3 });
    schedule.add_monotonic_z_dependencies();

    std::vector<ToolLayerPolicy> policies;
    for (unsigned int tool = 0; tool < 4; ++ tool)
        policies.push_back(make_tool_layer_policy(config, tool, 0.2));
    CHECK(schedule.validate(policies, SCALING_FACTOR).empty());

    const std::vector<size_t> order = schedule.ordered_operation_ids(&error);
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(order.size() == 6);
    const std::array<double, 6> expected_z { 0.2, 0.4, 0.4, 0.6, 0.8, 0.8 };
    const std::array<unsigned int, 6> expected_tools { 0, 0, 3, 0, 0, 3 };
    for (size_t idx = 0; idx < order.size(); ++ idx) {
        CHECK(schedule.operations()[order[idx]].print_z == Approx(expected_z[idx]));
        CHECK(schedule.operations()[order[idx]].tool_id == expected_tools[idx]);
    }
}

TEST_CASE("Three-to-one sparse infill creates 0.60 mm native slabs", "[independent-tool-layers][scheduler]")
{
    PrintConfig config = four_tool_config();
    config.max_layer_height.values[3] = 0.8;
    config.extruder_preferred_layer_height.values[3] = 0.6;
    config.extruder_layer_height_ratio.values[3] = 3;
    const ToolLayerPolicy policy = make_tool_layer_policy(config, 3, 0.2);
    const std::vector<MultiRatePlane> planes {
        { 0.0, 0.2, false }, { 0.2, 0.4, false }, { 0.4, 0.6, true },
        { 0.6, 0.8, false }, { 0.8, 1.0, false }, { 1.0, 1.2, true }
    };
    std::string error;
    const std::vector<MultiRateRun> runs = build_multi_rate_runs(planes, policy, 0.2, &error);
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].height() == Approx(0.6));
    CHECK(runs[1].height() == Approx(0.6));
}

TEST_CASE("Forced synchronization planes split coarse runs without exceeding tool limits", "[independent-tool-layers][scheduler]")
{
    PrintConfig config = four_tool_config();
    config.extruder_layer_height_ratio.values[3] = 4;
    const ToolLayerPolicy policy = make_tool_layer_policy(config, 3, 0.1);
    const std::vector<MultiRatePlane> planes {
        { 0.0, 0.1, false }, { 0.1, 0.2, true },
        { 0.2, 0.3, false }, { 0.3, 0.4, false }
    };
    std::string error;
    const std::vector<MultiRateRun> runs = build_multi_rate_runs(planes, policy, 0.1, &error);
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].height() == Approx(0.2));
    CHECK(runs[1].height() == Approx(0.2));
}

TEST_CASE("Non-integral tool grids merge deterministically at synchronization planes", "[independent-tool-layers][scheduler]")
{
    const std::vector<coordf_t> merged = merge_multi_rate_planes({
        { 0.10, 0.20, 0.30, 0.40, 0.50, 0.60 },
        { 0.12, 0.24, 0.36, 0.48, 0.60 }
    }, 0.01);
    CHECK(merged == std::vector<coordf_t> { 0.10, 0.12, 0.20, 0.24, 0.30, 0.36, 0.40, 0.48, 0.50, 0.60 });
}

TEST_CASE("Dependency graph rejects downward edges and reports cycles", "[independent-tool-layers][scheduler]")
{
    MultiRateSchedule schedule;
    const size_t low = schedule.add_operation({ 0, 0, 0, 0, MultiRateFeature::Walls, 0.0, 0.2, 0.2, 0, 0 });
    const size_t high = schedule.add_operation({ 0, 0, 0, 1, MultiRateFeature::Walls, 0.2, 0.4, 0.2, 1, 1 });
    std::string error;
    CHECK_FALSE(schedule.add_dependency(high, low, MultiRateDependencyReason::CollisionClearance, &error));
    CHECK(error.find("lower Z") != std::string::npos);
    REQUIRE(schedule.add_dependency(low, high, MultiRateDependencyReason::VerticalSupport));

    MultiRateSchedule cycle;
    const size_t a = cycle.add_operation({ 0, 0, 0, 0, MultiRateFeature::Walls, 0.0, 0.2, 0.2, 0, 0 });
    const size_t b = cycle.add_operation({ 0, 0, 0, 1, MultiRateFeature::SparseInfill, 0.0, 0.2, 0.2, 0, 0 });
    REQUIRE(cycle.add_dependency(a, b, MultiRateDependencyReason::MaterialInterface));
    REQUIRE(cycle.add_dependency(b, a, MultiRateDependencyReason::MaterialInterface));
    CHECK(cycle.ordered_operation_ids(&error).empty());
    CHECK(error.find("cycle") != std::string::npos);
}

TEST_CASE("Operation validation rejects extrusion above a tool maximum", "[independent-tool-layers][scheduler]")
{
    const PrintConfig config = four_tool_config();
    std::vector<ToolLayerPolicy> policies;
    for (unsigned int tool = 0; tool < 4; ++ tool)
        policies.push_back(make_tool_layer_policy(config, tool, 0.1));

    MultiRateSchedule schedule;
    schedule.add_operation({ 0, 0, 0, 1, MultiRateFeature::Walls, 0.0, 0.2, 0.2, 0, 0 });
    CHECK(schedule.validate(policies, SCALING_FACTOR).find("outside its layer-height limits") != std::string::npos);
}
