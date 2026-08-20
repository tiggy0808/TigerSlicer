// Phase 1: tool ownership resolution. This sits ahead of the existing
// multi-rate scheduler and decides which tool owns each feature, so the tests
// pin the precedence order, the two scheduling modes, and the rule that a
// displayed filament colour never moves geometry to a different tool.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ToolOwnership.hpp"

using namespace Slic3r;

namespace {

OwnershipInputs inputs(int default_extruder = 0)
{
    OwnershipInputs value;
    value.default_extruder = default_extruder;
    return value;
}

ToolLayerPolicy policy(unsigned int tool, double preferred)
{
    ToolLayerPolicy item;
    item.tool_id          = tool;
    item.preferred_height = preferred;
    return item;
}

} // namespace

TEST_CASE("Ownership falls back to the default extruder", "[tool-ownership]")
{
    const OwnershipDecision decision = resolve_tool_ownership(
        inputs(2), MultiRateFeature::Walls, SchedulingMode::PerToolGeometry);

    CHECK(decision.tool == 2);
    CHECK(decision.source == OwnershipSource::DefaultExtruder);
    CHECK_FALSE(decision.overrides_region);
}

TEST_CASE("Precedence runs override, painted, volume, object, default", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.object_tool = 1;
    CHECK(resolve_tool_ownership(value, MultiRateFeature::Walls,
                                 SchedulingMode::PerToolGeometry).source ==
          OwnershipSource::ObjectAssignment);

    value.volume_tool = 2;
    CHECK(resolve_tool_ownership(value, MultiRateFeature::Walls,
                                 SchedulingMode::PerToolGeometry).tool == 2);

    value.painted_region_tool = 3;
    CHECK(resolve_tool_ownership(value, MultiRateFeature::Walls,
                                 SchedulingMode::PerToolGeometry).tool == 3);

    value.feature_override = 4;
    const OwnershipDecision top = resolve_tool_ownership(
        value, MultiRateFeature::Walls, SchedulingMode::PerToolGeometry);
    CHECK(top.tool == 4);
    CHECK(top.source == OwnershipSource::FeatureOverride);
}

TEST_CASE("Displayed colour never affects ownership", "[tool-ownership]")
{
    OwnershipInputs magenta = inputs(0);
    magenta.object_tool   = 1;
    magenta.display_color = "#FF00AA";

    OwnershipInputs blue = magenta;
    blue.display_color = "#0000FF";

    // Re-tinting a filament must not move geometry to a different tool.
    const OwnershipDecision a = resolve_tool_ownership(
        magenta, MultiRateFeature::Walls, SchedulingMode::PerToolGeometry);
    const OwnershipDecision b = resolve_tool_ownership(
        blue, MultiRateFeature::Walls, SchedulingMode::PerToolGeometry);

    CHECK(a.tool == b.tool);
    CHECK(a.source == b.source);
}

TEST_CASE("Geometry mode gives every feature the region's tool", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.painted_region_tool = 3;
    // A feature preference exists but must be ignored in geometry mode.
    value.feature_preference = 1;

    for (const MultiRateFeature feature : { MultiRateFeature::Walls,
                                            MultiRateFeature::SparseInfill,
                                            MultiRateFeature::TopSurface }) {
        const OwnershipDecision decision =
            resolve_tool_ownership(value, feature, SchedulingMode::PerToolGeometry);
        CHECK(decision.tool == 3);
        CHECK_FALSE(decision.overrides_region);
    }
}

TEST_CASE("Feature mode honours a feature's own preference", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.object_tool        = 0;   // region is T0
    value.feature_preference = 3;   // e.g. T3 sparse infill

    const OwnershipDecision decision = resolve_tool_ownership(
        value, MultiRateFeature::SparseInfill, SchedulingMode::PerFeature);

    CHECK(decision.tool == 3);
    // It departs from the region, so the scheduler must use T3's grid for it.
    CHECK(decision.overrides_region);
}

TEST_CASE("An explicit override outranks a feature preference", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.feature_preference = 3;
    value.feature_override   = 1;

    CHECK(resolve_tool_ownership(value, MultiRateFeature::SparseInfill,
                                 SchedulingMode::PerFeature).tool == 1);
}

TEST_CASE("Overrides are allowed in geometry mode and flagged", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.painted_region_tool = 0;
    value.feature_override    = 3;

    const OwnershipDecision decision = resolve_tool_ownership(
        value, MultiRateFeature::SparseInfill, SchedulingMode::PerToolGeometry);

    CHECK(decision.tool == 3);
    // The flag is what tells the scheduler to schedule this feature on T3's
    // grid rather than the region's.
    CHECK(decision.overrides_region);
}

TEST_CASE("An override matching the region is not treated as a departure", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.painted_region_tool = 2;
    value.feature_override    = 2;

    const OwnershipDecision decision = resolve_tool_ownership(
        value, MultiRateFeature::Walls, SchedulingMode::PerToolGeometry);

    CHECK(decision.tool == 2);
    CHECK_FALSE(decision.overrides_region);
}

TEST_CASE("Region ownership ignores per-feature detail", "[tool-ownership]")
{
    OwnershipInputs value = inputs(0);
    value.volume_tool      = 2;
    value.feature_override = 3;

    // The region still belongs to T2 even though one feature departs from it.
    CHECK(resolve_region_tool(value) == 2);
}

TEST_CASE("A tool without a layer policy is reported, not defaulted", "[tool-ownership]")
{
    const std::vector<ToolLayerPolicy> policies { policy(0, 0.2), policy(1, 0.1) };

    REQUIRE(policy_for_tool(policies, 1) != nullptr);
    CHECK(policy_for_tool(policies, 1)->preferred_height == 0.1);
    // Silently substituting another tool's grid would change layer heights
    // behind the user's back.
    CHECK(policy_for_tool(policies, 7) == nullptr);
    CHECK(policy_for_tool(policies, -1) == nullptr);
}
