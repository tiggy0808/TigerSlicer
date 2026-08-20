///|/ TigerSlicer Phase 16: directional material-transition matrix.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "TransitionMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *transition_compatibility_name(TransitionCompatibility compatibility)
{
    switch (compatibility) {
    case TransitionCompatibility::Compatible:         return "compatible";
    case TransitionCompatibility::RequiresExtraPurge: return "requires extra purge";
    case TransitionCompatibility::Incompatible:       return "incompatible";
    case TransitionCompatibility::Unknown:            return "uncharacterized";
    }
    return "uncharacterized";
}

const char *standby_behavior_name(StandbyBehavior behavior)
{
    switch (behavior) {
    case StandbyBehavior::MayStayHot:       return "may stay hot";
    case StandbyBehavior::MustDropToStandby: return "must drop to standby";
    case StandbyBehavior::MustDropToIdle:   return "must drop to idle";
    case StandbyBehavior::Unknown:          return "unknown";
    }
    return "unknown";
}

bool TransitionEndpoint::matches(const TransitionEndpoint &concrete) const
{
    if (tool >= 0 && tool != concrete.tool)
        return false;
    if (!material.empty() && material != concrete.material)
        return false;
    if (!color.empty() && color != concrete.color)
        return false;
    return true;
}

int TransitionEndpoint::specificity() const
{
    // Tool outranks colour outranks material, because a rule naming a physical
    // tool is talking about that machine rather than about the material class.
    return (tool >= 0 ? 4 : 0) + (color.empty() ? 0 : 2) + (material.empty() ? 0 : 1);
}

std::string TransitionEndpoint::describe() const
{
    std::ostringstream out;
    out << (material.empty() ? "any material" : material);
    if (!color.empty())
        out << " " << color;
    if (tool >= 0)
        out << " on T" << tool;
    return out.str();
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

} // namespace

TransitionCost resolve_transition(const TransitionMatrix   &matrix,
                                  const TransitionEndpoint &from,
                                  const TransitionEndpoint &to,
                                  double                    source_temperature_c)
{
    const TransitionRule *best        = nullptr;
    size_t                best_index  = 0;
    int                   best_score  = -1;
    size_t                ties        = 0;

    for (size_t index = 0; index < matrix.rules.size(); ++index) {
        const TransitionRule &rule = matrix.rules[index];
        // Both ends must match in the direction asked for. The reverse rule is
        // never consulted: A -> B and B -> A are separate facts.
        if (!rule.from.matches(from) || !rule.to.matches(to))
            continue;
        const int score = rule.from.specificity() + rule.to.specificity();
        if (score > best_score) {
            best_score = score;
            best       = &rule;
            best_index = index;
            ties       = 0;
        } else if (score == best_score) {
            ++ties;
        }
    }

    TransitionCost cost;
    const TransitionRule &rule = best != nullptr ? *best : matrix.fallback;
    cost.provenance = best != nullptr
                          ? "rule " + std::to_string(best_index) + ": " +
                                rule.from.describe() + " -> " + rule.to.describe()
                          : "no rule matched; the matrix fallback was used";
    if (best == nullptr)
        cost.reasons.push_back("nothing in the matrix describes " + from.describe() + " -> " +
                               to.describe());
    if (ties > 0)
        // Reported, not silently broken by declaration order.
        cost.reasons.push_back(std::to_string(ties + 1) +
                               " rules are equally specific for this transition; the one "
                               "declared first was used");

    cost.compatibility     = rule.compatibility;
    cost.prime_volume_mm3  = rule.prime_volume_mm3;
    cost.brush_passes      = rule.brush_passes;
    cost.standby           = rule.standby;
    cost.destination_temperature_c = rule.destination_temperature_c;

    // Contamination sensitivity scales the purge; a destination that shows
    // every trace of the source needs more than the base volume.
    const double sensitivity = std::max(0., std::min(1., rule.contamination_sensitivity));
    cost.purge_volume_mm3    = rule.purge_volume_mm3 * (1. + sensitivity);
    if (sensitivity > EPSILON)
        cost.reasons.push_back(to.describe() + " tolerates little contamination, so the purge "
                               "is scaled by " + number(1. + sensitivity, 2));

    if (rule.destination_temperature_c.has_value())
        cost.temperature_change_c = *rule.destination_temperature_c - source_temperature_c;

    if (rule.compatibility == TransitionCompatibility::Incompatible)
        cost.reasons.push_back(rule.reason.empty()
                                   ? from.describe() + " -> " + to.describe() +
                                         " is declared incompatible, with no reason recorded"
                                   : rule.reason);
    else if (rule.compatibility == TransitionCompatibility::Unknown)
        cost.reasons.push_back(from.describe() + " -> " + to.describe() +
                               " has not been characterized");

    // Timing. A rate that was never configured makes the duration unknown; an
    // unpriced transition is not a free one.
    double      seconds = 0.;
    bool        priced  = true;
    std::string missing;
    if (cost.purge_volume_mm3 > EPSILON || cost.prime_volume_mm3 > EPSILON) {
        if (matrix.timing.purge_rate_mm3_s.has_value() &&
            *matrix.timing.purge_rate_mm3_s > EPSILON)
            seconds += (cost.purge_volume_mm3 + cost.prime_volume_mm3) /
                       *matrix.timing.purge_rate_mm3_s;
        else {
            priced  = false;
            missing = "no purge rate is configured";
        }
    }
    if (cost.brush_passes > 0) {
        if (matrix.timing.brush_pass_s.has_value())
            seconds += double(cost.brush_passes) * *matrix.timing.brush_pass_s;
        else {
            priced  = false;
            missing = "no brush pass duration is configured";
        }
    }
    if (cost.temperature_change_c.has_value() &&
        std::abs(*cost.temperature_change_c) > EPSILON) {
        const std::optional<double> &rate = *cost.temperature_change_c > 0.
                                                ? matrix.timing.heating_rate_c_s
                                                : matrix.timing.cooling_rate_c_s;
        if (rate.has_value() && *rate > EPSILON)
            seconds += std::abs(*cost.temperature_change_c) / *rate;
        else {
            priced  = false;
            missing = *cost.temperature_change_c > 0. ? "no heating rate is configured"
                                                      : "no cooling rate is configured";
        }
    }

    if (priced)
        cost.estimated_time_s = seconds;
    else
        cost.reasons.push_back(missing + ", so this transition cannot be priced");

    return cost;
}

