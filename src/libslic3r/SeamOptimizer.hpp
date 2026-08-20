///|/ TigerSlicer Phase 21: cross-tool seam optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_SeamOptimizer_hpp_
#define slic3r_SeamOptimizer_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class SeamPolicy : uint8_t
{
    Aligned,
    Random,
    Rear,
    Hidden,
    Optimized,
};

const char *seam_policy_name(SeamPolicy policy);

// One place a seam could go on one loop.
struct SeamCandidate
{
    size_t index { 0 };
    double x { 0. }, y { 0. };

    // 0 (convex or flat) to 1 (a deep concave corner a seam hides in).
    double concavity { 0. };
    // Whether this point is on a surface the user will see.
    bool visible { true };
    // Whether the wipe or brush can actually reach here to finish the seam.
    bool reachable_for_wipe { true };

    // Travel from where the tool currently is.
    double travel_from_previous_mm { 0. };
};

struct SeamLoop
{
    size_t id { 0 };
    int    tool { 0 };
    double print_z { 0. };

    std::vector<SeamCandidate> candidates;

    // The user painted a seam here. This outranks every policy and every
    // heuristic; it is never overridden.
    std::optional<size_t> painted_candidate;

    // How much starting this loop costs in pressure terms after a pickup,
    // 0 (nothing) to 1 (a fresh pickup with no pressure). A high value argues
    // for a seam somewhere forgiving.
    double pickup_pressure_penalty { 0. };
};

struct SeamOptions
{
    SeamPolicy policy { SeamPolicy::Optimized };

    // Seams within this distance of a previously placed seam on a nearby layer
    // are penalized, so a visible scar does not accumulate down the part.
    double stack_radius_mm { 2.0 };
    double stack_layer_span_mm { 1.0 };
    double same_tool_stack_penalty { 1.0 };
    // Stacking seams across two different tools is worse than within one,
    // because the two tools' seams will not match each other.
    double cross_tool_stack_penalty { 2.0 };

    // Where "rear" is, in model coordinates.
    double rear_y { 1000. };
};

struct SeamChoice
{
    size_t loop { 0 };
    // Absent when no candidate could be chosen at all.
    std::optional<size_t> candidate;

    SeamPolicy applied_policy { SeamPolicy::Optimized };
    // True when this came from the user's painting rather than from a policy.
    bool from_painting { false };

    std::vector<std::string> reasons;
};

struct SeamPlan
{
    std::vector<SeamChoice>  choices;
    std::vector<std::string> notes;
    std::string              error;
};

// Place seams across every loop in the print.
//
// Explicit seam painting has the highest priority and is never overridden, in
// the same way Phase 19 never overrides an explicit colour. Otherwise the
// requested policy decides, and the optimized policy weighs concavity,
// visibility, travel, wipe access and pickup pressure while penalizing seams
// that would stack on top of earlier ones - especially across two tools, whose
// seams will never match each other.
//
// The random policy is deterministic: it is seeded from the loop identity, so
// the same model always slices to the same seams.
SeamPlan optimize_seams(const std::vector<SeamLoop> &loops, const SeamOptions &options);

} // namespace Slic3r

#endif // slic3r_SeamOptimizer_hpp_
