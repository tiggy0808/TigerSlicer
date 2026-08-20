///|/ TigerSlicer Phase 7: dock, brush, purge and prime station planner.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_StationPlanner_hpp_
#define slic3r_StationPlanner_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// The hardware a toolchanger may have for parking and preparing tools. These
// are generic capabilities; nothing here assumes a particular machine.
enum class StationKind : uint8_t
{
    Dock,           // where a tool is parked and picked up
    Brush,          // wipes the nozzle
    PurgePosition,  // a place it is safe to extrude waste
    Cutter,         // trims the filament tip
    Bucket,         // catches and holds purged material
    PurgeObject,    // sacrificial geometry printed on the plate
    WipeTower,      // the classic software fallback
};

const char *station_kind_name(StationKind kind);

// A station's location. It has no default: TigerSlicer never guesses where a
// machine's dock or bucket is, because a wrong coordinate is a crash.
struct StationPosition
{
    bool   configured { false };
    double x { 0. };
    double y { 0. };
    double z { 0. };
};

struct Station
{
    size_t          id { 0 };
    StationKind     kind { StationKind::Dock };
    std::string     name;
    StationPosition position;

    // Tools this station can serve. Empty means every tool.
    std::vector<int> serves_tools;

    // How long one use takes.
    double pass_duration_s { 0. };

    // For destinations that hold waste: how much they can still take.
    // Absent means the capacity was never characterized, which is not the
    // same as unlimited.
    std::optional<double> remaining_capacity_mm3;

    bool serves(int tool) const;
};

// Everything about the transition that determines what preparation it needs.
struct PreparationContext
{
    int         from_tool { 0 };
    int         to_tool { 0 };
    std::string from_material;
    std::string to_material;

    double nozzle_diameter_mm { 0. };
    double temperature_c { 0. };
    // How long the incoming tool sat unused, and how it was left.
    double idle_s { 0. };
    bool   depressurized { false };

    // Requirements computed upstream (Phase 3 and Phase 6).
    double required_purge_volume_mm3 { 0. };
    int    required_brush_passes { 0 };
    // Set when the filament tip must be trimmed, e.g. after a long soak.
    bool requires_tip_cut { false };
};

struct PreparationStep
{
    size_t      station { 0 };
    StationKind kind { StationKind::Dock };
    std::string station_name;
    std::string action;
    double      duration_s { 0. };
    double      material_mm3 { 0. };
    std::string reason;
};

struct PreparationPlan
{
    std::vector<PreparationStep> steps;

    double total_duration_s { 0. };
    double total_material_mm3 { 0. };
    // How much of the required purge no station could absorb.
    double unabsorbed_purge_mm3 { 0. };

    // True only when the purge could not be absorbed by hardware. Hardware
    // cleaning is allowed to replace a wipe tower entirely.
    bool wipe_tower_required { false };

    // Requirements nothing on this machine can satisfy, each explained.
    std::vector<std::string> unmet;
    std::vector<std::string> notes;

    std::string error;

    bool is_satisfiable() const { return error.empty() && unmet.empty(); }
};

// Report configuration problems that would otherwise become motion.
//
// A station without a configured position is unusable, not defaultable. The
// returned strings name each one.
std::vector<std::string> validate_stations(const std::vector<Station> &stations);

// Plan the preparation for one toolchange.
//
// Purge is directed to hardware destinations first; a wipe tower is only
// required when hardware cannot absorb it. Any requirement no station can
// meet is reported rather than skipped.
PreparationPlan plan_preparation(const PreparationContext   &context,
                                 const std::vector<Station> &stations);

} // namespace Slic3r

#endif // slic3r_StationPlanner_hpp_
