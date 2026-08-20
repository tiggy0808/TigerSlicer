///|/ TigerSlicer Phase 1: color/tool ownership resolution.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ToolOwnership_hpp_
#define slic3r_ToolOwnership_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/MultiRateLayerSchedule.hpp"

namespace Slic3r {

// Where a feature's owning tool came from. Recorded so Preview and the
// preflight report can explain an assignment instead of asserting it.
enum class OwnershipSource
{
    FeatureOverride,    // explicit per-feature tool choice
    PaintedRegion,      // painted multi-material region
    VolumeAssignment,   // model part / modifier volume
    ObjectAssignment,   // object-level tool
    DefaultExtruder,    // nothing more specific applies
};

// How features inside one region acquire their tool.
enum class SchedulingMode
{
    // All features in a region inherit the region's tool and preferred grid
    // unless explicitly overridden.
    PerToolGeometry,
    // Existing behaviour: features choose their own tool (T0 walls,
    // T3 sparse infill, ...).
    PerFeature,
};

// Everything that could determine the owner of one feature in one region.
// Each level is optional; absence means "nothing said at this level".
struct OwnershipInputs
{
    // Highest precedence: this exact feature was explicitly assigned.
    std::optional<int> feature_override;
    // Painted region / volume / object assignments, in descending precedence.
    std::optional<int> painted_region_tool;
    std::optional<int> volume_tool;
    std::optional<int> object_tool;
    // Per-feature preference used only in PerFeature mode.
    std::optional<int> feature_preference;

    int default_extruder { 0 };

    // Present so it is unmistakable that ownership never reads it. "Color"
    // in this system means tool ownership, not displayed RGB: re-tinting a
    // filament must never move geometry to a different tool.
    std::string display_color;
};

struct OwnershipDecision
{
    int             tool { 0 };
    OwnershipSource source { OwnershipSource::DefaultExtruder };

    // True when the feature took a tool other than its region's, so the
    // scheduler knows to use the override tool's grid for it.
    bool overrides_region { false };
};

// Resolve the owning tool for one feature.
//
// Precedence is fixed and does not vary by mode: explicit feature override,
// then painted region, then volume, then object, then the default extruder.
// The mode only decides whether a feature's own preference may participate.
OwnershipDecision resolve_tool_ownership(const OwnershipInputs &inputs,
                                         MultiRateFeature feature,
                                         SchedulingMode mode);

// The tool that owns a region as a whole, ignoring per-feature detail.
int resolve_region_tool(const OwnershipInputs &inputs);

// Per-tool grid selection: the layer policy a feature must be scheduled on,
// given its resolved owner. Returns nullptr when the tool has no policy, which
// callers must treat as a configuration error rather than silently defaulting.
const ToolLayerPolicy *policy_for_tool(const std::vector<ToolLayerPolicy> &policies, int tool);

const char *ownership_source_name(OwnershipSource source);

} // namespace Slic3r

#endif // slic3r_ToolOwnership_hpp_
