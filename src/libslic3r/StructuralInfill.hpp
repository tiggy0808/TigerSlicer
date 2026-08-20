///|/ TigerSlicer Phase 20: heuristic structural infill planner.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_StructuralInfill_hpp_
#define slic3r_StructuralInfill_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// This planner is a set of rules of thumb. It is NOT a structural analysis and
// nothing it produces is certified for load-bearing use. The label is attached
// to every proposal so it cannot be separated from the numbers - this is the
// phase most likely to be mistaken for FEA.
const char *structural_disclaimer();

struct LoadDirection
{
    std::string name;
    // Unit-ish direction; it is normalized on use.
    double x { 0. }, y { 0. }, z { 0. };
    // Absent means the user described a direction but not a magnitude, which
    // caps what can be said about it.
    std::optional<double> magnitude_n;
};

struct MountingSurface
{
    size_t id { 0 };
    // Outward normal of the face that is bolted down or seated.
    double nx { 0. }, ny { 0. }, nz { 1. };
    bool fixed { true };
};

struct CriticalHole
{
    size_t id { 0 };
    double diameter_mm { 0. };
    // How much material the user wants left around it. Absent means unstated.
    std::optional<double> required_wall_mm;
};

enum class StructuralPriority : uint8_t
{
    Stiffness,
    Weight,
    Balanced,
};

const char *structural_priority_name(StructuralPriority priority);

enum class SafetyPreference : uint8_t
{
    // Round every judgement towards more material.
    Conservative,
    Nominal,
};

const char *safety_preference_name(SafetyPreference preference);

// What the profile currently says.
struct StructuralSettings
{
    double infill_density { 0.15 };   // 0 to 1
    double infill_angle_deg { 45. };
    int    wall_count { 2 };
    int    solid_top_layers { 4 };
    int    solid_bottom_layers { 4 };
};

struct StructuralTool
{
    int         tool { 0 };
    std::string material;
    // Relative stiffness of this material, 1.0 nominal. Absent means unknown,
    // which disqualifies it from being proposed as reinforcement.
    std::optional<double> stiffness_factor;
    double throughput_mm3_s { 10. };
};

struct StructuralOptions
{
    StructuralPriority priority { StructuralPriority::Balanced };
    SafetyPreference   safety { SafetyPreference::Conservative };

    // A weight-first request may legitimately want less material. Without
    // this, a proposal that reduces strength is withheld and reported instead
    // of being made quietly.
    bool allow_strength_reduction { false };

    // For costing. Absent leaves the proposal unpriced rather than free.
    std::optional<double> material_cost_per_mm3;
    // The part's approximate solid volume, used to turn a density change into
    // a material figure.
    std::optional<double> part_volume_mm3;
};

enum class StructuralChangeKind : uint8_t
{
    InfillDensity,
    InfillDirection,
    WallCount,
    SolidLayers,
    Rib,
    Reinforcement,
};

const char *structural_change_kind_name(StructuralChangeKind kind);

struct StructuralChange
{
    StructuralChangeKind kind { StructuralChangeKind::InfillDensity };

    double current_value { 0. };
    double proposed_value { 0. };

    // Set when the change would leave the part weaker than it is now.
    bool reduces_strength { false };

    // Which tool would do this work, where the change implies one.
    std::optional<int> tool;

    // Always non-empty, and always begins with the heuristic disclaimer.
    std::vector<std::string> reasons;
};

struct StructuralPlan
{
    std::vector<StructuralChange> changes;

    // Changes that were NOT made because they would reduce strength and
    // reduction was not permitted, each with its reason.
    std::vector<std::string> withheld;
    std::vector<std::string> notes;

    // Cost of everything proposed. Absent when the inputs to price it were not
    // supplied; an unpriced proposal is not a free one.
    std::optional<double> material_delta_mm3;
    std::optional<double> time_delta_s;

    std::string error;
};

// Propose a structural infill plan.
//
// Nothing is applied. No strength setting is ever reduced silently: such a
// change is withheld and reported unless the caller explicitly permitted
// reductions. Every proposal carries the heuristic disclaimer, and the time
// and material effect is reported or explicitly absent.
StructuralPlan plan_structural_infill(const std::vector<LoadDirection>   &loads,
                                      const std::vector<MountingSurface> &mounts,
                                      const std::vector<CriticalHole>    &holes,
                                      const StructuralSettings           &current,
                                      const std::vector<StructuralTool>  &tools,
                                      const StructuralOptions            &options);

} // namespace Slic3r

#endif // slic3r_StructuralInfill_hpp_
