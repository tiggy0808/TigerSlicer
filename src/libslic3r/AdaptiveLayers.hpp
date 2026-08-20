///|/ TigerSlicer Phase 23: curvature-aware adaptive layers per tool.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_AdaptiveLayers_hpp_
#define slic3r_AdaptiveLayers_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class LayerVerdict : uint8_t
{
    Planned,
    // Printable, but coarser than the surface asked for, and it says why.
    Coarsened,
    // Something needed to judge the surface is not recorded, so the finest
    // layer was used and the resulting error is not claimed.
    Unproven,
    Refused,
};

const char *layer_verdict_name(LayerVerdict verdict);

// A z at which a layer boundary has to land.
enum class LayerConstraintKind : uint8_t
{
    // A flat top; a layer must end here or the surface is wrong.
    TopSurface,
    // A colour or material changes here.
    ColorBoundary,
    // Every tool has to meet here.
    Synchronization,
    // A tool must not print above here.
    Collision,
};

const char *layer_constraint_kind_name(LayerConstraintKind kind);

struct LayerConstraint
{
    double              z_mm { 0. };
    LayerConstraintKind kind { LayerConstraintKind::TopSurface };

    // Absent means the constraint applies to every tool.
    std::optional<int> tool;

    std::string description;
};

// A z range of the model over which the surface has one character.
struct SurfaceBand
{
    double z_bottom_mm { 0. };
    double z_top_mm { 0. };

    // The surface angle from horizontal: 0 is a shallow, near-flat slope that
    // stair-steps badly, 90 is a vertical wall that does not step at all.
    // Absent means it was never measured, which is reported rather than
    // guessed at in either direction.
    std::optional<double> slope_deg;

    // 0 (flat) to 1 (tightly curved). Curvature makes the same layer height
    // read worse, so it tightens the height alongside the slope.
    double curvature { 0. };

    // A surface nobody will see does not need to be refined for looks.
    bool visible { true };

    // Text, engraving, or anything else that has to survive.
    bool carries_detail { false };
};

struct ToolLayerGrid
{
    int    tool { 0 };
    double nozzle_mm { 0.4 };
    double min_layer_height_mm { 0.06 };
    double max_layer_height_mm { 0.30 };

    // What one change to this tool costs. Absent means it was never measured,
    // and the synchronization cost is then withheld rather than guessed.
    std::optional<double> change_cost_s;

    bool available { true };
};

struct AdaptiveLayerOptions
{
    // The most stair-stepping the user is willing to accept on a visible
    // surface, as a cusp height.
    double surface_error_tolerance_mm { 0.05 };

    // Refining a layer that would improve the surface by less than this is not
    // worth the extra layer, and the schedule says so instead of doing it.
    double minimum_benefit_mm { 0.02 };

    // Never emit a layer thinner than this, whatever a tool claims it can do.
    double micro_layer_floor_mm { 0.04 };

    // Schedule complexity. Zero means unlimited. Exceeding the change limit
    // replans the tool at one uniform height and reports the coarsening.
    size_t max_height_changes { 0 };
    size_t max_layers_per_tool { 5000 };

    // A detail-carrying band is capped at this fraction of the nozzle.
    double detail_height_fraction { 0.5 };

    double boundary_tolerance_mm { 1e-6 };
};

struct PlannedLayer
{
    double z_bottom_mm { 0. };
    double z_top_mm { 0. };
    double height_mm { 0. };

    // Absent when the band's slope was never measured, so no error is claimed.
    std::optional<double> estimated_surface_error_mm;

    // True when this layer ends on a constraint rather than on a height.
    bool at_constraint { false };

    std::string reason;
};

struct ToolSchedule
{
    int          tool { 0 };
    LayerVerdict verdict { LayerVerdict::Refused };

    std::vector<PlannedLayer> layers;
    size_t                    height_changes { 0 };

    // True when the schedule stopped early rather than covering the model.
    bool capped { false };

    std::vector<std::string> notes;
};

struct AdaptiveLayerPlan
{
    std::vector<ToolSchedule> schedules;
    std::vector<std::string>  notes;

    // Every z where all tools have to meet.
    std::vector<double> synchronization_points;
    // Absent when any tool's change cost is unrecorded.
    std::optional<double> synchronization_cost_s;

    std::string error;
};

// The stair-stepping a layer of this height leaves on this band, or nothing
// when the band's slope was never measured.
std::optional<double> estimate_surface_error(const SurfaceBand &band, double height_mm);

// Plan a layer schedule for every tool.
//
// Within each tool's own grid, shallow slopes, curves and detail get finer
// layers and vertical or hidden regions get thicker ones, bounded by the
// tool's own minimum and maximum and by a micro-layer floor that no tool may
// go under.
//
// Layers land exactly on top surfaces, colour boundaries and synchronization
// points; a boundary that cannot be reached without a sliver is merged into
// the layer below where that fits and refused where it does not - the boundary
// itself is never moved. A band whose slope was never measured gets the finest
// layer and is reported as unproven rather than being assumed flat or
// vertical.
//
// Surface-error tolerance, minimum benefit and schedule complexity are the
// three controls the caller is expected to turn.
AdaptiveLayerPlan plan_adaptive_layers(const std::vector<SurfaceBand>     &bands,
                                       const std::vector<ToolLayerGrid>   &grids,
                                       const std::vector<LayerConstraint> &constraints,
                                       const AdaptiveLayerOptions         &options);

} // namespace Slic3r

#endif // slic3r_AdaptiveLayers_hpp_
