///|/ TigerSlicer Phase 12: preview filtering, diagnostics and estimates.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PreviewDiagnostics_hpp_
#define slic3r_PreviewDiagnostics_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/MultiRateLayerSchedule.hpp"
#include "libslic3r/SweptCollision.hpp"
#include "libslic3r/ToolchangeOptimizer.hpp"

namespace Slic3r {

// The thermal state a tool was in when an operation ran.
enum class ThermalState : uint8_t
{
    Active,
    Standby,
    Idle,
    Unknown,
};

const char *thermal_state_name(ThermalState state);

// What preparation, if any, immediately preceded an operation.
enum class PrepAction : uint8_t
{
    None,
    Purge,
    Brush,
    Prime,
    Cut,
};

const char *prep_action_name(PrepAction action);

// One row of what Preview shows. Everything here is *recorded* by the phases
// that made the decisions; this layer never re-derives any of it.
struct PreviewRecord
{
    size_t operation { 0 };
    size_t chronological_index { 0 };

    int         tool { 0 };
    std::string display_color;
    MultiRateFeature feature { MultiRateFeature::Walls };

    // Actual geometry, as printed - not the nominal profile values.
    double print_z { 0. };
    double layer_height_mm { 0. };
    double extrusion_width_mm { 0. };
    double flow_mm3_s { 0. };

    // Which per-tool grid this operation sits on, and whether its top plane is
    // a synchronization plane shared with another tool.
    double tool_grid_height_mm { 0. };
    bool   on_synchronization_plane { false };

    ThermalState thermal_state { ThermalState::Unknown };
    PrepAction   prep_action { PrepAction::None };

    // Present only on the operation a toolchange was made for.
    std::optional<ToolchangeNecessity> change_reason;
    std::string                        change_explanation;

    CollisionVerdict collision { CollisionVerdict::Unproven };

    double time_s { 0. };
    double waste_mm3 { 0. };
};

// Every field is optional; an unset field does not filter. Ranges are
// inclusive.
struct PreviewFilter
{
    std::vector<int>              tools;
    std::vector<std::string>      display_colors;
    std::vector<MultiRateFeature> features;
    std::vector<ThermalState>     thermal_states;
    std::vector<PrepAction>       prep_actions;
    std::vector<CollisionVerdict> collision_verdicts;
    std::vector<ToolchangeNecessity> change_reasons;

    std::optional<double> min_print_z, max_print_z;
    std::optional<double> min_layer_height_mm, max_layer_height_mm;
    std::optional<double> min_extrusion_width_mm, max_extrusion_width_mm;
    std::optional<double> min_flow_mm3_s, max_flow_mm3_s;
    std::optional<double> tool_grid_height_mm;
    std::optional<bool>   on_synchronization_plane;

    std::optional<size_t> from_chronological_index, to_chronological_index;

    bool empty() const;
};

// Indices into the supplied records, in chronological order.
std::vector<size_t> filter_preview(const std::vector<PreviewRecord> &records,
                                   const PreviewFilter              &filter);

// Totals over a selection.
struct PreviewTotals
{
    size_t operations { 0 };
    double time_s { 0. };
    double waste_mm3 { 0. };
    size_t toolchanges { 0 };
    size_t unproven_operations { 0 };
    size_t colliding_operations { 0 };

    // True when any operation in the selection was not proven collision-free.
    bool fully_proven() const { return unproven_operations == 0 && colliding_operations == 0; }
};

PreviewTotals summarize_preview(const std::vector<PreviewRecord> &records,
                                const std::vector<size_t>        &selection);

// --- single-tool comparison ------------------------------------------------

enum class ComparisonStatus : uint8_t
{
    Comparable,
    // The work cannot be done on one tool at all, so no honest number exists.
    SingleToolImpossible,
    // Something needed for the comparison was never recorded.
    Incomparable,
};

const char *comparison_status_name(ComparisonStatus status);

struct SingleToolComparison
{
    ComparisonStatus status { ComparisonStatus::Incomparable };

    PreviewTotals mixed;
    PreviewTotals single_tool;

    // Positive means the mixed-tool schedule costs more.
    double time_delta_s { 0. };
    double waste_delta_mm3 { 0. };

    std::vector<std::string> reasons;
};

// Compare the mixed-tool schedule against doing the same work on one tool.
//
// `single_tool` is the caller's estimate of the same job restricted to one
// tool. When any operation could not be produced by that tool - a material it
// cannot carry, a detail its nozzle cannot resolve - the comparison reports
// SingleToolImpossible rather than inventing a baseline.
SingleToolComparison compare_against_single_tool(
    const std::vector<PreviewRecord> &records,
    int                               single_tool,
    const std::optional<PreviewTotals> &single_tool_estimate,
    const std::vector<std::string>     &impossible_reasons);

// Collect the scheduler's own explanations, in chronological order, so
// Preview can show why the schedule looks the way it does without
// reconstructing the reasoning.
std::vector<std::string> explain_schedule(const std::vector<PreviewRecord> &records,
                                          const OptimizationResult         &optimization);

} // namespace Slic3r

#endif // slic3r_PreviewDiagnostics_hpp_
