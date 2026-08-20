///|/ TigerSlicer Phase 18: thermal and cooling-aware scheduling.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CoolingScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *heat_model_disclaimer()
{
    return "bounded approximation from declared contributions, not a fluid simulation";
}

const char *cooling_remedy_name(CoolingRemedy remedy)
{
    switch (remedy) {
    case CoolingRemedy::None:             return "none";
    case CoolingRemedy::Delay:            return "delay";
    case CoolingRemedy::IncreaseFan:      return "increase fan";
    case CoolingRemedy::ReduceSpeed:      return "reduce speed";
    case CoolingRemedy::LowerTemperature: return "lower temperature";
    case CoolingRemedy::Reorder:          return "reorder";
    }
    return "none";
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

const MaterialThermalModel *find_material(const std::vector<MaterialThermalModel> &materials,
                                          const std::string                      &name)
{
    for (const MaterialThermalModel &material : materials)
        if (material.material == name)
            return &material;
    return nullptr;
}

bool nearby(const CoolingOperation &a, const CoolingOperation &b, double radius_mm)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy) <= radius_mm + EPSILON;
}

} // namespace

std::vector<HeatEstimate> estimate_heat_accumulation(
    const std::vector<CoolingOperation>     &operations,
    const std::vector<MaterialThermalModel> &materials,
    const EnvironmentModel                  &environment,
    const HeatModelBounds                   &bounds)
{
    std::vector<HeatEstimate> estimates;
    estimates.reserve(operations.size());

    for (size_t index = 0; index < operations.size(); ++index) {
        const CoolingOperation &operation = operations[index];

        HeatEstimate estimate;
        estimate.operation = index;
        estimate.basis.push_back(heat_model_disclaimer());

        const MaterialThermalModel *material = find_material(materials, operation.material);
        size_t                      known    = 0;

        // Start from what is actually being deposited.
        double temperature = operation.extrusion_temperature_c;
        estimate.basis.push_back("extruded at " +
                                 number(operation.extrusion_temperature_c, 0) + " C");

        // The environment the part sits in. Each term is only applied when it
        // was recorded; an unrecorded chamber is not a cold one.
        double ambient = 0.;
        if (environment.chamber_temperature_c.has_value()) {
            ambient = *environment.chamber_temperature_c;
            ++known;
            estimate.basis.push_back("chamber at " + number(ambient, 0) + " C");
        } else if (environment.ambient_temperature_c.has_value()) {
            ambient = *environment.ambient_temperature_c;
            ++known;
            estimate.basis.push_back("ambient at " + number(ambient, 0) + " C");
        } else {
            estimate.gaps.push_back("neither chamber nor ambient temperature was recorded");
        }

        if (operation.is_first_layer && environment.bed_temperature_c.has_value()) {
            ambient = std::max(ambient, *environment.bed_temperature_c);
            estimate.basis.push_back("first layer over a bed at " +
                                     number(*environment.bed_temperature_c, 0) + " C");
        }

        // Cooling towards ambient over the layer time, scaled by how readily
        // the material sheds heat.
        double cooling_factor = 1.;
        if (material != nullptr && material->cooling_factor.has_value()) {
            cooling_factor = *material->cooling_factor;
            ++known;
        } else {
            estimate.gaps.push_back("no cooling factor is recorded for " +
                                    (operation.material.empty() ? "an unrecorded material"
                                                                : operation.material));
        }

        const double layer_time = std::max(0., operation.layer_time_s);
        // A deliberately crude exponential decay. Its only job is to be
        // monotonic in the inputs and bounded; it is not a physical claim.
        const double shed = (temperature - ambient) *
                            (1. - std::exp(-cooling_factor * layer_time / 30.));
        temperature -= shed;
        estimate.basis.push_back("shed " + number(shed, 1) + " C over " +
                                 number(layer_time, 1) + " s of layer time");

        // Fan.
        const double fan = std::max(0., std::min(1., operation.fan_fraction));
        if (fan > EPSILON) {
            const double fan_shed = (temperature - ambient) * 0.4 * fan;
            temperature -= fan_shed;
            estimate.basis.push_back("fan at " + number(fan * 100., 0) + "% removed a further " +
                                     number(fan_shed, 1) + " C");
        }

        // Thin walls hold less heat; thick ones hold more.
        if (operation.wall_thickness_mm > EPSILON) {
            const double mass_term = std::min(15., operation.wall_thickness_mm * 6.);
            temperature += mass_term;
            ++known;
            estimate.basis.push_back(number(operation.wall_thickness_mm, 2) +
                                     " mm of wall retains " + number(mass_term, 1) + " C");
        } else {
            estimate.gaps.push_back("no wall thickness was recorded");
        }

        // Recent nearby work, which is the term the schedule can actually
        // change by reordering.
        double neighbour_term = 0.;
        size_t neighbours     = 0;
        const size_t first = index > bounds.recent_window ? index - bounds.recent_window : 0;
        for (size_t other = first; other < index; ++other) {
            const CoolingOperation &previous = operations[other];
            if (!nearby(operation, previous, bounds.neighbourhood_mm))
                continue;
            if (std::abs(previous.print_z - operation.print_z) > operation.wall_thickness_mm + 1.)
                continue;
            ++neighbours;
            neighbour_term += 4. * (previous.deposited_volume_mm3 > 0. ? 1. : 0.);
        }
        if (neighbours > 0) {
            temperature += neighbour_term;
            estimate.basis.push_back(std::to_string(neighbours) +
                                     " recent nearby operation(s) added " +
                                     number(neighbour_term, 1) + " C");
        }

        // Bridges and overhangs have nothing under them to conduct into.
        if (operation.is_bridge || operation.is_overhang) {
            temperature += 6.;
            estimate.basis.push_back(std::string(operation.is_bridge ? "a bridge" : "an overhang") +
                                     " has no material beneath it to conduct into");
        }

        // The clamp is what makes this a bounded approximation. The bound is
        // on the UPWARD excursion, which is the direction that matters and the
        // direction the model can get dangerously wrong. Downward it stops at
        // ambient, because a part cannot be estimated cooler than what
        // surrounds it - and clamping the low side to
        // `extrusion - max_delta` would push a thoroughly cooled part back up
        // and make it look hot.
        const double low  = ambient;
        const double high = operation.extrusion_temperature_c + bounds.max_delta_c;
        if (temperature < low || temperature > high) {
            estimate.clamped = true;
            temperature      = std::max(low, std::min(high, temperature));
            estimate.basis.push_back("the result was clamped to the model's " +
                                     number(bounds.max_delta_c, 0) + " C bound");
        }
        estimate.estimated_temperature_c = temperature;

        // Confidence follows what was actually known, never the arithmetic.
        if (material == nullptr || !material->softening_c.has_value()) {
            estimate.gaps.push_back("no softening temperature is recorded, so there is nothing "
                                    "to compare this estimate against");
            estimate.confidence = Confidence::Insufficient;
        } else if (estimate.gaps.empty() && known >= 3)
            estimate.confidence = Confidence::High;
        else if (estimate.gaps.size() <= 1)
            estimate.confidence = Confidence::Medium;
        else
            estimate.confidence = Confidence::Low;

        estimates.push_back(estimate);
    }

    return estimates;
}

