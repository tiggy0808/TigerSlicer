// Phase 12: preview filtering and diagnostics. This layer presents what the
// earlier phases decided; it must never re-derive or soften any of it. The
// tests concentrate on that: an empty filter means "no opinion" rather than
// "nothing matches", unproven motion stays counted separately from clear
// motion, and a single-tool comparison that cannot honestly be made is
// refused instead of estimated.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/PreviewDiagnostics.hpp"

using namespace Slic3r;

namespace {

PreviewRecord record(size_t index, int tool, MultiRateFeature feature, double print_z)
{
    PreviewRecord value;
    value.operation            = index;
    value.chronological_index  = index;
    value.tool                 = tool;
    value.display_color        = tool == 0 ? "#F6A5C1" : "#000000";
    value.feature              = feature;
    value.print_z              = print_z;
    value.layer_height_mm      = 0.2;
    value.extrusion_width_mm   = 0.45;
    value.flow_mm3_s           = 8.;
    value.tool_grid_height_mm  = 0.2;
    value.thermal_state        = ThermalState::Active;
    value.collision            = CollisionVerdict::Clear;
    value.time_s               = 10.;
    value.waste_mm3            = 0.;
    return value;
}

std::vector<PreviewRecord> schedule()
{
    std::vector<PreviewRecord> records {
        record(0, 0, MultiRateFeature::Walls, 0.2),
        record(1, 0, MultiRateFeature::SparseInfill, 0.2),
        record(2, 1, MultiRateFeature::TopSurface, 0.2),
        record(3, 0, MultiRateFeature::Walls, 0.4),
    };
    // The change onto T1 carries its reason and its waste.
    records[2].change_reason      = ToolchangeNecessity::ToolExhausted;
    records[2].change_explanation = "T0 -> T1: T0 has no remaining work";
    records[2].prep_action        = PrepAction::Purge;
    records[2].waste_mm3          = 100.;
    records[2].thermal_state      = ThermalState::Standby;
    records[2].extrusion_width_mm = 0.26;
    records[2].tool_grid_height_mm = 0.1;
    records[2].on_synchronization_plane = true;
    return records;
}

} // namespace

TEST_CASE("An empty filter selects everything, in chronological order",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();
    PreviewFilter filter;
    CHECK(filter.empty());

    const std::vector<size_t> selection = filter_preview(records, filter);
    CHECK(selection == std::vector<size_t> { 0, 1, 2, 3 });
}

TEST_CASE("Filtering by tool, colour and feature each narrow the selection",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();

    SECTION("by tool") {
        PreviewFilter filter;
        filter.tools = { 1 };
        CHECK(filter_preview(records, filter) == std::vector<size_t> { 2 });
    }

    SECTION("by display colour") {
        PreviewFilter filter;
        filter.display_colors = { "#F6A5C1" };
        CHECK(filter_preview(records, filter).size() == 3);
    }

    SECTION("by feature") {
        PreviewFilter filter;
        filter.features = { MultiRateFeature::Walls };
        CHECK(filter_preview(records, filter) == std::vector<size_t> { 0, 3 });
    }
}

TEST_CASE("Filtering by actual geometry uses the printed values",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();

    SECTION("by height") {
        PreviewFilter filter;
        filter.min_print_z = 0.3;
        CHECK(filter_preview(records, filter) == std::vector<size_t> { 3 });
    }

    SECTION("by extrusion width") {
        PreviewFilter filter;
        filter.max_extrusion_width_mm = 0.3;
        CHECK(filter_preview(records, filter) == std::vector<size_t> { 2 });
    }

    SECTION("by tool grid") {
        PreviewFilter filter;
        filter.tool_grid_height_mm = 0.1;
        CHECK(filter_preview(records, filter) == std::vector<size_t> { 2 });
    }

    SECTION("by synchronization plane") {
        PreviewFilter filter;
        filter.on_synchronization_plane = true;
        CHECK(filter_preview(records, filter) == std::vector<size_t> { 2 });
    }
}

TEST_CASE("Filtering by chronological range works on the print order",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();
    PreviewFilter filter;
    filter.from_chronological_index = 1;
    filter.to_chronological_index   = 2;
    CHECK(filter_preview(records, filter) == std::vector<size_t> { 1, 2 });
}

TEST_CASE("An operation with no toolchange never matches a change-reason filter",
          "[preview-diagnostics]")
{
    // It is not an unknown match; it simply has no reason to match on.
    const std::vector<PreviewRecord> records = schedule();
    PreviewFilter filter;
    filter.change_reasons = { ToolchangeNecessity::ToolExhausted };
    CHECK(filter_preview(records, filter) == std::vector<size_t> { 2 });

    filter.change_reasons = { ToolchangeNecessity::DependencyForced };
    CHECK(filter_preview(records, filter).empty());
}

TEST_CASE("Thermal state and prep action are filterable", "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();

    PreviewFilter standby;
    standby.thermal_states = { ThermalState::Standby };
    CHECK(filter_preview(records, standby) == std::vector<size_t> { 2 });

    PreviewFilter purge;
    purge.prep_actions = { PrepAction::Purge };
    CHECK(filter_preview(records, purge) == std::vector<size_t> { 2 });
}

