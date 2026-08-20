///|/ TigerSlicer Phase 9S: multi-material support interface engine, supplementing
///|/ Phase 9's mixed-nozzle support planning.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_SupportInterface_hpp_
#define slic3r_SupportInterface_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// How a support is meant to come off the part. Phase 9 already has a
// SupportStyle for traditional versus organic geometry; this is the separate
// question of what the support is made of and how it is removed.
enum class SupportStrategy : uint8_t
{
    Soluble,
    Breakaway,
    SameMaterial,
    None,
};

const char *support_strategy_name(SupportStrategy strategy);

// How well two materials stick to each other. Unknown is a real answer here
// and is never quietly turned into "compatible".
enum class AdhesionClass : uint8_t
{
    // Sticks so little the support will not stay attached while it prints.
    TooWeak,
    // Holds during the print and still comes apart afterwards.
    Removable,
    // Welds itself to the part; it will not come off without damage.
    Bonded,
    Unknown,
};

const char *adhesion_class_name(AdhesionClass adhesion);

enum class RemovalOutcome : uint8_t
{
    Dissolves,
    PeelsAway,
    RequiresForce,
    // Physically cannot be got out. Said before the print, not after.
    NotRemovable,
    Unknown,
};

const char *removal_outcome_name(RemovalOutcome outcome);

enum class InterfaceVerdict : uint8_t
{
    Planned,
    // Printable, but not the strategy that was asked for, and it says why.
    Compromise,
    // Something needed to judge this interface is not recorded.
    Unproven,
    Refused,
};

const char *interface_verdict_name(InterfaceVerdict verdict);

// A measured adhesion between two materials. The order of the two names does
// not matter; adhesion is looked up both ways round.
struct MaterialPairing
{
    std::string a, b;

    // 0 (falls apart) to 1 (welded). Absent means this pair was never
    // measured - which is reported, not guessed at.
    std::optional<double> adhesion;

    // Whether one of the two dissolves in the other's solvent.
    bool solvent_removable { false };
};

struct SupportTool
{
    int         tool { 0 };
    std::string material;
    double      nozzle_mm { 0.4 };

    // Whether this material dissolves, and in what.
    bool        soluble { false };
    std::string solvent;

    // What it costs to change to this tool.
    double change_cost_s { 0. };

    // A tool that is defined but not currently loaded cannot be planned with.
    bool loaded { true };
};

struct SupportedRegion
{
    size_t      id { 0 };
    std::string part_material;
    int         part_tool { 0 };

    double area_mm2 { 0. };
    // 0 is a horizontal ceiling, 90 is a vertical wall.
    double overhang_angle_deg { 0. };
    double layer_height_mm { 0.2 };

    // Whether the contact surface is on show once the support is gone.
    bool visible_after_removal { false };

    // Whether the support sits inside a closed cavity. Nothing mechanical can
    // reach it there, so only a soluble support can ever come out.
    bool enclosed { false };
};

struct SupportPolicy
{
    SupportStrategy preferred { SupportStrategy::Breakaway };

    // Adhesion below this will not hold the support on during the print;
    // above the upper bound it will not come off afterwards.
    double min_adhesion_to_hold { 0.15 };
    double max_adhesion_to_remove { 0.60 };

    // A soluble interface can touch the part. Everything else needs a gap.
    double soluble_contact_distance_mm { 0.0 };
    double interface_contact_distance_mm { 0.2 };

    size_t min_interface_layers { 2 };

    // Whether an unmeasured material pair may be planned with anyway. Off by
    // default: an unknown pair stays unknown.
    bool allow_unmeasured_pairs { false };

    // Whether a non-soluble support may be put inside a closed cavity, where
    // it will be trapped.
    bool allow_trapped_support { false };

    size_t max_regions { 500 };
};

struct InterfaceDecision
{
    size_t           region { 0 };
    InterfaceVerdict verdict { InterfaceVerdict::Refused };
    SupportStrategy  strategy { SupportStrategy::None };

    std::optional<int> interface_tool;
    std::string        interface_material;

    size_t                interface_layers { 0 };
    std::optional<double> contact_distance_mm;

    AdhesionClass         adhesion { AdhesionClass::Unknown };
    std::optional<double> adhesion_value;
    RemovalOutcome        removal { RemovalOutcome::Unknown };

    // Charged only when the interface is on a different tool from the part.
    std::optional<double> change_cost_s;

    std::vector<std::string> rejected_tools;
    std::vector<std::string> reasons;
};

struct SupportInterfacePlan
{
    std::vector<InterfaceDecision> decisions;
    std::vector<std::string>       notes;

    // Absent when any region could not be planned, so a partial sum never
    // reads as the whole cost.
    std::optional<double> total_change_cost_s;

    std::string error;
};

// Look up the adhesion between two materials, in either order. Returns
// nullptr when the pair was never measured.
const MaterialPairing *find_pairing(const std::vector<MaterialPairing> &pairings,
                                    const std::string &a, const std::string &b);

// Plan the support interface for every supported region.
//
// Chooses the interface material, the number of interface layers and the
// contact distance from the adhesion between that material and the part's,
// and reports what removing the support will actually take.
//
// An unmeasured material pair is reported as unknown rather than assumed
// compatible, and a support that cannot be removed - because it is welded to
// the part, or because it is sealed inside a cavity with no solvent to reach
// it - is refused before it is printed rather than discovered afterwards.
SupportInterfacePlan plan_support_interfaces(const std::vector<SupportedRegion> &regions,
                                             const std::vector<SupportTool>     &tools,
                                             const std::vector<MaterialPairing> &pairings,
                                             const SupportPolicy                &policy);

} // namespace Slic3r

#endif // slic3r_SupportInterface_hpp_
