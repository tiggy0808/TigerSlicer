///|/ TigerSlicer Phase 19: automatic detail decomposition.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_DetailDecomposition_hpp_
#define slic3r_DetailDecomposition_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// One candidate region of the model, described by what was measured about it.
struct RegionGeometry
{
    size_t id { 0 };

    // The smallest feature the region actually contains. Absent means nothing
    // measured it, which is not the same as the region being coarse.
    std::optional<double> min_feature_mm;
    std::optional<double> wall_thickness_mm;
    std::optional<double> required_accuracy_mm;

    // 0 (flat) to 1 (tightly curved).
    double curvature { 0. };
    bool   carries_text { false };
    bool   visible { false };

    double volume_mm3 { 0. };
    double print_z { 0. };

    // Which tools can physically reach this region. Empty means nobody worked
    // it out, which is reported rather than read as "all of them".
    std::vector<int> reachable_tools;
    bool             accessibility_known { false };

    // What the user already said. An explicit colour or tool assignment is the
    // user's decision and is not overridden unless remapping is enabled.
    std::optional<std::string> explicit_color;
    std::optional<int>         explicit_tool;

    // The tool currently doing this work, if any.
    std::optional<int> current_tool;
};

struct DecompositionTool
{
    int    tool { 0 };
    double nozzle_diameter_mm { 0.4 };
    // The finest feature this tool can resolve at all.
    double min_resolvable_mm { 0.4 };
    // The per-tool layer grid this tool prints on.
    double grid_height_mm { 0.2 };
    // How fast it lays material down.
    double throughput_mm3_s { 10. };
    // Absent means the tool's positional accuracy was never measured.
    std::optional<double> accuracy_mm;

    std::string color;
};

struct DecompositionOptions
{
    // When false, an explicit colour or tool assignment is preserved even if a
    // different tool would be better.
    bool allow_color_remap { false };

    // What one extra toolchange costs, and the least saving that justifies
    // proposing one.
    double toolchange_cost_s { 20. };
    double min_benefit_s { 5. };

    // Curvature above this is treated as detail worth a finer nozzle.
    double detail_curvature { 0.6 };
};

enum class RegionOutcome : uint8_t
{
    // A different tool is suggested, and the user may edit or reject it.
    Proposed,
    // The user's explicit assignment stands.
    KeptExplicit,
    // The current tool is already the right one.
    Unchanged,
    // Two or more tools are equally suitable; the tie is reported, not broken.
    Ambiguous,
    // Nothing on the machine can produce this region.
    NoCapableTool,
    // Something needed to decide was never measured.
    NeedsMeasurement,
};

const char *region_outcome_name(RegionOutcome outcome);

struct RegionProposal
{
    size_t             region { 0 };
    std::optional<int> current_tool;
    std::optional<int> proposed_tool;
    // Every tool that was equally suitable, when the outcome is Ambiguous.
    std::vector<int> tied_tools;

    RegionOutcome outcome { RegionOutcome::NeedsMeasurement };
    // Every proposal is editable; this layer proposes, it never decides.
    bool editable { true };

    std::vector<std::string> reasons;
};

// Where two adjacent regions end up owned by different tools. This is handed
// to the ownership and synchronization engine rather than resolved here: this
// phase decides who owns what, not how the boundary is printed.
struct InterfaceRequirement
{
    size_t region_a { 0 };
    size_t region_b { 0 };
    int    tool_a { 0 };
    int    tool_b { 0 };
    double print_z { 0. };
    // The layer height both grids must agree on at this boundary.
    std::optional<double> synchronization_height_mm;
    std::vector<std::string> reasons;
};

struct DecompositionResult
{
    std::vector<RegionProposal>       proposals;
    std::vector<InterfaceRequirement> interfaces;
    std::vector<std::string>          notes;
    std::string                       error;
};

// Propose tool-owned regions.
//
// Nothing here is applied. An explicit colour or tool assignment is preserved
// unless remapping was enabled, a tie between equally suitable tools is
// reported rather than broken by index, and a proposal that would not repay
// its toolchange is not made at all.
DecompositionResult decompose_details(const std::vector<RegionGeometry>            &regions,
                                      const std::vector<DecompositionTool>         &tools,
                                      const std::vector<std::pair<size_t, size_t>> &adjacency,
                                      const DecompositionOptions                   &options);

} // namespace Slic3r

#endif // slic3r_DetailDecomposition_hpp_
