///|/ TigerSlicer Phase 1: color/tool ownership resolution.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ToolOwnership.hpp"

#include <algorithm>

namespace Slic3r {

const char *ownership_source_name(OwnershipSource source)
{
    switch (source) {
    case OwnershipSource::FeatureOverride:  return "feature override";
    case OwnershipSource::PaintedRegion:    return "painted region";
    case OwnershipSource::VolumeAssignment: return "volume assignment";
    case OwnershipSource::ObjectAssignment: return "object assignment";
    case OwnershipSource::DefaultExtruder:  return "default extruder";
    }
    return "default extruder";
}

int resolve_region_tool(const OwnershipInputs &inputs)
{
    // Region ownership deliberately ignores feature-level detail: it is the
    // tool the geometry belongs to, which per-feature overrides then depart
    // from individually.
    if (inputs.painted_region_tool.has_value())
        return *inputs.painted_region_tool;
    if (inputs.volume_tool.has_value())
        return *inputs.volume_tool;
    if (inputs.object_tool.has_value())
        return *inputs.object_tool;
    return inputs.default_extruder;
}

OwnershipDecision resolve_tool_ownership(const OwnershipInputs &inputs,
                                         MultiRateFeature feature,
                                         SchedulingMode mode)
{
    (void)feature;   // features are distinguished by the caller's inputs

    OwnershipDecision decision;
    const int region_tool = resolve_region_tool(inputs);

    // 1. An explicit per-feature assignment always wins, in either mode.
    if (inputs.feature_override.has_value()) {
        decision.tool             = *inputs.feature_override;
        decision.source           = OwnershipSource::FeatureOverride;
        decision.overrides_region = decision.tool != region_tool;
        return decision;
    }

    // 2. In per-feature mode a feature's own preference may claim it, but it
    //    still ranks below an explicit override.
    if (mode == SchedulingMode::PerFeature && inputs.feature_preference.has_value()) {
        decision.tool             = *inputs.feature_preference;
        decision.source           = OwnershipSource::FeatureOverride;
        decision.overrides_region = decision.tool != region_tool;
        return decision;
    }

    // 3. Otherwise the region's own precedence chain decides.
    decision.tool = region_tool;
    if (inputs.painted_region_tool.has_value())
        decision.source = OwnershipSource::PaintedRegion;
    else if (inputs.volume_tool.has_value())
        decision.source = OwnershipSource::VolumeAssignment;
    else if (inputs.object_tool.has_value())
        decision.source = OwnershipSource::ObjectAssignment;
    else
        decision.source = OwnershipSource::DefaultExtruder;

    decision.overrides_region = false;
    return decision;
}

const ToolLayerPolicy *policy_for_tool(const std::vector<ToolLayerPolicy> &policies, int tool)
{
    if (tool < 0)
        return nullptr;
    const auto it = std::find_if(policies.begin(), policies.end(),
        [tool](const ToolLayerPolicy &policy) { return int(policy.tool_id) == tool; });
    return it == policies.end() ? nullptr : &*it;
}

} // namespace Slic3r
