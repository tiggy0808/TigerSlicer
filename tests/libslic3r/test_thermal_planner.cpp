// Phase 6: predictive heating and ooze control. The tests concentrate on the
// three things that make a thermal plan trustworthy: it heats just in time
// rather than early, it never emits a command the firmware did not declare,
// and an unavoidable wait or an uncharacterized material limit is reported
// instead of absorbed.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/ThermalPlanner.hpp"

using namespace Slic3r;

namespace {

ToolThermalState state(int tool, double current = 25.)
{
    ToolThermalState value;
    value.tool                  = tool;
    value.active_temperature_c  = 215.;
    value.standby_temperature_c = 175.;
    value.idle_temperature_c    = 25.;
    value.heat_rate_c_s         = 2.;    // 25 -> 215 is 95 s; 175 -> 215 is 20 s
    value.cool_rate_c_s         = 1.;    // 215 -> 175 is 40 s; 215 -> 25 is 190 s
    value.current_temperature_c = current;
    return value;
}

MaterialThermalLimits pla()
{
    MaterialThermalLimits limits;
    limits.material     = "PLA";
    limits.ooze_onset_c = 170.;
    limits.min_active_c = 190.;
    limits.max_active_c = 230.;
    return limits;
}

FirmwareThermalCapabilities full_firmware()
{
    FirmwareThermalCapabilities firmware;
    firmware.can_set_tool_temperature    = true;
    firmware.can_set_standby_temperature = true;
    firmware.can_wait_for_temperature    = true;
    firmware.set_temperature_gcode       = "M104 T[tool] S[temperature]";
    firmware.set_standby_gcode           = "M568 P[tool] R[temperature]";
    firmware.wait_temperature_gcode      = "M109 T[tool] S[temperature]";
    return firmware;
}

const ThermalAction *find(const ThermalPlan &plan, int tool, ThermalActionKind kind)
{
    for (const ThermalAction &action : plan.actions)
        if (action.tool == tool && action.kind == kind)
            return &action;
    return nullptr;
}

size_t count(const ThermalPlan &plan, int tool, ThermalActionKind kind)
{
    return size_t(std::count_if(plan.actions.begin(), plan.actions.end(),
                                [&](const ThermalAction &action) {
                                    return action.tool == tool && action.kind == kind;
                                }));
}

} // namespace

TEST_CASE("A tool is preheated exactly early enough, not from the start",
          "[thermal-planner]")
{
    // Needed at 300 s, 95 s of heating: start at 205 s.
    const std::vector<ToolUsageWindow> windows { { 0, 300., 400. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0) }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    const ThermalAction *preheat = find(plan, 0, ThermalActionKind::Preheat);
    REQUIRE(preheat != nullptr);
    CHECK(preheat->at_s == Catch::Approx(205.));
    CHECK(preheat->target_c == Catch::Approx(215.));
    CHECK(plan.total_heating_wait_s == Catch::Approx(0.));
    // Nothing waits, because the heat fits.
    CHECK(count(plan, 0, ThermalActionKind::WaitForTemperature) == 0);
}

TEST_CASE("A heat that does not fit is reported as a wait, not absorbed",
          "[thermal-planner]")
{
    // Needed at 30 s but the heat takes 95 s.
    const std::vector<ToolUsageWindow> windows { { 0, 30., 100. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0) }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    const ThermalAction *preheat = find(plan, 0, ThermalActionKind::Preheat);
    REQUIRE(preheat != nullptr);
    CHECK(preheat->at_s == Catch::Approx(0.));   // as early as possible, still not enough
    CHECK(plan.total_heating_wait_s == Catch::Approx(65.));

    const ThermalAction *wait = find(plan, 0, ThermalActionKind::WaitForTemperature);
    REQUIRE(wait != nullptr);
    CHECK(wait->at_s == Catch::Approx(30.));
    REQUIRE_FALSE(plan.notes.empty());
    CHECK(plan.notes.front().severity == ThermalSeverity::Warning);
    CHECK(plan.notes.front().message.find("the print waits") != std::string::npos);
}

TEST_CASE("A long gap drops the tool to idle and brings it back in time",
          "[thermal-planner]")
{
    // Gap of 500 s: 190 s down plus 95 s back up fits comfortably.
    const std::vector<ToolUsageWindow> windows { { 0, 200., 300. }, { 0, 800., 900. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0, 215.) }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    const ThermalAction *down = find(plan, 0, ThermalActionKind::DropToIdle);
    REQUIRE(down != nullptr);
    CHECK(down->at_s == Catch::Approx(300.));
    CHECK(down->target_c == Catch::Approx(25.));

    // The return is scheduled 95 s before the next use, not immediately.
    bool found_return = false;
    for (const ThermalAction &action : plan.actions)
        if (action.kind == ThermalActionKind::Preheat && action.at_s > 300.) {
            CHECK(action.at_s == Catch::Approx(705.));
            found_return = true;
        }
    CHECK(found_return);
    CHECK(plan.total_hot_idle_s == Catch::Approx(0.));
}

