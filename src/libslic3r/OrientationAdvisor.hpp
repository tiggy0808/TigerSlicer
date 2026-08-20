///|/ TigerSlicer Phase 24: tool-aware object orientation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_OrientationAdvisor_hpp_
#define slic3r_OrientationAdvisor_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class OrientationVerdict : uint8_t
{
    Ranked,
    // Ranked, but something needed to judge it was never checked.
    Unproven,
    // Cannot be printed at all in this orientation.
    Excluded,
};

const char *orientation_verdict_name(OrientationVerdict verdict);

// One orientation, with whatever has actually been measured about it. Every
// measurement is optional: an orientation nobody has evaluated on some axis is
// not the same as one that scored zero there, and the two never merge.
struct OrientationCandidate
{
    size_t id { 0 };
    double rx_deg { 0. }, ry_deg { 0. }, rz_deg { 0. };

    std::optional<double> support_volume_mm3;
    std::optional<double> support_interface_area_mm2;
    std::optional<double> print_time_s;
    std::optional<double> waste_mm3;

    std::optional<size_t> toolchanges;
    std::optional<size_t> unreachable_detail_regions;
    std::optional<size_t> color_boundaries_crossed;
    std::optional<size_t> unanchored_bridges;

    // 0 (poor) to 1 (good).
    std::optional<double> visible_surface_quality;
    // How well the layers line up against the load, 0 to 1.
    std::optional<double> strength_alignment;

    // Whether this orientation was actually checked against the toolhead, and
    // what happened. Unchecked is reported, never treated as clear.
    bool        collision_checked { true };
    bool        collides { false };
    std::string collision_reason;
};

struct OrientationPolicy
{
    bool consider_support { true };
    bool consider_time { true };
    bool consider_waste { true };
    bool consider_finish { true };
    bool consider_strength { true };
    bool consider_toolchanges { true };
    bool consider_detail_access { true };
    bool consider_color_boundaries { true };
    bool consider_bridges { true };

    // An orientation whose collision check never ran is reported as unproven
    // rather than quietly ranked as clear.
    bool require_collision_check { true };

    size_t max_candidates { 200 };
};

struct OrientationAssessment
{
    size_t             candidate { 0 };
    OrientationVerdict verdict { OrientationVerdict::Ranked };

    // 1 is the Pareto front. Zero means the candidate was excluded and never
    // took part in the ranking.
    size_t pareto_rank { 0 };
    bool   on_front { false };

    // Candidates that beat this one on every measured count.
    std::vector<size_t> dominated_by;

    std::vector<std::string> strengths;
    std::vector<std::string> weaknesses;
    // Objectives that were never measured for this orientation, by name.
    std::vector<std::string> unknowns;
    std::vector<std::string> reasons;
};

struct OrientationAdvice
{
    std::vector<OrientationAssessment> assessments;
    // Every candidate on the Pareto front, in candidate order. Deliberately
    // not narrowed to one: where the front holds more than one orientation,
    // none of them is better than the others on every count.
    std::vector<size_t> recommended;

    // Always false. This phase never rotates anything; see accept_orientation.
    bool applied { false };

    std::vector<std::string> notes;
    std::string              error;
};

// The orientation the caller has chosen to apply, once they have chosen it.
struct AcceptedOrientation
{
    size_t candidate { 0 };
    double rx_deg { 0. }, ry_deg { 0. }, rz_deg { 0. };
};

// Rank candidate orientations into Pareto fronts and explain each one.
//
// Domination is only claimed between two orientations that were measured on
// every objective under consideration. If either is missing a measurement, one
// cannot be shown to beat the other, and neither is dropped on the strength of
// a guess.
//
// The result is advice. Nothing is rotated here and `applied` is always false.
OrientationAdvice advise_orientation(const std::vector<OrientationCandidate> &candidates,
                                     const OrientationPolicy                 &policy);

// Apply an orientation the user has accepted.
//
// Fails when the user has not accepted anything, when the named candidate does
// not exist, or when it was excluded - an orientation that cannot be printed
// is not applied just because someone asked for it by number.
bool accept_orientation(const OrientationAdvice                 &advice,
                        const std::vector<OrientationCandidate> &candidates,
                        size_t candidate, bool user_accepted, AcceptedOrientation *out,
                        std::string *error);

// A key for reusing an analysis. Two requests share a key only when the model,
// the exact set of tools and the objectives under consideration all match, so
// an analysis run for a different machine is never handed back.
std::string orientation_cache_key(const std::string      &model_digest,
                                  const std::vector<int> &tools,
                                  const OrientationPolicy &policy);

} // namespace Slic3r

#endif // slic3r_OrientationAdvisor_hpp_
