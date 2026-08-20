///|/ TigerSlicer Phase 22: advanced bridge planner.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BridgePlanner_hpp_
#define slic3r_BridgePlanner_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// What a bridge is for. A hidden structural bridge and a visible surface
// bridge want opposite things from a tool, which is why they are allowed to
// use different ones.
enum class BridgeRole : uint8_t
{
    Structural,
    Surface,
    Unknown,
};

const char *bridge_role_name(BridgeRole role);

enum class BridgeVerdict : uint8_t
{
    // A capable tool was found and every precondition was checked and met.
    Planned,
    // The bridge is only partly anchored; it can be attempted, but it wants
    // support underneath it first.
    NeedsSupport,
    // Something needed to judge this bridge is not recorded. This is not the
    // same as "checked and fine" and never collapses into it.
    Unproven,
    // The bridge cannot be attempted at all.
    Refused,
};

const char *bridge_verdict_name(BridgeVerdict verdict);

enum class AnchorState : uint8_t
{
    Anchored,
    PartiallyAnchored,
    Unanchored,
    Unknown,
};

const char *anchor_state_name(AnchorState state);

// One end of a bridge, where the extrusion has to land on something.
struct BridgeAnchor
{
    // Whether the material under this end is solid rather than sparse infill.
    bool solid { false };
    // How much landing width there is.
    double width_mm { 0. };
    // False when this anchor was never actually measured. An unmeasured anchor
    // is not an absent one, and it is not a good one either.
    bool measured { true };
};

struct BridgeGeometry
{
    size_t     id { 0 };
    BridgeRole role { BridgeRole::Unknown };

    double span_mm { 0. };
    double direction_deg { 0. };
    double layer_height_mm { 0.2 };

    // A bridge needs two ends to hold on to. Fewer is not a bridge.
    std::vector<BridgeAnchor> anchors;

    // Whether the user will see this bridge's underside.
    bool visible { false };

    // Bridges that must be printed before this one, by id. A bridge landing on
    // something that is not being printed cannot be synchronized.
    std::vector<size_t> depends_on;

    // A tool the caller has already committed to for this bridge. When set,
    // the planner validates it rather than choosing, and refuses if it is not
    // capable - it never quietly substitutes another tool.
    std::optional<int> required_tool;
};

struct BridgeTool
{
    int         tool { 0 };
    std::string material;
    double      nozzle_mm { 0.4 };

    // The longest span this tool has been shown to bridge. Absent means it was
    // never characterized, which is reported as unproven rather than assumed
    // either way.
    std::optional<double> max_bridge_span_mm;
    std::optional<double> max_flow_mm3_s;
    std::optional<double> bridge_flow_ratio;

    std::optional<double> min_layer_height_mm;
    std::optional<double> max_layer_height_mm;

    // A bridge without cooling sags. A tool with no part cooling fan cannot
    // bridge unless the policy says otherwise.
    bool has_part_cooling_fan { false };

    // Whether pressure can be brought up to full before the bridge starts.
    // Phase 17's rule: a bridge that begins under-pressured is a broken bridge.
    bool pressure_can_be_established { true };

    // What it costs to switch to this tool.
    double change_cost_s { 0. };
};

struct BridgePolicy
{
    double bridge_speed_mm_s { 60. };

    // A bridge with no cooling is refused by default.
    bool require_cooling { true };
    // Unmeasured anchors stay unproven by default rather than being trusted.
    bool allow_unmeasured_anchors { false };

    double anchor_min_width_mm { 3.0 };

    // Whether a hidden structural bridge and a visible surface bridge may use
    // different tools. When false, a bridge that depends on another one uses
    // that one's tool or is refused.
    bool allow_split_tools { true };

    // Above this many bridges the plan stops and says it stopped.
    size_t max_bridges { 500 };
};

struct BridgeDecision
{
    size_t        bridge { 0 };
    BridgeVerdict verdict { BridgeVerdict::Refused };
    AnchorState   anchors { AnchorState::Unknown };

    std::optional<int> tool;

    std::optional<double> flow_mm3_s;
    std::optional<double> estimated_time_s;
    // The toolchange this bridge pays for, when it is on a different tool from
    // the bridge it depends on.
    std::optional<double> change_cost_s;

    // True when every bridge this one lands on is itself being printed.
    bool synchronized { false };

    // Every tool that was considered and rejected, with why. A refusal that
    // does not name what is responsible is not a useful refusal.
    std::vector<std::string> rejected_tools;
    std::vector<std::string> reasons;
};

struct BridgePlan
{
    std::vector<BridgeDecision> decisions;
    std::vector<std::string>    notes;

    // Absent when any bridge could not be priced. A partial total reported as
    // a whole one would be a lie.
    std::optional<double> total_time_s;

    std::string error;
};

// Plan every bridge in the print.
//
// Each bridge is judged on span, direction, anchors, nozzle, layer height,
// material, cooling, flow, visibility and the cost of changing to the tool
// that would print it. A hidden structural bridge and a visible surface bridge
// may end up on different tools, which is usually what you want, and is
// allowed wherever the dependency order leaves room for the toolchange.
//
// Anchors, pressure, fans and synchronization are all validated before a
// bridge is planned. An unanchored bridge is refused, and a bridge that could
// not be validated is reported as unproven - never attempted on the assumption
// that it is probably fine.
BridgePlan plan_bridges(const std::vector<BridgeGeometry> &bridges,
                        const std::vector<BridgeTool>     &tools,
                        const BridgePolicy                &policy);

} // namespace Slic3r

#endif // slic3r_BridgePlanner_hpp_
