///|/ TigerSlicer Phase 16: directional material-transition matrix.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_TransitionMatrix_hpp_
#define slic3r_TransitionMatrix_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// One end of a transition. A rule may leave any field empty, which means "any"
// - that is how a material-level rule covers every colour of that material.
struct TransitionEndpoint
{
    // -1 means the rule does not care which tool.
    int         tool { -1 };
    std::string material;
    std::string color;

    bool matches(const TransitionEndpoint &concrete) const;
    // How specific this pattern is, for choosing between rules that both match.
    int specificity() const;

    std::string describe() const;
};

enum class TransitionCompatibility : uint8_t
{
    Compatible,
    // Printable, but only after more purging than the default.
    RequiresExtraPurge,
    // Declared impossible. This is a first-class state, not an absence.
    Incompatible,
    // Nobody has characterized this pair. Never treated as Compatible.
    Unknown,
};

const char *transition_compatibility_name(TransitionCompatibility compatibility);

// What standby is allowed to do across a transition. A material that degrades
// while parked hot cannot simply be left at printing temperature.
enum class StandbyBehavior : uint8_t
{
    MayStayHot,
    MustDropToStandby,
    MustDropToIdle,
    Unknown,
};

const char *standby_behavior_name(StandbyBehavior behavior);

// A directional rule. `from -> to` says nothing whatsoever about `to -> from`;
// the matrix never mirrors a rule, because the cost of putting white after
// black is not the cost of putting black after white.
struct TransitionRule
{
    TransitionEndpoint from;
    TransitionEndpoint to;

    TransitionCompatibility compatibility { TransitionCompatibility::Unknown };

    // What the destination material costs to establish cleanly.
    double purge_volume_mm3 { 0. };
    double prime_volume_mm3 { 0. };
    int    brush_passes { 0 };

    // How much residue the destination tolerates, 0 (indifferent) to 1
    // (any contamination shows). Scales the purge on top of the base volume.
    double contamination_sensitivity { 0. };

    // The temperature the destination needs. Absent means the rule does not
    // change it.
    std::optional<double> destination_temperature_c;

    StandbyBehavior standby { StandbyBehavior::Unknown };

    // Required when the rule declares Incompatible: a refusal names what is
    // responsible for it.
    std::string reason;
};

// Rates used to turn volumes and passes into time. Every field is optional
// because an unconfigured rate must produce an unknown duration rather than a
// confident zero.
struct TransitionTimingModel
{
    std::optional<double> purge_rate_mm3_s;
    std::optional<double> brush_pass_s;
    std::optional<double> heating_rate_c_s;
    std::optional<double> cooling_rate_c_s;
};

struct TransitionMatrix
{
    std::vector<TransitionRule> rules;
    TransitionTimingModel       timing;

    // Applied when no rule matches at all. Its compatibility is Unknown by
    // default on purpose.
    TransitionRule fallback;
};

// The resolved cost of one concrete transition.
struct TransitionCost
{
    TransitionCompatibility compatibility { TransitionCompatibility::Unknown };

    double purge_volume_mm3 { 0. };
    double prime_volume_mm3 { 0. };
    int    brush_passes { 0 };

    std::optional<double> destination_temperature_c;
    std::optional<double> temperature_change_c;
    StandbyBehavior       standby { StandbyBehavior::Unknown };

    // Absent when the timing model does not have the rate this transition
    // needs. An unpriced transition is not a free one.
    std::optional<double> estimated_time_s;

    // Which rule answered, so a surprising number can be traced to its source.
    std::string              provenance;
    std::vector<std::string> reasons;

    bool blocks_export() const
    {
        return compatibility == TransitionCompatibility::Incompatible;
    }
};

// Resolve one transition.
//
// The most specific matching rule wins; ties on specificity go to the rule
// declared first, and the tie is reported rather than hidden. Resolution never
// consults the opposite direction.
TransitionCost resolve_transition(const TransitionMatrix   &matrix,
                                  const TransitionEndpoint &from,
                                  const TransitionEndpoint &to,
                                  double                    source_temperature_c);

// --- validation ------------------------------------------------------------

struct TransitionPolicy
{
    // When false, a transition nobody has characterized blocks export. When
    // true it is permitted, loudly: the operator has said they accept it.
    bool allow_uncharacterized { false };
};

struct TransitionValidation
{
    // Export must not proceed while this is non-empty.
    std::vector<std::string> blocking;
    std::vector<std::string> warnings;

    // Totals across the whole sequence, for waste and time estimates.
    double total_purge_mm3 { 0. };
    double total_prime_mm3 { 0. };
    int    total_brush_passes { 0 };

    // Absent when any transition in the sequence could not be priced; a
    // partial total would understate the print.
    std::optional<double> total_time_s;

    std::vector<TransitionCost> costs;

    bool may_export() const { return blocking.empty(); }
};

// Walk a schedule's material states in order and price every transition.
TransitionValidation validate_transitions(const TransitionMatrix                &matrix,
                                          const std::vector<TransitionEndpoint> &sequence,
                                          const std::vector<double>             &temperatures,
                                          const TransitionPolicy                &policy);

} // namespace Slic3r

#endif // slic3r_TransitionMatrix_hpp_