TransitionValidation validate_transitions(const TransitionMatrix                &matrix,
                                          const std::vector<TransitionEndpoint> &sequence,
                                          const std::vector<double>             &temperatures,
                                          const TransitionPolicy                &policy)
{
    TransitionValidation validation;

    if (sequence.size() < 2) {
        validation.total_time_s = 0.;
        return validation;
    }
    if (!temperatures.empty() && temperatures.size() != sequence.size()) {
        validation.blocking.push_back("there are " + std::to_string(sequence.size()) +
                                      " material states but " +
                                      std::to_string(temperatures.size()) +
                                      " temperatures; the sequence cannot be priced");
        return validation;
    }

    double total_time = 0.;
    bool   all_priced = true;

    for (size_t index = 1; index < sequence.size(); ++index) {
        const TransitionEndpoint &from = sequence[index - 1];
        const TransitionEndpoint &to   = sequence[index];
        if (from.tool == to.tool && from.material == to.material && from.color == to.color)
            continue;

        const double source_c = temperatures.empty() ? 0. : temperatures[index - 1];
        const TransitionCost cost = resolve_transition(matrix, from, to, source_c);
        validation.costs.push_back(cost);

        const std::string where = "step " + std::to_string(index) + " (" + from.describe() +
                                  " -> " + to.describe() + ")";

        switch (cost.compatibility) {
        case TransitionCompatibility::Incompatible:
            validation.blocking.push_back(
                where + " is incompatible: " +
                (cost.reasons.empty() ? std::string("no reason recorded") : cost.reasons.back()));
            break;
        case TransitionCompatibility::Unknown:
            if (policy.allow_uncharacterized)
                validation.warnings.push_back(where + " has not been characterized; it is "
                                                      "being exported because uncharacterized "
                                                      "transitions were explicitly allowed");
            else
                validation.blocking.push_back(where + " has not been characterized, so its "
                                                      "purge and compatibility are unknown");
            break;
        case TransitionCompatibility::RequiresExtraPurge:
            validation.warnings.push_back(where + " needs " +
                                          number(cost.purge_volume_mm3, 1) +
                                          " mm3 of purge to run cleanly");
            break;
        case TransitionCompatibility::Compatible: break;
        }

        validation.total_purge_mm3 += cost.purge_volume_mm3;
        validation.total_prime_mm3 += cost.prime_volume_mm3;
        validation.total_brush_passes += cost.brush_passes;

        if (cost.estimated_time_s.has_value())
            total_time += *cost.estimated_time_s;
        else
            all_priced = false;
    }

    if (all_priced)
        validation.total_time_s = total_time;
    else
        // A partial total would understate the print, which is worse than
        // having no total at all.
        validation.warnings.push_back("at least one transition could not be priced, so no "
                                      "total transition time is reported");

    return validation;
}

} // namespace Slic3r
