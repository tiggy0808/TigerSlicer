///|/ TigerSlicer Phase 23: curvature-aware adaptive layers per tool.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "AdaptiveLayers.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *layer_verdict_name(LayerVerdict verdict)
{
    switch (verdict) {
    case LayerVerdict::Planned:   return "planned";
    case LayerVerdict::Coarsened: return "coarsened";
    case LayerVerdict::Unproven:  return "unproven";
    case LayerVerdict::Refused:   return "refused";
    }
    return "refused";
}

const char *layer_constraint_kind_name(LayerConstraintKind kind)
{
    switch (kind) {
    case LayerConstraintKind::TopSurface:      return "top surface";
    case LayerConstraintKind::ColorBoundary:   return "colour boundary";
    case LayerConstraintKind::Synchronization: return "synchronization point";
    case LayerConstraintKind::Collision:       return "collision limit";
    }
    return "top surface";
}

std::optional<double> estimate_surface_error(const SurfaceBand &band, double height_mm)
{
    if (!band.slope_deg.has_value())
        return std::nullopt;
    // The cusp left by one layer on a slope, made worse by curvature. A
    // vertical wall does not step at all; a shallow slope steps the full
    // layer height.
    const double radians = *band.slope_deg * M_PI / 180.;
    return height_mm * std::abs(std::cos(radians)) * (1. + band.curvature);
}

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

std::string tool_name(int tool) { return "T" + std::to_string(tool); }

int severity(LayerVerdict verdict)
{
    switch (verdict) {
    case LayerVerdict::Planned:   return 0;
    case LayerVerdict::Coarsened: return 1;
    case LayerVerdict::Unproven:  return 2;
    case LayerVerdict::Refused:   return 3;
    }
    return 3;
}

LayerVerdict worse_of(LayerVerdict left, LayerVerdict right)
{
    return severity(left) >= severity(right) ? left : right;
}

struct BandHeight
{
    double       height { 0. };
    LayerVerdict verdict { LayerVerdict::Planned };
    std::string  note;
};

// What one band asks of one tool, before any boundary is taken into account.
BandHeight height_for_band(const SurfaceBand &band, const ToolLayerGrid &grid,
                           const AdaptiveLayerOptions &options)
{
    BandHeight result;
    const double floor_height = std::max(grid.min_layer_height_mm,
                                         options.micro_layer_floor_mm);
    const double ceiling      = std::max(floor_height, grid.max_layer_height_mm);

    if (!band.slope_deg.has_value()) {
        // Never measured. The finest layer is the one that cannot make the
        // surface worse, and the error is not claimed either way.
        result.height  = floor_height;
        result.verdict = LayerVerdict::Unproven;
        result.note    = "the slope between " + number(band.z_bottom_mm, 2) + " and " +
                      number(band.z_top_mm, 2) +
                      " mm was never measured, so the finest layer this tool allows is used "
                      "and the surface error there is not known";
        return result;
    }

    if (!band.visible) {
        result.height = ceiling;
        result.note   = "the band between " + number(band.z_bottom_mm, 2) + " and " +
                      number(band.z_top_mm, 2) +
                      " mm is not visible, so it is printed at the thickest layer this tool "
                      "allows";
        return result;
    }

    const double radians = *band.slope_deg * M_PI / 180.;
    const double stepping = std::abs(std::cos(radians)) * (1. + band.curvature);

    double height = ceiling;
    if (stepping > EPSILON)
        height = options.surface_error_tolerance_mm / stepping;

    height = std::min(std::max(height, floor_height), ceiling);

    // Refining for a gain nobody can see is not worth the extra layers. This
    // is asked of the height the surface error wanted, before any detail cap:
    // detail has to survive whether or not it changes the cusp height.
    if (height < ceiling - EPSILON) {
        const std::optional<double> coarse = estimate_surface_error(band, ceiling);
        const std::optional<double> fine   = estimate_surface_error(band, height);
        if (coarse.has_value() && fine.has_value() &&
            *coarse - *fine < options.minimum_benefit_mm - EPSILON) {
            result.note = "refining the band between " + number(band.z_bottom_mm, 2) +
                          " and " + number(band.z_top_mm, 2) + " mm would improve the "
                          "surface by " + number(*coarse - *fine, 3) +
                          " mm, less than the " + number(options.minimum_benefit_mm, 3) +
                          " mm that was asked to be worth an extra layer, so it was left "
                          "coarse";
            height = ceiling;
        }
    }

    if (band.carries_detail) {
        const double capped = std::min(height, grid.nozzle_mm * options.detail_height_fraction);
        height = std::min(std::max(capped, floor_height), ceiling);
    }

    result.height = height;
    return result;
}

} // namespace