TEST_CASE("A medium gap uses standby rather than a full cool-down",
          "[thermal-planner]")
{
    // Gap of 100 s: too short for 190 + 95 idle, long enough for 40 + 20.
    const std::vector<ToolUsageWindow> windows { { 0, 200., 300. }, { 0, 400., 500. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0, 215.) }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    const ThermalAction *standby = find(plan, 0, ThermalActionKind::HoldStandby);
    REQUIRE(standby != nullptr);
    CHECK(standby->at_s == Catch::Approx(300.));
    CHECK(standby->target_c == Catch::Approx(175.));
    CHECK(standby->gcode == "M568 P0 R175");
    // No full cool-down was scheduled during the gap.
    CHECK(count(plan, 0, ThermalActionKind::DropToIdle) == 1);   // only the post-print one
}

TEST_CASE("A gap too short to cool is counted as hot idle, not ignored",
          "[thermal-planner]")
{
    // Gap of 30 s: cooling to standby and back needs 60 s.
    const std::vector<ToolUsageWindow> windows { { 0, 200., 300. }, { 0, 330., 400. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0, 215.) }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    CHECK(plan.total_hot_idle_s == Catch::Approx(30.));
    CHECK(count(plan, 0, ThermalActionKind::HoldStandby) == 0);
    // Sitting above the ooze threshold is stated even though nothing can be
    // done about a gap this short.
    bool mentioned = false;
    for (const ThermalNote &note : plan.notes)
        if (note.message.find("ooze threshold") != std::string::npos)
            mentioned = true;
    CHECK(mentioned);
}

TEST_CASE("No thermal command is invented when the firmware did not declare it",
          "[thermal-planner]")
{
    const std::vector<ToolUsageWindow> windows { { 0, 200., 300. }, { 0, 400., 500. } };

    FirmwareThermalCapabilities partial;
    partial.can_set_tool_temperature = true;
    partial.set_temperature_gcode    = "M104 T[tool] S[temperature]";
    // Standby is not declared at all.

    const ThermalPlan plan = plan_thermals(windows, { state(0, 215.) }, { pla() }, partial);
    REQUIRE(plan.error.empty());

    const ThermalAction *standby = find(plan, 0, ThermalActionKind::HoldStandby);
    REQUIRE(standby != nullptr);
    // The plan still says what it would do, but emits nothing.
    CHECK_FALSE(standby->emitted);
    CHECK(standby->gcode.empty());
    CHECK_FALSE(standby->reason.empty());

    REQUIRE_FALSE(plan.limitations.empty());
    CHECK(plan.limitations.front().find("standby") != std::string::npos);
}

TEST_CASE("A declared capability with no configured hook still emits nothing",
          "[thermal-planner]")
{
    FirmwareThermalCapabilities claimed;
    claimed.can_set_tool_temperature = true;   // declared, but no hook configured

    const std::vector<ToolUsageWindow> windows { { 0, 300., 400. } };
    const ThermalPlan plan = plan_thermals(windows, { state(0) }, { pla() }, claimed);

    REQUIRE(plan.error.empty());
    const ThermalAction *preheat = find(plan, 0, ThermalActionKind::Preheat);
    REQUIRE(preheat != nullptr);
    CHECK_FALSE(preheat->emitted);
    REQUIRE_FALSE(plan.limitations.empty());
    CHECK(plan.limitations.front().find("hook is not configured") != std::string::npos);
}

TEST_CASE("Hooks are substituted, never assembled from assumptions",
          "[thermal-planner]")
{
    FirmwareThermalCapabilities firmware = full_firmware();
    firmware.set_temperature_gcode = "SET_HEATER TOOL=[tool] TARGET=[temperature]";

    const std::vector<ToolUsageWindow> windows { { 1, 300., 400. } };
    std::vector<ToolThermalState> tools { state(0), state(1) };
    const ThermalPlan plan = plan_thermals(windows, tools, { pla(), pla() }, firmware);

    REQUIRE(plan.error.empty());
    const ThermalAction *preheat = find(plan, 1, ThermalActionKind::Preheat);
    REQUIRE(preheat != nullptr);
    CHECK(preheat->gcode == "SET_HEATER TOOL=1 TARGET=215");
    CHECK(preheat->emitted);
}

