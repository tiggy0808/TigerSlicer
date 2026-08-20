///|/ TigerSlicer Phase 21: cross-tool seam optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SeamOptimizer.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *seam_policy_name(SeamPolicy policy)
{
    switch (policy) {
    case SeamPolicy::Aligned:   return "aligned";
    case SeamPolicy::Random:    return "random";
    case SeamPolicy::Rear:      return "rear";
    case SeamPolicy::Hidden:    return "hidden";
    case SeamPolicy::Optimized: return "optimized";
    }
    return "optimized";
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

struct PlacedSeam
{
    double x { 0. }, y { 0. }, z { 0. };
    int    tool { 0 };
};

// A deterministic hash, so the "random" policy produces the same seams every
// time the same model is sliced. A slicer that moved its seams between runs
// would be unusable for iterating on a part.
size_t stable_hash(size_t loop_id, size_t candidates)
{
    size_t value = loop_id * 0x9E3779B97F4A7C15ull;
    value ^= value >> 29;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 32;
    return candidates == 0 ? 0 : value % candidates;
}

} // namespace

SeamPlan optimize_seams(const std::vector<SeamLoop> &loops, const SeamOptions &options)
{
    SeamPlan plan;

    if (loops.empty()) {
        plan.error = "there are no loops to place seams on";
        return plan;
    }

    std::vector<PlacedSeam> placed;

    for (const SeamLoop &loop : loops) {
        SeamChoice choice;
        choice.loop           = loop.id;
        choice.applied_policy = options.policy;

        if (loop.candidates.empty()) {
            choice.reasons.push_back("this loop offers no seam candidates at all");
            plan.choices.push_back(choice);
            continue;
        }

        // The user's painting is the highest authority here and is applied
        // before any policy is even consulted.
        if (loop.painted_candidate.has_value()) {
            if (*loop.painted_candidate >= loop.candidates.size()) {
                choice.reasons.push_back("the painted seam names a candidate this loop does "
                                         "not have, so it could not be applied");
                plan.choices.push_back(choice);
                continue;
            }
            choice.candidate     = *loop.painted_candidate;
            choice.from_painting = true;
            choice.reasons.push_back("the seam is painted here, which outranks every policy");
            const SeamCandidate &at = loop.candidates[*loop.painted_candidate];
            placed.push_back({ at.x, at.y, loop.print_z, loop.tool });
            plan.choices.push_back(choice);
            continue;
        }

        size_t best        = 0;
        double best_score  = -1e30;
        size_t ties        = 0;

        for (size_t index = 0; index < loop.candidates.size(); ++index) {
            const SeamCandidate &candidate = loop.candidates[index];
            double               score     = 0.;

            switch (options.policy) {
            case SeamPolicy::Random:
                score = index == stable_hash(loop.id, loop.candidates.size()) ? 1. : 0.;
                break;
            case SeamPolicy::Rear:
                score = -std::abs(options.rear_y - candidate.y);
                break;
            case SeamPolicy::Hidden:
                score = (candidate.visible ? 0. : 10.) + candidate.concavity * 5.;
                break;
            case SeamPolicy::Aligned:
                // Stay where the last seam was, so the scar is one straight
                // line rather than a scattering.
                if (placed.empty())
                    score = candidate.concavity * 5.;
                else {
                    const PlacedSeam &last = placed.back();
                    score = -std::sqrt((candidate.x - last.x) * (candidate.x - last.x) +
                                       (candidate.y - last.y) * (candidate.y - last.y));
                }
                break;
            case SeamPolicy::Optimized:
                score = candidate.concavity * 6.;
                if (!candidate.visible)
                    score += 8.;
                if (!candidate.reachable_for_wipe)
                    // A seam the wipe cannot reach never gets finished.
                    score -= 12.;
                score -= candidate.travel_from_previous_mm * 0.05;
                // After a pickup there is little pressure, so a forgiving
                // concave corner matters more than usual.
                score += loop.pickup_pressure_penalty * candidate.concavity * 4.;
                break;
            }

            // Stacking penalty, for every policy except the user's painting.
            if (options.policy != SeamPolicy::Aligned) {
                for (const PlacedSeam &earlier : placed) {
                    if (std::abs(earlier.z - loop.print_z) > options.stack_layer_span_mm)
                        continue;
                    const double distance =
                        std::sqrt((candidate.x - earlier.x) * (candidate.x - earlier.x) +
                                  (candidate.y - earlier.y) * (candidate.y - earlier.y));
                    if (distance > options.stack_radius_mm)
                        continue;
                    // Two tools' seams will never match each other, so stacking
                    // across a toolchange is worse than stacking within one.
                    score -= earlier.tool == loop.tool ? options.same_tool_stack_penalty
                                                       : options.cross_tool_stack_penalty;
                }
            }

            if (score > best_score + EPSILON) {
                best_score = score;
                best       = index;
                ties       = 0;
            } else if (std::abs(score - best_score) <= EPSILON) {
                ++ties;
            }
        }

        choice.candidate = best;
        const SeamCandidate &at = loop.candidates[best];

        std::ostringstream why;
        why << seam_policy_name(options.policy) << " policy chose candidate " << best;
        if (options.policy == SeamPolicy::Optimized) {
            why << ": concavity " << number(at.concavity, 2)
                << (at.visible ? ", visible" : ", hidden")
                << (at.reachable_for_wipe ? ", wipe can reach it"
                                          : ", the wipe cannot reach it");
        }
        choice.reasons.push_back(why.str());

        if (options.policy == SeamPolicy::Optimized && !at.reachable_for_wipe)
            choice.reasons.push_back("every candidate on this loop is out of the wipe's reach, "
                                     "so this seam will not be finished cleanly");
        if (ties > 0)
            choice.reasons.push_back(std::to_string(ties + 1) +
                                     " candidates scored identically; the first was used");

        placed.push_back({ at.x, at.y, loop.print_z, loop.tool });
        plan.choices.push_back(choice);
    }

    return plan;
}

} // namespace Slic3r