AdaptiveLayerPlan plan_adaptive_layers(const std::vector<SurfaceBand>     &bands,
                                       const std::vector<ToolLayerGrid>   &grids,
                                       const std::vector<LayerConstraint> &constraints,
                                       const AdaptiveLayerOptions         &options)
{
    AdaptiveLayerPlan plan;

    if (bands.empty()) {
        plan.error = "the model was not described as any surface bands, so there is nothing "
                     "to lay out";
        return plan;
    }
    if (grids.empty()) {
        plan.error = "no tool grids were described, so there is nothing to plan a schedule "
                     "for";
        return plan;
    }

    const double tol = options.boundary_tolerance_mm;

    // The bands have to cover the model without gaps, or the schedule would be
    // laying out z it knows nothing about.
    for (size_t index = 0; index + 1 < bands.size(); ++index) {
        if (bands[index].z_top_mm > bands[index + 1].z_bottom_mm + tol ||
            bands[index].z_top_mm < bands[index + 1].z_bottom_mm - tol) {
            plan.error = "the surface bands do not meet at " +
                         number(bands[index].z_top_mm, 3) +
                         " mm, so part of the model is undescribed and no schedule is "
                         "produced for it";
            return plan;
        }
    }

    const double bottom = bands.front().z_bottom_mm;
    const double top    = bands.back().z_top_mm;
    if (top <= bottom + tol) {
        plan.error = "the described model has no height";
        return plan;
    }

    for (const LayerConstraint &constraint : constraints)
        if (constraint.kind == LayerConstraintKind::Synchronization)
            plan.synchronization_points.push_back(constraint.z_mm);
    std::sort(plan.synchronization_points.begin(), plan.synchronization_points.end());
    // Drop duplicate and out-of-range synchronization points so they cannot
    // inflate the reported cost (mirrors the per-tool `stops` handling below):
    // two constraints at the same z are one physical meeting, and a z outside
    // the printable range is one no layer can land on.
    {
        auto &pts = plan.synchronization_points;
        pts.erase(std::remove_if(pts.begin(), pts.end(),
                      [&](double z) { return z <= bottom + tol || z >= top - tol; }),
                  pts.end());
        pts.erase(std::unique(pts.begin(), pts.end(),
                      [&](double a, double b) { return std::abs(a - b) <= tol; }),
                  pts.end());
    }

    // The cost of making every tool meet at each synchronization point. One
    // unrecorded change cost withholds the whole figure rather than reporting
    // a total that quietly leaves a tool out.
    // One already-mounted tool needs no change, so exactly one tool's cost is
    // excluded. Choose it deterministically (lowest tool id) rather than by
    // vector position, so the total does not depend on the order of `grids` and
    // does not shift when grids[0] happens to be unavailable.
    bool   all_costs_known = true;
    double total_cost      = 0.;
    bool   have_excluded   = false;
    int    excluded_tool   = 0;
    double excluded_cost   = 0.;
    for (const ToolLayerGrid &grid : grids) {
        if (!grid.available)
            continue;
        if (!grid.change_cost_s.has_value()) {
            all_costs_known = false;
            continue;
        }
        total_cost += *grid.change_cost_s;
        if (!have_excluded || grid.tool < excluded_tool) {
            have_excluded = true;
            excluded_tool = grid.tool;
            excluded_cost = *grid.change_cost_s;
        }
    }
    const double per_meeting = have_excluded ? total_cost - excluded_cost : 0.;
    if (plan.synchronization_points.empty())
        plan.synchronization_cost_s = 0.;
    else if (all_costs_known)
        plan.synchronization_cost_s =
            per_meeting * double(plan.synchronization_points.size());
    else
        plan.notes.push_back("at least one tool has no recorded change cost, so the cost of "
                             "synchronizing the tools is not reported");

    for (const ToolLayerGrid &grid : grids) {
        ToolSchedule schedule;
        schedule.tool    = grid.tool;
        schedule.verdict = LayerVerdict::Planned;

        if (!grid.available) {
            schedule.verdict = LayerVerdict::Refused;
            schedule.notes.push_back(tool_name(grid.tool) +
                                     " is not available, so no schedule was laid out for it");
            plan.schedules.push_back(schedule);
            continue;
        }
        if (grid.max_layer_height_mm < grid.min_layer_height_mm - tol) {
            schedule.verdict = LayerVerdict::Refused;
            schedule.notes.push_back(tool_name(grid.tool) + " has a maximum layer height " +
                                     number(grid.max_layer_height_mm, 3) +
                                     " mm below its minimum " +
                                     number(grid.min_layer_height_mm, 3) +
                                     " mm, which is not a grid it can print on");
            plan.schedules.push_back(schedule);
            continue;
        }
        if (grid.min_layer_height_mm < options.micro_layer_floor_mm - tol)
            schedule.notes.push_back(tool_name(grid.tool) + " allows layers down to " +
                                     number(grid.min_layer_height_mm, 3) +
                                     " mm, but the micro-layer floor of " +
                                     number(options.micro_layer_floor_mm, 3) +
                                     " mm is used instead");

        // Boundaries this tool has to land on, and the height it must stop at.
        std::vector<double> stops;
        double              ceiling_z = top;
        for (const LayerConstraint &constraint : constraints) {
            if (constraint.tool.has_value() && *constraint.tool != grid.tool)
                continue;
            if (constraint.kind == LayerConstraintKind::Collision) {
                if (constraint.z_mm < ceiling_z) {
                    ceiling_z = constraint.z_mm;
                    schedule.notes.push_back(
                        tool_name(grid.tool) + " must not print above " +
                        number(constraint.z_mm, 2) + " mm" +
                        (constraint.description.empty() ? "" : ": " + constraint.description) +
                        ", so its schedule stops there rather than covering the model");
                }
                continue;
            }
            if (constraint.z_mm > bottom + tol && constraint.z_mm < top - tol)
                stops.push_back(constraint.z_mm);
        }
        std::sort(stops.begin(), stops.end());
        stops.erase(std::unique(stops.begin(), stops.end(),
                                [&](double a, double b) { return std::abs(a - b) <= tol; }),
                    stops.end());

        const double floor_height = std::max(grid.min_layer_height_mm,
                                             options.micro_layer_floor_mm);

        // One pass over the model. A forced height replans the whole tool at a
        // single height when the schedule turned out too complex to be worth
        // its own detail.
        auto lay_out = [&](std::optional<double> forced) {
            schedule.layers.clear();
            // The band each pushed layer's bulk lies in, kept parallel to
            // schedule.layers so a folded sliver can recompute the previous
            // layer's surface error against ITS band, not the current one.
            std::vector<size_t> layer_band;
            std::vector<bool> band_noted(bands.size(), false);
            LayerVerdict      verdict = LayerVerdict::Planned;

            double z          = bottom;
            size_t band_index = 0;

            while (z < ceiling_z - tol) {
                while (band_index + 1 < bands.size() &&
                       z >= bands[band_index].z_top_mm - tol)
                    ++band_index;
                const SurfaceBand &band = bands[band_index];

                double height = 0.;
                if (forced.has_value()) {
                    height = *forced;
                } else {
                    const BandHeight ask = height_for_band(band, grid, options);
                    height               = ask.height;
                    verdict              = worse_of(verdict, ask.verdict);
                    if (!ask.note.empty() && !band_noted[band_index]) {
                        schedule.notes.push_back(ask.note);
                        band_noted[band_index] = true;
                    }
                }

                double stop = std::min(z + height, ceiling_z);
                stop        = std::min(stop, band.z_top_mm);
                bool at_constraint = std::abs(stop - band.z_top_mm) <= tol;
                for (double boundary : stops) {
                    if (boundary > z + tol && boundary < stop - tol) {
                        stop          = boundary;
                        at_constraint = true;
                        break;
                    }
                    if (std::abs(boundary - stop) <= tol)
                        at_constraint = true;
                }

                double actual = stop - z;
                if (actual < floor_height - tol) {
                    // A sliver. The boundary is never moved to get rid of it;
                    // it is folded into the layer below where that fits, and
                    // refused where it does not.
                    if (!schedule.layers.empty() &&
                        schedule.layers.back().height_mm + actual <=
                            grid.max_layer_height_mm + tol) {
                        PlannedLayer &previous = schedule.layers.back();

                        // If this layer was already closed on a boundary,
                        // extending it carries it past that one. Two boundaries
                        // closer together than the micro-layer floor cannot both
                        // be layer ends, so one is given up - and which one is
                        // said out loud rather than left in a reason string that
                        // claims both landed.
                        if (previous.at_constraint)
                            schedule.notes.push_back(
                                "the boundary at " + number(previous.z_top_mm, 2) +
                                " mm is no longer a layer end: the boundary at " +
                                number(stop, 2) + " mm is only " +
                                number(stop - previous.z_top_mm, 3) +
                                " mm above it, under the " + number(floor_height, 3) +
                                " mm floor, and two boundaries that close cannot both be "
                                "landed on; the lower one is the one given up");

                        previous.z_top_mm      = stop;
                        previous.height_mm     = stop - previous.z_bottom_mm;
                        previous.at_constraint = at_constraint;
                        // Recompute the previous layer's error against the band
                        // its bulk lies in, not the current loop band. If the
                        // extended layer now straddles measured and unmeasured
                        // bands, refuse to claim a value (provenance would be
                        // wrong either way).
                        const SurfaceBand &prev_band = bands[layer_band.back()];
                        if (prev_band.slope_deg.has_value() != band.slope_deg.has_value())
                            previous.estimated_surface_error_mm = std::nullopt;
                        else
                            previous.estimated_surface_error_mm =
                                estimate_surface_error(prev_band, previous.height_mm);
                        // Drop any earlier extension clause. Appending a second
                        // one would leave the layer claiming to end on a
                        // boundary it has since been carried past.
                        const size_t earlier = previous.reason.find("; extended by ");
                        if (earlier != std::string::npos)
                            previous.reason.erase(earlier);

                        previous.reason +=
                            "; extended by " + number(actual, 3) +
                            " mm, a sliver below the micro-layer floor, so that this layer "
                            "ends exactly on the boundary at " + number(stop, 2) + " mm";
                        z = stop;
                        continue;
                    }
                    verdict = LayerVerdict::Refused;
                    schedule.notes.push_back(
                        "reaching the boundary at " + number(stop, 2) + " mm would need a " +
                        number(actual, 3) + " mm layer, under the " +
                        number(floor_height, 3) +
                        " mm floor, and it cannot be folded into the layer below without "
                        "exceeding " + tool_name(grid.tool) + "'s maximum height; the "
                        "boundary is not moved to make it fit");
                    break;
                }

                PlannedLayer layer;
                layer.z_bottom_mm                = z;
                layer.z_top_mm                   = stop;
                layer.height_mm                  = actual;
                layer.at_constraint              = at_constraint;
                layer.estimated_surface_error_mm = estimate_surface_error(band, actual);
                layer.reason =
                    number(actual, 3) + " mm " +
                    (band.slope_deg.has_value()
                         ? (band.visible ? "for a " + number(*band.slope_deg, 0) +
                                               " degree visible surface"
                                         : "through a hidden region")
                         : "because the slope here was never measured");
                schedule.layers.push_back(layer);
                layer_band.push_back(band_index);

                z = stop;

                if (schedule.layers.size() >= options.max_layers_per_tool) {
                    schedule.capped = true;
                    schedule.notes.push_back(
                        tool_name(grid.tool) + "'s schedule stopped after " +
                        std::to_string(options.max_layers_per_tool) + " layers at " +
                        number(z, 2) + " mm; the rest of the model was not laid out");
                    break;
                }
            }

            schedule.height_changes = 0;
            for (size_t index = 1; index < schedule.layers.size(); ++index)
                if (std::abs(schedule.layers[index].height_mm -
                             schedule.layers[index - 1].height_mm) > tol)
                    ++schedule.height_changes;

            return verdict;
        };

        // Notes recorded before any layout describe the tool itself and are
        // kept if the schedule is replanned.
        const size_t preamble = schedule.notes.size();

        LayerVerdict verdict = lay_out(std::nullopt);

        // Schedule complexity. A schedule that changes height more often than
        // the caller wants is replanned at one height, and says so.
        if (options.max_height_changes > 0 && verdict != LayerVerdict::Refused &&
            schedule.height_changes > options.max_height_changes) {
            double uniform = grid.max_layer_height_mm;
            for (const SurfaceBand &band : bands)
                uniform = std::min(uniform, height_for_band(band, grid, options).height);
            uniform = std::max(uniform, floor_height);

            const size_t was = schedule.height_changes;
            // Preserve the pre-replan verdict as a lower bound: severity may only
            // escalate. A uniform replan is at least Coarsened, but if the
            // adaptive pass was Unproven (a band had unmeasured slope) that must
            // not be downgraded to Coarsened, which would let an unmeasured band
            // pass as merely "coarsened".
            const LayerVerdict before = verdict;
            const bool had_unproven = before == LayerVerdict::Unproven;
            schedule.notes.erase(schedule.notes.begin() + long(preamble),
                                 schedule.notes.end());
            verdict = worse_of(lay_out(uniform), worse_of(before, LayerVerdict::Coarsened));
            schedule.notes.push_back(
                tool_name(grid.tool) + "'s adaptive schedule changed height " +
                std::to_string(was) + " times, more than the " +
                std::to_string(options.max_height_changes) +
                " asked for, so the whole tool was replanned at a uniform " +
                number(uniform, 3) + " mm; the finer detail that would have bought is gone");
            if (had_unproven)
                // The forced replan does not emit per-band notes, so re-state the
                // unmeasured-slope signal that the erase above removed.
                schedule.notes.push_back(
                    "at least one band's slope was never measured, so the surface "
                    "error of the replanned schedule is still not known");
        }

        schedule.verdict = verdict;
        plan.schedules.push_back(schedule);
    }

    return plan;
}

} // namespace Slic3r