TEST_CASE("A target the material cannot survive is refused, not clamped",
          "[thermal-planner]")
{
    ToolThermalState hot = state(0);
    hot.active_temperature_c = 260.;

    const std::vector<ToolUsageWindow> windows { { 0, 300., 400. } };
    const ThermalPlan plan = plan_thermals(windows, { hot }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    CHECK(plan.has_blocking_note());
    bool named = false;
    for (const ThermalNote &note : plan.notes)
        if (note.severity == ThermalSeverity::Blocked &&
            note.message.find("rated to") != std::string::npos)
            named = true;
    CHECK(named);
}

TEST_CASE("An uncharacterized hot-idle limit is not read as no limit",
          "[thermal-planner]")
{
    MaterialThermalLimits unknown;
    unknown.material     = "unknown filament";
    unknown.ooze_onset_c = 170.;
    // max_hot_idle_s deliberately absent.

    const std::vector<ToolUsageWindow> windows { { 0, 200., 300. }, { 0, 330., 400. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0, 215.) }, { unknown }, full_firmware());

    REQUIRE(plan.error.empty());
    bool stated = false;
    for (const ThermalNote &note : plan.notes)
        if (note.message.find("no hot-idle limit is characterized") != std::string::npos)
            stated = true;
    CHECK(stated);
}

TEST_CASE("Exceeding a characterized hot-idle limit warns", "[thermal-planner]")
{
    MaterialThermalLimits petg;
    petg.material       = "PETG";
    petg.ooze_onset_c   = 180.;
    petg.max_hot_idle_s = 20.;

    const std::vector<ToolUsageWindow> windows { { 0, 200., 300. }, { 0, 330., 400. } };
    const ThermalPlan plan =
        plan_thermals(windows, { state(0, 215.) }, { petg }, full_firmware());

    REQUIRE(plan.error.empty());
    bool warned = false;
    for (const ThermalNote &note : plan.notes)
        if (note.severity == ThermalSeverity::Warning &&
            note.message.find("beyond PETG's") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("A tool with no heating rate blocks rather than guessing",
          "[thermal-planner]")
{
    ToolThermalState broken = state(0);
    broken.heat_rate_c_s = 0.;

    const std::vector<ToolUsageWindow> windows { { 0, 300., 400. } };
    const ThermalPlan plan = plan_thermals(windows, { broken }, { pla() }, full_firmware());

    REQUIRE(plan.error.empty());
    CHECK(plan.has_blocking_note());
    CHECK(plan.actions.empty());
}

TEST_CASE("An empty or malformed schedule is refused", "[thermal-planner]")
{
    SECTION("nothing to plan") {
        const ThermalPlan plan = plan_thermals({}, { state(0) }, { pla() }, full_firmware());
        CHECK(plan.error.find("no tool usage windows") != std::string::npos);
    }

    SECTION("a window that ends before it starts") {
        const ThermalPlan plan =
            plan_thermals({ { 0, 400., 300. } }, { state(0) }, { pla() }, full_firmware());
        CHECK(plan.error.find("ends before it starts") != std::string::npos);
    }
}

TEST_CASE("Prime volume grows with how long and how hot a tool oozed",
          "[thermal-planner]")
{
    const PrimeRequirement brief =
        estimate_prime_requirement(pla(), 10., 215., false, 60., 4);
    const PrimeRequirement lengthy =
        estimate_prime_requirement(pla(), 600., 215., false, 60., 4);

    CHECK(brief.purge_volume_mm3 > 60.);
    CHECK(lengthy.purge_volume_mm3 > brief.purge_volume_mm3);
    CHECK(lengthy.brush_passes > 4);
    CHECK_FALSE(lengthy.reasons.empty());
}

TEST_CASE("A tool kept below the ooze threshold needs no extra purge",
          "[thermal-planner]")
{
    const PrimeRequirement cool =
        estimate_prime_requirement(pla(), 600., 150., false, 60., 4);
    CHECK(cool.purge_volume_mm3 == Catch::Approx(60.));
    CHECK(cool.brush_passes == 4);
}

TEST_CASE("Without an ooze threshold no allowance is invented", "[thermal-planner]")
{
    MaterialThermalLimits unknown;
    unknown.material = "unknown filament";

    const PrimeRequirement requirement =
        estimate_prime_requirement(unknown, 600., 215., false, 60., 4);
    CHECK(requirement.purge_volume_mm3 == Catch::Approx(60.));
    bool stated = false;
    for (const std::string &reason : requirement.reasons)
        if (reason.find("no ooze threshold is characterized") != std::string::npos)
            stated = true;
    CHECK(stated);
}

TEST_CASE("Released pressure has to be rebuilt and says so", "[thermal-planner]")
{
    const PrimeRequirement requirement =
        estimate_prime_requirement(pla(), 0., 215., true, 60., 4);
    CHECK(requirement.needs_pressure_recovery);
    bool stated = false;
    for (const std::string &reason : requirement.reasons)
        if (reason.find("pressure was released") != std::string::npos)
            stated = true;
    CHECK(stated);
}
