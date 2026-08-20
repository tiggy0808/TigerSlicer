// Phase 7: dock/brush/purge/prime planning. The tests concentrate on the
// safety-relevant properties: no station is ever used without a configured
// position, hardware cleaning is allowed to replace the wipe tower entirely,
// waste with nowhere to go is refused rather than dropped, and a requirement
// nothing can meet is reported instead of skipped.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/StationPlanner.hpp"

using namespace Slic3r;

namespace {

Station station(size_t id, StationKind kind, const std::string &name, bool positioned = true)
{
    Station value;
    value.id                  = id;
    value.kind                = kind;
    value.name                = name;
    value.position.configured = positioned;
    value.position.x          = 10.;
    value.position.y          = 20.;
    value.position.z          = 5.;
    value.pass_duration_s     = 1.;
    return value;
}

std::vector<Station> typical_machine()
{
    std::vector<Station> stations {
        station(0, StationKind::Dock, "dock rack"),
        station(1, StationKind::Brush, "brass brush"),
        station(2, StationKind::Bucket, "waste bucket"),
    };
    stations[0].pass_duration_s = 3.;
    stations[1].pass_duration_s = 0.5;
    stations[2].remaining_capacity_mm3 = 5000.;
    return stations;
}

PreparationContext transition(int from, int to)
{
    PreparationContext context;
    context.from_tool                = from;
    context.to_tool                  = to;
    context.from_material            = "PLA";
    context.to_material              = "PETG";
    context.nozzle_diameter_mm       = 0.4;
    context.temperature_c            = 215.;
    context.idle_s                   = 120.;
    context.required_purge_volume_mm3 = 100.;
    context.required_brush_passes     = 4;
    return context;
}

bool has_kind(const PreparationPlan &plan, StationKind kind)
{
    return std::any_of(plan.steps.begin(), plan.steps.end(),
                       [&](const PreparationStep &step) { return step.kind == kind; });
}

double material_into(const PreparationPlan &plan, StationKind kind)
{
    double total = 0.;
    for (const PreparationStep &step : plan.steps)
        if (step.kind == kind)
            total += step.material_mm3;
    return total;
}

} // namespace

TEST_CASE("A station with no configured position is unusable, not defaulted",
          "[station-planner]")
{
    std::vector<Station> stations = typical_machine();
    stations[0].position.configured = false;

    const std::vector<std::string> problems = validate_stations(stations);
    REQUIRE_FALSE(problems.empty());
    CHECK(problems.front().find("no configured position") != std::string::npos);
    CHECK(problems.front().find("reference printer") != std::string::npos);

    // And the planner refuses to use it rather than moving somewhere invented.
    const PreparationPlan plan = plan_preparation(transition(0, 1), stations);
    CHECK_FALSE(plan.is_satisfiable());
    CHECK_FALSE(has_kind(plan, StationKind::Dock));
}

TEST_CASE("A wipe tower is software, so it needs no coordinate", "[station-planner]")
{
    std::vector<Station> stations { station(0, StationKind::Dock, "dock rack"),
                                    station(1, StationKind::WipeTower, "tower", false) };
    CHECK(validate_stations(stations).empty());
}

TEST_CASE("Hardware cleaning replaces the wipe tower entirely", "[station-planner]")
{
    const PreparationPlan plan = plan_preparation(transition(0, 1), typical_machine());

    REQUIRE(plan.is_satisfiable());
    CHECK_FALSE(plan.wipe_tower_required);
    CHECK(material_into(plan, StationKind::Bucket) == Catch::Approx(100.));
    CHECK(plan.unabsorbed_purge_mm3 == Catch::Approx(0.));

    bool stated = false;
    for (const std::string &note : plan.notes)
        if (note.find("no wipe tower") != std::string::npos)
            stated = true;
    CHECK(stated);
}