TEST_CASE("Totals separate unproven motion from proven motion",
          "[preview-diagnostics]")
{
    std::vector<PreviewRecord> records = schedule();
    records[1].collision = CollisionVerdict::Unproven;
    records[3].collision = CollisionVerdict::Collision;

    const PreviewTotals totals =
        summarize_preview(records, filter_preview(records, PreviewFilter {}));

    CHECK(totals.operations == 4);
    CHECK(totals.time_s == Catch::Approx(40.));
    CHECK(totals.waste_mm3 == Catch::Approx(100.));
    CHECK(totals.toolchanges == 1);
    // "We did not check" is counted apart from "we checked and it hits".
    CHECK(totals.unproven_operations == 1);
    CHECK(totals.colliding_operations == 1);
    CHECK_FALSE(totals.fully_proven());
}

TEST_CASE("A fully clear schedule reports as proven", "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();
    const PreviewTotals totals =
        summarize_preview(records, filter_preview(records, PreviewFilter {}));
    CHECK(totals.fully_proven());
}

TEST_CASE("A single-tool comparison reports both deltas", "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();

    PreviewTotals single;
    single.operations = 4;
    single.time_s     = 55.;
    single.waste_mm3  = 0.;

    const SingleToolComparison comparison =
        compare_against_single_tool(records, 0, single, {});

    REQUIRE(comparison.status == ComparisonStatus::Comparable);
    // 40 s mixed against 55 s single-tool.
    CHECK(comparison.time_delta_s == Catch::Approx(-15.));
    CHECK(comparison.waste_delta_mm3 == Catch::Approx(100.));
    CHECK(comparison.reasons.front().find("less") != std::string::npos);
    CHECK(comparison.reasons[1].find("more") != std::string::npos);
}

TEST_CASE("A job that one tool cannot produce is refused, not estimated",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();

    PreviewTotals single;
    single.time_s = 55.;

    const SingleToolComparison comparison = compare_against_single_tool(
        records, 0, single, { "T0's 0.4 nozzle cannot resolve the 0.26 mm top surface" });

    CHECK(comparison.status == ComparisonStatus::SingleToolImpossible);
    // No number is offered, because none would be honest.
    CHECK(comparison.time_delta_s == Catch::Approx(0.));
    CHECK(comparison.reasons.front().find("cannot be produced on T0 alone") !=
          std::string::npos);
    CHECK(comparison.reasons.size() == 2);
}

TEST_CASE("A missing single-tool estimate is incomparable, not zero",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();
    const SingleToolComparison comparison =
        compare_against_single_tool(records, 0, std::nullopt, {});

    CHECK(comparison.status == ComparisonStatus::Incomparable);
    CHECK(comparison.reasons.front().find("no single-tool estimate") != std::string::npos);
}

TEST_CASE("A comparison over unproven motion says so", "[preview-diagnostics]")
{
    std::vector<PreviewRecord> records = schedule();
    records[1].collision = CollisionVerdict::Unproven;

    PreviewTotals single;
    single.time_s = 55.;

    const SingleToolComparison comparison =
        compare_against_single_tool(records, 0, single, {});
    REQUIRE(comparison.status == ComparisonStatus::Comparable);

    bool noted = false;
    for (const std::string &reason : comparison.reasons)
        if (reason.find("not proven collision-free") != std::string::npos)
            noted = true;
    CHECK(noted);
}

TEST_CASE("The schedule explanation reuses the scheduler's own words",
          "[preview-diagnostics]")
{
    const std::vector<PreviewRecord> records = schedule();

    OptimizationResult optimization;
    optimization.improved = true;
    optimization.baseline.toolchanges.resize(3);
    optimization.optimized.toolchanges.resize(2);
    optimization.eliminated.push_back("T0 -> T1: 1 change(s) removed");
    optimization.introduced.push_back("T0 -> T2: 1 change(s) introduced");
    optimization.notes.push_back("lookahead was limited");

    const std::vector<std::string> lines = explain_schedule(records, optimization);

    CHECK(lines.front().find("reduced 3 toolchange(s) to 2") != std::string::npos);
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find("removed: T0 -> T1") != std::string::npos;
    }));
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find("introduced: T0 -> T2") != std::string::npos;
    }));
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find("note: lookahead was limited") != std::string::npos;
    }));
    // The per-change explanation is the one Phase 4 wrote, verbatim.
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find("T0 has no remaining work") != std::string::npos;
    }));
}

TEST_CASE("An unimproved schedule says the baseline is what is printed",
          "[preview-diagnostics]")
{
    OptimizationResult optimization;
    optimization.improved = false;

    const std::vector<std::string> lines = explain_schedule({}, optimization);
    CHECK(lines.front().find("nothing better than the baseline") != std::string::npos);
}

TEST_CASE("A failed optimization is reported rather than explained away",
          "[preview-diagnostics]")
{
    OptimizationResult optimization;
    optimization.error = "the dependency graph cannot be ordered without moving down in Z";

    const std::vector<std::string> lines = explain_schedule({}, optimization);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().find("could not be optimized") != std::string::npos);
    CHECK(lines.front().find("moving down in Z") != std::string::npos);
}
