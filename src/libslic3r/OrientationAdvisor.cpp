///|/ TigerSlicer Phase 24: tool-aware object orientation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "OrientationAdvisor.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *orientation_verdict_name(OrientationVerdict verdict)
{
    switch (verdict) {
    case OrientationVerdict::Ranked:   return "ranked";
    case OrientationVerdict::Unproven: return "unproven";
    case OrientationVerdict::Excluded: return "excluded";
    }
    return "excluded";
}

namespace {

struct Objective
{
    const char           *name { "" };
    std::optional<double> value;
    bool                  higher_is_better { false };
    const char           *best_phrase { "" };
    const char           *worst_phrase { "" };
};

std::optional<double> as_double(const std::optional<size_t> &value)
{
    if (!value.has_value())
        return std::nullopt;
    return double(*value);
}

// The objectives under consideration, in a fixed order so two candidates are
// always compared axis for axis.
std::vector<Objective> objectives_of(const OrientationCandidate &candidate,
                                     const OrientationPolicy    &policy)
{
    std::vector<Objective> objectives;

    if (policy.consider_support) {
        objectives.push_back({ "support volume", candidate.support_volume_mm3, false,
                               "needs the least support material",
                               "needs the most support material" });
        objectives.push_back({ "support interface area",
                               candidate.support_interface_area_mm2, false,
                               "leaves the least surface touched by support",
                               "leaves the most surface touched by support" });
    }
    if (policy.consider_toolchanges)
        objectives.push_back({ "toolchanges", as_double(candidate.toolchanges), false,
                               "needs the fewest toolchanges",
                               "needs the most toolchanges" });
    if (policy.consider_detail_access)
        objectives.push_back({ "unreachable detail",
                               as_double(candidate.unreachable_detail_regions), false,
                               "leaves no detail out of a tool's reach",
                               "puts the most detail out of a tool's reach" });
    if (policy.consider_finish)
        objectives.push_back({ "visible surface quality", candidate.visible_surface_quality,
                               true, "gives the best finish on the surfaces that show",
                               "gives the worst finish on the surfaces that show" });
    if (policy.consider_strength)
        objectives.push_back({ "strength alignment", candidate.strength_alignment, true,
                               "lines the layers up best against the load",
                               "lines the layers up worst against the load" });
    if (policy.consider_color_boundaries)
        objectives.push_back({ "colour boundaries crossed",
                               as_double(candidate.color_boundaries_crossed), false,
                               "cuts across the fewest colour boundaries",
                               "cuts across the most colour boundaries" });
    if (policy.consider_bridges)
        objectives.push_back({ "unanchored bridges", as_double(candidate.unanchored_bridges),
                               false, "leaves no bridge unanchored",
                               "leaves the most bridges unanchored" });
    if (policy.consider_time)
        objectives.push_back({ "print time", candidate.print_time_s, false,
                               "prints fastest", "prints slowest" });
    if (policy.consider_waste)
        objectives.push_back({ "waste", candidate.waste_mm3, false,
                               "wastes the least material", "wastes the most material" });

    return objectives;
}

bool at_least_as_good(const Objective &left, double right)
{
    return left.higher_is_better ? *left.value >= right - EPSILON
                                 : *left.value <= right + EPSILON;
}

bool strictly_better(const Objective &left, double right)
{
    return left.higher_is_better ? *left.value > right + EPSILON
                                 : *left.value < right - EPSILON;
}

// Whether `left` beats `right` on every objective. Domination is only ever
// claimed when both were measured on every axis: an orientation is not thrown
// away because nobody got round to measuring it.
bool dominates(const std::vector<Objective> &left, const std::vector<Objective> &right)
{
    bool any_strictly_better = false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (!left[index].value.has_value() || !right[index].value.has_value())
            return false;
        if (!at_least_as_good(left[index], *right[index].value))
            return false;
        if (strictly_better(left[index], *right[index].value))
            any_strictly_better = true;
    }
    return any_strictly_better;
}

} // namespace

