///|/ TigerSlicer Phase 20: heuristic structural infill planner.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "StructuralInfill.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *structural_disclaimer()
{
    return "heuristic guidance, not a certified structural analysis";
}

const char *structural_priority_name(StructuralPriority priority)
{
    switch (priority) {
    case StructuralPriority::Stiffness: return "stiffness";
    case StructuralPriority::Weight:    return "weight";
    case StructuralPriority::Balanced:  return "balanced";
    }
    return "balanced";
}

const char *safety_preference_name(SafetyPreference preference)
{
    switch (preference) {
    case SafetyPreference::Conservative: return "conservative";
    case SafetyPreference::Nominal:      return "nominal";
    }
    return "conservative";
}

const char *structural_change_kind_name(StructuralChangeKind kind)
{
    switch (kind) {
    case StructuralChangeKind::InfillDensity:   return "infill density";
    case StructuralChangeKind::InfillDirection: return "infill direction";
    case StructuralChangeKind::WallCount:       return "wall count";
    case StructuralChangeKind::SolidLayers:     return "solid layers";
    case StructuralChangeKind::Rib:             return "rib";
    case StructuralChangeKind::Reinforcement:   return "reinforcement";
    }
    return "infill density";
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

// The dominant in-plane direction of the applied loads, in degrees. Loads with
// no magnitude count as unit weight, and that is stated rather than assumed
// away.
std::optional<double> dominant_angle_deg(const std::vector<LoadDirection> &loads,
                                         bool                             *any_unweighted)
{
    double sum_x = 0., sum_y = 0.;
    bool   any   = false;
    for (const LoadDirection &load : loads) {
        const double length = std::sqrt(load.x * load.x + load.y * load.y + load.z * load.z);
        if (length <= EPSILON)
            continue;
        const double weight = load.magnitude_n.value_or(1.);
        if (!load.magnitude_n.has_value() && any_unweighted != nullptr)
            *any_unweighted = true;
        sum_x += weight * load.x / length;
        sum_y += weight * load.y / length;
        any = true;
    }
    if (!any || (std::abs(sum_x) <= EPSILON && std::abs(sum_y) <= EPSILON))
        return std::nullopt;
    return std::atan2(sum_y, sum_x) * 180. / M_PI;
}

} // namespace