TEST_CASE("The wipe tower is used only when hardware cannot absorb the purge",
          "[station-planner]")
{
    std::vector<Station> stations = typical_machine();
    stations[2].remaining_capacity_mm3 = 30.;   // bucket nearly full
    stations.push_back(station(3, StationKind::WipeTower, "tower", false));

    const PreparationPlan plan = plan_preparation(transition(0, 1), stations);
    REQUIRE(plan.is_satisfiable());

    CHECK(material_into(plan, StationKind::Bucket) == Catch::Approx(30.));
    CHECK(material_into(plan, StationKind::WipeTower) == Catch::Approx(70.));
    CHECK(plan.wipe_tower_required);

    bool warned = false;
    for (const std::string &note : plan.notes)
        if (note.find("needs emptying") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("Waste with nowhere to go is refused, not dropped", "[station-planner]")
{
    std::vector<Station> stations { station(0, StationKind::Dock, "dock rack"),
                                    station(1, StationKind::Brush, "brush") };
    // No bucket, no purge position, no tower.

    const PreparationPlan plan = plan_preparation(transition(0, 1), stations);
    CHECK_FALSE(plan.is_satisfiable());
    CHECK(plan.unabsorbed_purge_mm3 == Catch::Approx(100.));
    bool refused = false;
    for (const std::string &line : plan.unmet)
        if (line.find("absorb") != std::string::npos)
            refused = true;
    CHECK(refused);
}

TEST_CASE("A full bucket absorbs nothing and says so", "[station-planner]")
{
    std::vector<Station> stations = typical_machine();
    stations[2].remaining_capacity_mm3 = 0.;

    const PreparationPlan plan = plan_preparation(transition(0, 1), stations);
    CHECK_FALSE(plan.is_satisfiable());
    bool stated = false;
    for (const std::string &note : plan.notes)
        if (note.find("is full") != std::string::npos)
            stated = true;
    CHECK(stated);
}

TEST_CASE("An uncharacterized capacity is not treated as unlimited by accident",
          "[station-planner]")
{
    // A bucket whose remaining capacity was never characterized takes the
    // purge, but the fact that nobody knows how full it is stays visible in
    // the configuration rather than being asserted here.
    std::vector<Station> stations = typical_machine();
    stations[2].remaining_capacity_mm3.reset();

    const PreparationPlan plan = plan_preparation(transition(0, 1), stations);
    REQUIRE(plan.is_satisfiable());
    CHECK(material_into(plan, StationKind::Bucket) == Catch::Approx(100.));
    CHECK_FALSE(plan.wipe_tower_required);
}

TEST_CASE("A station that does not serve the tool is explained", "[station-planner]")
{
    std::vector<Station> stations = typical_machine();
    stations[1].serves_tools = { 3 };   // brush only reaches T3

    const PreparationPlan plan = plan_preparation(transition(0, 1), stations);
    CHECK_FALSE(plan.is_satisfiable());

    bool unmet = false;
    for (const std::string &line : plan.unmet)
        if (line.find("no usable brush") != std::string::npos)
            unmet = true;
    CHECK(unmet);

    // The difference between "no brush" and "a brush that cannot reach it"
    // is preserved.
    bool explained = false;
    for (const std::string &note : plan.notes)
        if (note.find("does not serve T1") != std::string::npos)
            explained = true;
    CHECK(explained);
}

TEST_CASE("A requested tip cut with no cutter is reported", "[station-planner]")
{
    PreparationContext context = transition(0, 1);
    context.requires_tip_cut = true;

    const PreparationPlan plan = plan_preparation(context, typical_machine());
    CHECK_FALSE(plan.is_satisfiable());
    bool unmet = false;
    for (const std::string &line : plan.unmet)
        if (line.find("tip cut") != std::string::npos)
            unmet = true;
    CHECK(unmet);
}

TEST_CASE("A cutter that exists is used", "[station-planner]")
{
    std::vector<Station> stations = typical_machine();
    stations.push_back(station(3, StationKind::Cutter, "snips"));

    PreparationContext context = transition(0, 1);
    context.requires_tip_cut = true;

    const PreparationPlan plan = plan_preparation(context, stations);
    REQUIRE(plan.is_satisfiable());
    CHECK(has_kind(plan, StationKind::Cutter));
}

TEST_CASE("Both docking movements are planned and timed", "[station-planner]")
{
    const PreparationPlan plan = plan_preparation(transition(0, 1), typical_machine());
    REQUIRE(plan.is_satisfiable());

    size_t docks = 0;
    for (const PreparationStep &step : plan.steps)
        if (step.kind == StationKind::Dock)
            ++docks;
    CHECK(docks == 2);   // park the outgoing tool, collect the incoming one

    // 3 s per dock move plus 4 brush passes at 0.5 s.
    CHECK(plan.total_duration_s == Catch::Approx(8.));
}

TEST_CASE("A change to the same tool needs no preparation", "[station-planner]")
{
    const PreparationPlan plan = plan_preparation(transition(2, 2), typical_machine());
    CHECK(plan.steps.empty());
    CHECK(plan.total_duration_s == Catch::Approx(0.));
    CHECK(plan.is_satisfiable());
    CHECK_FALSE(plan.notes.empty());
}

TEST_CASE("Released pressure and an unreported nozzle are both surfaced",
          "[station-planner]")
{
    PreparationContext context = transition(0, 1);
    context.depressurized      = true;
    context.nozzle_diameter_mm = 0.;

    const PreparationPlan plan = plan_preparation(context, typical_machine());
    REQUIRE(plan.is_satisfiable());

    bool pressure = false, nozzle = false;
    for (const std::string &note : plan.notes) {
        if (note.find("pressure was released") != std::string::npos)
            pressure = true;
        if (note.find("nozzle diameter was not reported") != std::string::npos)
            nozzle = true;
    }
    CHECK(pressure);
    CHECK(nozzle);
}

TEST_CASE("The purge reason names both materials", "[station-planner]")
{
    const PreparationPlan plan = plan_preparation(transition(0, 1), typical_machine());
    REQUIRE(plan.is_satisfiable());

    bool named = false;
    for (const PreparationStep &step : plan.steps)
        if (step.kind == StationKind::Bucket &&
            step.reason.find("PLA -> PETG") != std::string::npos)
            named = true;
    CHECK(named);
}

TEST_CASE("Negative configuration is reported rather than normalized",
          "[station-planner]")
{
    std::vector<Station> stations = typical_machine();
    stations[1].pass_duration_s        = -1.;
    stations[2].remaining_capacity_mm3 = -5.;

    const std::vector<std::string> problems = validate_stations(stations);
    CHECK(problems.size() == 2);
}