CoolingPlan plan_cooling(const std::vector<CoolingOperation>     &operations,
                         const std::vector<MaterialThermalModel> &materials,
                         const EnvironmentModel                  &environment,
                         const HeatModelBounds                   &bounds,
                         const CoolingPolicy                     &policy)
{
    CoolingPlan plan;

    if (operations.empty()) {
        plan.error = "there are no operations to schedule cooling for";
        return plan;
    }

    plan.estimates = estimate_heat_accumulation(operations, materials, environment, bounds);
    plan.notes.push_back(std::string("heat estimates are a ") + heat_model_disclaimer());

    for (size_t index = 0; index < operations.size(); ++index) {
        const CoolingOperation &operation = operations[index];
        const HeatEstimate     &estimate  = plan.estimates[index];

        const MaterialThermalModel *material = find_material(materials, operation.material);
        if (material == nullptr || !material->softening_c.has_value()) {
            plan.notes.push_back("operation " + std::to_string(operation.index) +
                                 " has no softening temperature recorded, so no cooling "
                                 "decision is made for it");
            continue;
        }

        const double over =
            estimate.estimated_temperature_c - (*material->softening_c + policy.tolerance_c);
        if (over <= EPSILON)
            continue;

        const std::string why = "operation " + std::to_string(operation.index) +
                                " is estimated at " +
                                number(estimate.estimated_temperature_c, 1) + " C, " +
                                number(over, 1) + " C above where " + material->material +
                                " softens";

        // A decision resting on a weak estimate gets only the remedy that
        // cannot make anything worse.
        const bool confident = estimate.confidence >= policy.confident_at;
        if (!confident) {
            CoolingAction action;
            action.operation = operation.index;
            action.remedy    = CoolingRemedy::Delay;
            action.value     = policy.max_delay_s;
            action.explanation.push_back(why);
            action.explanation.push_back(
                "the estimate behind this is only " +
                std::string(confidence_name(estimate.confidence)) +
                " confidence, so only a delay is proposed: it cannot degrade the part");
            plan.actions.push_back(action);
            continue;
        }

        std::vector<std::string> unavailable;

        // Fan first: it is the cheapest and most direct, but never on the
        // first layer, where it costs adhesion.
        if (operation.fan_fraction < 1. - EPSILON && !operation.is_first_layer) {
            CoolingAction action;
            action.operation = operation.index;
            action.remedy    = CoolingRemedy::IncreaseFan;
            action.value     = std::min(1., operation.fan_fraction + 0.25);
            action.explanation.push_back(why);
            action.explanation.push_back("fan raised from " +
                                         number(operation.fan_fraction * 100., 0) + "% to " +
                                         number(action.value * 100., 0) +
                                         "%, which cools without changing geometry");
            plan.actions.push_back(action);
            continue;
        }
        if (operation.is_first_layer)
            unavailable.push_back("more fan would cost first-layer adhesion");
        else
            unavailable.push_back("the fan is already at full");

        // Reordering, to move this work later and give what is underneath it
        // more time to cool. Deferring is the only direction that helps, so
        // the question is whether anything is waiting on this operation - not
        // whether it is waiting on anything itself.
        const bool reorderable =
            std::none_of(operations.begin(), operations.end(),
                         [&](const CoolingOperation &other) {
                             return std::find(other.depends_on.begin(), other.depends_on.end(),
                                              operation.index) != other.depends_on.end();
                         });
        if (reorderable && !operation.is_first_layer) {
            CoolingAction action;
            action.operation = operation.index;
            action.remedy    = CoolingRemedy::Reorder;
            action.explanation.push_back(why);
            action.explanation.push_back("nothing is waiting on this operation, so deferring "
                                         "it away from the recent nearby work costs no "
                                         "dependency");
            plan.actions.push_back(action);
            continue;
        }
        unavailable.push_back(operation.is_first_layer
                                  ? "the first layer cannot be reordered without risking "
                                    "adhesion"
                                  : "other operations are waiting on this one, so it cannot "
                                    "be deferred");

        // Slowing down, which lengthens the layer and gives more cooling time.
        if (policy.max_speed_reduction > EPSILON && !operation.is_bridge) {
            CoolingAction action;
            action.operation = operation.index;
            action.remedy    = CoolingRemedy::ReduceSpeed;
            action.value     = 1. - policy.max_speed_reduction;
            action.explanation.push_back(why);
            action.explanation.push_back("speed reduced to " +
                                         number(action.value * 100., 0) +
                                         "%, lengthening the layer so more heat is shed");
            plan.actions.push_back(action);
            continue;
        }
        if (operation.is_bridge)
            unavailable.push_back("slowing a bridge would let it sag further");

        // Lowering the temperature last, bounded, and never on the first
        // layer.
        if (policy.max_temperature_drop_c > EPSILON && !operation.is_first_layer) {
            CoolingAction action;
            action.operation = operation.index;
            action.remedy    = CoolingRemedy::LowerTemperature;
            action.value = std::min(policy.max_temperature_drop_c, std::max(0., over));
            action.explanation.push_back(why);
            action.explanation.push_back("extrusion temperature lowered by " +
                                         number(action.value, 1) + " C, within the " +
                                         number(policy.max_temperature_drop_c, 1) +
                                         " C bound this policy allows");
            plan.actions.push_back(action);
            continue;
        }
        unavailable.push_back("lowering the first-layer temperature would cost adhesion");

        std::ostringstream line;
        line << why << ", and nothing can be done about it: ";
        for (size_t at = 0; at < unavailable.size(); ++at)
            line << (at > 0 ? "; " : "") << unavailable[at];
        plan.unresolved.push_back(line.str());
    }

    return plan;
}

} // namespace Slic3r