StructuralPlan plan_structural_infill(const std::vector<LoadDirection>   &loads,
                                      const std::vector<MountingSurface> &mounts,
                                      const std::vector<CriticalHole>    &holes,
                                      const StructuralSettings           &current,
                                      const std::vector<StructuralTool>  &tools,
                                      const StructuralOptions            &options)
{
    StructuralPlan plan;
    plan.notes.push_back(std::string("every proposal below is ") + structural_disclaimer());

    if (loads.empty() && mounts.empty() && holes.empty()) {
        plan.error = "no loads, mounting surfaces or critical holes were described, so there "
                     "is nothing to plan structure around";
        return plan;
    }

    const bool conservative = options.safety == SafetyPreference::Conservative;

    auto add = [&](StructuralChangeKind kind, double from, double to,
                   const std::string &why, std::optional<int> tool = std::nullopt) {
        StructuralChange change;
        change.kind             = kind;
        change.current_value    = from;
        change.proposed_value   = to;
        change.tool = tool;
        // Only the quantities where a smaller number really is a weaker part
        // count as a reduction. An infill angle of 0 is not weaker than 45,
        // and a rib or reinforcement is only ever additive.
        const bool strength_ordered = kind == StructuralChangeKind::InfillDensity ||
                                      kind == StructuralChangeKind::WallCount ||
                                      kind == StructuralChangeKind::SolidLayers;
        change.reduces_strength = strength_ordered && to < from - EPSILON;
        // The disclaimer leads every reason list so it cannot be read past.
        change.reasons.push_back(structural_disclaimer());
        change.reasons.push_back(why);

        if (change.reduces_strength && !options.allow_strength_reduction) {
            plan.withheld.push_back(
                std::string(structural_change_kind_name(kind)) + " would drop from " +
                number(from, 2) + " to " + number(to, 2) +
                ", which leaves the part weaker; strength is never reduced without being "
                "asked for, so this was not proposed");
            return;
        }
        plan.changes.push_back(change);
    };

    // Infill direction: align with the dominant load.
    bool unweighted = false;
    const std::optional<double> angle = dominant_angle_deg(loads, &unweighted);
    if (angle.has_value()) {
        if (unweighted)
            plan.notes.push_back("some load directions carry no magnitude and were counted as "
                                 "unit weight, so the dominant direction is rough");
        if (std::abs(*angle - current.infill_angle_deg) > 5.)
            add(StructuralChangeKind::InfillDirection, current.infill_angle_deg, *angle,
                "aligning infill with the dominant load direction puts material along the "
                "path the force takes");
    } else if (!loads.empty()) {
        plan.notes.push_back("the described loads cancel out in plane, so no infill direction "
                             "is proposed");
    }

    // Density and walls, from priority and how much load there is.
    double density = current.infill_density;
    if (options.priority == StructuralPriority::Stiffness)
        density = std::min(1., current.infill_density + (conservative ? 0.25 : 0.15));
    else if (options.priority == StructuralPriority::Weight)
        density = std::max(0.05, current.infill_density - 0.05);

    if (std::abs(density - current.infill_density) > EPSILON)
        add(StructuralChangeKind::InfillDensity, current.infill_density, density,
            std::string("the request prioritizes ") +
                structural_priority_name(options.priority) + " with a " +
                safety_preference_name(options.safety) + " safety preference");

    if (options.priority != StructuralPriority::Weight && !loads.empty())
        add(StructuralChangeKind::WallCount, double(current.wall_count),
            double(current.wall_count + (conservative ? 2 : 1)),
            "walls carry far more load than infill of the same mass, so they are increased "
            "before density is");

    // Mounting surfaces want solid material under the fasteners.
    size_t fixed = 0;
    for (const MountingSurface &mount : mounts)
        if (mount.fixed)
            ++fixed;
    if (fixed > 0)
        add(StructuralChangeKind::SolidLayers, double(current.solid_bottom_layers),
            double(current.solid_bottom_layers + 2),
            std::to_string(fixed) + " fixed mounting surface(s) need solid material to spread "
            "the fastener load into");

    // Critical holes want material around them, and an unstated requirement is
    // reported rather than guessed at.
    for (const CriticalHole &hole : holes) {
        if (!hole.required_wall_mm.has_value()) {
            plan.notes.push_back("hole " + std::to_string(hole.id) +
                                 " is marked critical but no required wall thickness was "
                                 "given, so none is proposed for it");
            continue;
        }
        add(StructuralChangeKind::Rib, 0., *hole.required_wall_mm,
            "hole " + std::to_string(hole.id) + " is critical and wants " +
                number(*hole.required_wall_mm, 2) + " mm of material around it");
    }

    // Reinforcement, only from a tool whose material stiffness is actually
    // known.
    const StructuralTool *stiffest = nullptr;
    for (const StructuralTool &tool : tools) {
        if (!tool.stiffness_factor.has_value())
            continue;
        if (stiffest == nullptr || *tool.stiffness_factor > *stiffest->stiffness_factor)
            stiffest = &tool;
    }
    if (options.priority == StructuralPriority::Stiffness) {
        if (stiffest != nullptr && *stiffest->stiffness_factor > 1. + EPSILON)
            add(StructuralChangeKind::Reinforcement, 0., *stiffest->stiffness_factor,
                "T" + std::to_string(stiffest->tool) + " carries " + stiffest->material +
                    ", the stiffest characterized material on this machine",
                stiffest->tool);
        else if (stiffest == nullptr && !tools.empty())
            plan.notes.push_back("no tool has a recorded stiffness factor, so no reinforcement "
                                 "material can be proposed");
    }

    // Cost. Without a part volume there is nothing to turn a density change
    // into, and the plan says so rather than reporting zero.
    const auto density_change =
        std::find_if(plan.changes.begin(), plan.changes.end(),
                     [](const StructuralChange &change) {
                         return change.kind == StructuralChangeKind::InfillDensity;
                     });
    if (density_change != plan.changes.end()) {
        if (options.part_volume_mm3.has_value()) {
            const double delta = (density_change->proposed_value -
                                  density_change->current_value) *
                                 *options.part_volume_mm3;
            plan.material_delta_mm3 = delta;
            double throughput = 0.;
            for (const StructuralTool &tool : tools)
                throughput = std::max(throughput, tool.throughput_mm3_s);
            if (throughput > EPSILON)
                plan.time_delta_s = delta / throughput;
            else
                plan.notes.push_back("no tool throughput is known, so the time effect of this "
                                     "plan is not reported");
        } else {
            plan.notes.push_back("no part volume was supplied, so the material and time effect "
                                 "of this plan cannot be reported");
        }
    } else {
        plan.material_delta_mm3 = 0.;
        plan.time_delta_s       = 0.;
    }

    return plan;
}

} // namespace Slic3r
