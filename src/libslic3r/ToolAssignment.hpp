///|/ TigerSlicer Phase 5: automatic nozzle/tool assignment proposals.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ToolAssignment_hpp_
#define slic3r_ToolAssignment_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/MultiRateLayerSchedule.hpp"

namespace Slic3r {

// What a tool can actually do. Nothing here is inferred from the tool's index:
// no rule in this file says "T1 is the fine tool". Fine work lands on a small
// nozzle because it is small, whichever tool carries it.
struct ToolCapability
{
    int    tool { 0 };
    double nozzle_diameter_mm { 0. };
    double min_layer_height_mm { 0. };
    double max_layer_height_mm { 0. };
    double max_flow_mm3_s { 0. };

    // Loaded material. Empty means "not reported", which is different from a
    // material that disagrees, and is never treated as compatible.
    std::string material;
};

// Whether a surface is seen on the finished part.
enum class Visibility : uint8_t
{
    Visible,
    Hidden,
    Unknown,
};

const char *visibility_name(Visibility visibility);

// Measured properties of one piece of work. Every measurement is optional
// because a proposal must be able to say "this was never measured" instead of
// substituting a default and presenting the result as a decision.
struct FeatureGeometry
{
    size_t           id { 0 };
    MultiRateFeature feature { MultiRateFeature::Walls };

    std::optional<double> minimum_feature_width_mm;
    std::optional<double> wall_thickness_mm;
    std::optional<double> required_accuracy_mm;
    std::optional<double> layer_height_mm;
    std::optional<double> curvature_radius_mm;

    Visibility visibility { Visibility::Unknown };
    bool       carries_text_or_detail { false };

    // Empty means the work imposes no material requirement.
    std::string required_material;
    double      volume_mm3 { 0. };
};

struct FeatureAssignmentState
{
    FeatureGeometry geometry;
    int             current_tool { 0 };
    // Set by the user through an override, painted region, or volume/object
    // assignment. Phase 1's OwnershipSource is what decides this.
    bool manually_assigned { false };
};

struct AssignmentOptions
{
    // Manual ownership is authoritative. Only an explicit opt-in lets a
    // proposal contradict it, and even then the proposal is flagged for
    // confirmation rather than applied.
    bool allow_remapping_manual { false };
};

enum class AssignmentOutcome : uint8_t
{
    Proposed,           // a single tool is clearly best
    KeptManual,         // the user chose this tool and remapping is not enabled
    Unchanged,          // the best tool is the one already assigned
    Ambiguous,          // two or more tools are equally suitable
    NoCompatibleTool,   // nothing on the machine can do this work
    NeedsMeasurement,   // a property the decision depends on was never measured
};

const char *assignment_outcome_name(AssignmentOutcome outcome);

struct ToolProposal
{
    size_t feature { 0 };
    int    current_tool { 0 };
    // Only meaningful when the outcome is Proposed or Unchanged. Ambiguous,
    // NoCompatibleTool and NeedsMeasurement leave it at current_tool so a
    // caller that ignores the outcome cannot silently move geometry.
    int proposed_tool { 0 };

    AssignmentOutcome outcome { AssignmentOutcome::Unchanged };
    // Why, in the order the reasons were applied. Always populated.
    std::vector<std::string> reasons;
    // Tools that were equally good, when the outcome is Ambiguous.
    std::vector<int> tied_tools;
    // True when accepting this proposal would overwrite a manual assignment.
    bool requires_confirmation { false };

    bool changes_assignment() const
    {
        return outcome == AssignmentOutcome::Proposed && proposed_tool != current_tool;
    }
};

// Propose an owning tool for each piece of work.
//
// This proposes; it does not assign. Nothing here mutates ownership, and a
// proposal that is Ambiguous, NoCompatibleTool or NeedsMeasurement carries no
// tool change at all.
std::vector<ToolProposal> propose_tool_assignments(
    const std::vector<FeatureAssignmentState> &features,
    const std::vector<ToolCapability>         &tools,
    const AssignmentOptions                   &options);

// Resolve accepted proposals into final ownership.
//
// `accepted` is indexed alongside `proposals`. A proposal the user did not
// accept, or one that never carried a decision, leaves the feature on its
// current tool. Returns false and writes `error` if a proposal that cannot be
// applied was nevertheless accepted, rather than applying it anyway.
bool apply_tool_proposals(const std::vector<ToolProposal> &proposals,
                          const std::vector<bool>         &accepted,
                          std::vector<int>                *resolved_tools,
                          std::string                     *error);

} // namespace Slic3r

#endif // slic3r_ToolAssignment_hpp_