OrientationAdvice advise_orientation(const std::vector<OrientationCandidate> &candidates,
                                     const OrientationPolicy                 &policy)
{
    OrientationAdvice advice;

    if (candidates.empty()) {
        advice.error = "no candidate orientations were supplied, so there is nothing to rank";
        return advice;
    }

    size_t considered = candidates.size();
    if (considered > policy.max_candidates) {
        considered = policy.max_candidates;
        advice.notes.push_back(std::to_string(candidates.size()) +
                               " orientations were supplied and only the first " +
                               std::to_string(policy.max_candidates) +
                               " were examined; the rest were not ranked at all");
    }

    if (objectives_of(candidates.front(), policy).empty()) {
        advice.error = "every objective was turned off, so there is nothing to rank the "
                       "orientations by";
        return advice;
    }

    // Assess each candidate, and set aside the ones that cannot be printed.
    std::vector<size_t>                 ranking;
    std::vector<std::vector<Objective>> measured(considered);

    for (size_t index = 0; index < considered; ++index) {
        const OrientationCandidate &candidate = candidates[index];

        OrientationAssessment assessment;
        assessment.candidate = candidate.id;
        measured[index]      = objectives_of(candidate, policy);

        for (const Objective &objective : measured[index])
            if (!objective.value.has_value())
                assessment.unknowns.push_back(objective.name);

        if (candidate.collides) {
            assessment.verdict = OrientationVerdict::Excluded;
            assessment.reasons.push_back(
                "this orientation collides and cannot be printed" +
                (candidate.collision_reason.empty() ? std::string()
                                                    : ": " + candidate.collision_reason));
            advice.assessments.push_back(assessment);
            continue;
        }
        if (policy.require_collision_check && !candidate.collision_checked) {
            assessment.verdict = OrientationVerdict::Unproven;
            assessment.reasons.push_back(
                "this orientation was never checked against the toolhead, so whether it "
                "collides is unknown rather than clear");
        }
        if (!assessment.unknowns.empty()) {
            assessment.verdict = assessment.verdict == OrientationVerdict::Excluded
                                     ? assessment.verdict
                                     : OrientationVerdict::Unproven;
            std::ostringstream missing;
            missing << "not measured on: ";
            for (size_t at = 0; at < assessment.unknowns.size(); ++at)
                missing << (at == 0 ? "" : ", ") << assessment.unknowns[at];
            missing << "; it cannot be shown to beat or lose to any other orientation";
            assessment.reasons.push_back(missing.str());
        }

        ranking.push_back(index);
        advice.assessments.push_back(assessment);
    }

    if (ranking.empty()) {
        advice.notes.push_back("every orientation examined collides, so none is recommended");
        return advice;
    }

    // Pareto fronts, peeled one at a time.
    auto assessment_of = [&](size_t index) -> OrientationAssessment & {
        for (OrientationAssessment &assessment : advice.assessments)
            if (assessment.candidate == candidates[index].id)
                return assessment;
        return advice.assessments.front();
    };

    for (size_t left : ranking)
        for (size_t right : ranking)
            if (left != right && dominates(measured[left], measured[right]))
                assessment_of(right).dominated_by.push_back(candidates[left].id);

    std::vector<size_t> remaining = ranking;
    size_t              rank      = 1;
    while (!remaining.empty()) {
        std::vector<size_t> front;
        for (size_t index : remaining) {
            const bool beaten =
                std::any_of(remaining.begin(), remaining.end(), [&](size_t other) {
                    return other != index && dominates(measured[other], measured[index]);
                });
            if (!beaten)
                front.push_back(index);
        }
        if (front.empty()) // cannot happen with a strict order, but never spin
            front = remaining;

        for (size_t index : front) {
            OrientationAssessment &assessment = assessment_of(index);
            assessment.pareto_rank            = rank;
            assessment.on_front               = rank == 1;
            if (rank == 1)
                advice.recommended.push_back(candidates[index].id);
        }

        std::vector<size_t> next;
        for (size_t index : remaining)
            if (std::find(front.begin(), front.end(), index) == front.end())
                next.push_back(index);
        remaining = next;
        ++rank;
    }

    // Strengths and weaknesses, against the field that was actually ranked.
    const size_t objective_count = measured[ranking.front()].size();
    for (size_t axis = 0; axis < objective_count; ++axis) {
        std::vector<size_t> with_values;
        for (size_t index : ranking)
            if (measured[index][axis].value.has_value())
                with_values.push_back(index);
        if (with_values.size() < 2)
            continue;

        const bool higher = measured[with_values.front()][axis].higher_is_better;
        double     best   = *measured[with_values.front()][axis].value;
        double     worst  = best;
        for (size_t index : with_values) {
            const double value = *measured[index][axis].value;
            best  = higher ? std::max(best, value) : std::min(best, value);
            worst = higher ? std::min(worst, value) : std::max(worst, value);
        }
        if (std::abs(best - worst) <= EPSILON)
            continue;

        for (size_t index : with_values) {
            const double value = *measured[index][axis].value;
            if (std::abs(value - best) <= EPSILON)
                assessment_of(index).strengths.push_back(measured[index][axis].best_phrase);
            else if (std::abs(value - worst) <= EPSILON)
                assessment_of(index).weaknesses.push_back(measured[index][axis].worst_phrase);
        }
    }

    if (advice.recommended.size() > 1)
        advice.notes.push_back(
            std::to_string(advice.recommended.size()) +
            " orientations are on the front and none of them beats the others on every "
            "count; which one to use is a judgement call and is left to you");

    const bool any_unproven =
        std::any_of(advice.assessments.begin(), advice.assessments.end(),
                    [&](const OrientationAssessment &assessment) {
                        return assessment.on_front &&
                               assessment.verdict == OrientationVerdict::Unproven;
                    });
    if (any_unproven)
        advice.notes.push_back("at least one recommended orientation is unproven; it is on "
                               "the front because nothing could be shown to beat it, not "
                               "because it was shown to be good");

    advice.notes.push_back("nothing has been rotated; these are proposals and one of them "
                           "has to be accepted before anything moves");
    return advice;
}

bool accept_orientation(const OrientationAdvice                 &advice,
                        const std::vector<OrientationCandidate> &candidates,
                        size_t candidate, bool user_accepted, AcceptedOrientation *out,
                        std::string *error)
{
    auto fail = [&](const std::string &message) {
        if (error != nullptr)
            *error = message;
        return false;
    };

    if (!user_accepted)
        return fail("no orientation is applied until the user accepts one; this phase "
                    "proposes and never rotates on its own");

    const OrientationCandidate *found = nullptr;
    for (const OrientationCandidate &value : candidates)
        if (value.id == candidate)
            found = &value;
    if (found == nullptr)
        return fail("there is no orientation " + std::to_string(candidate) + " to apply");

    for (const OrientationAssessment &assessment : advice.assessments)
        if (assessment.candidate == candidate &&
            assessment.verdict == OrientationVerdict::Excluded)
            return fail("orientation " + std::to_string(candidate) +
                        " was excluded and cannot be printed, so it is not applied even "
                        "though it was asked for by number");

    if (out != nullptr) {
        out->candidate = found->id;
        out->rx_deg    = found->rx_deg;
        out->ry_deg    = found->ry_deg;
        out->rz_deg    = found->rz_deg;
    }
    return true;
}

std::string orientation_cache_key(const std::string      &model_digest,
                                  const std::vector<int> &tools,
                                  const OrientationPolicy &policy)
{
    std::vector<int> sorted = tools;
    std::sort(sorted.begin(), sorted.end());

    std::ostringstream key;
    key << "orientation/1|model=" << model_digest << "|tools=";
    for (size_t index = 0; index < sorted.size(); ++index)
        key << (index == 0 ? "" : ",") << sorted[index];
    key << "|objectives=" << (policy.consider_support ? "s" : "-")
        << (policy.consider_time ? "t" : "-") << (policy.consider_waste ? "w" : "-")
        << (policy.consider_finish ? "f" : "-") << (policy.consider_strength ? "g" : "-")
        << (policy.consider_toolchanges ? "c" : "-")
        << (policy.consider_detail_access ? "d" : "-")
        << (policy.consider_color_boundaries ? "b" : "-")
        << (policy.consider_bridges ? "r" : "-")
        << (policy.require_collision_check ? "x" : "-");
    return key.str();
}

} // namespace Slic3r
