// Phase 2: toolchanger preflight. This is the last gate before export, so the
// tests focus on genuinely unsafe combinations being errors rather than
// warnings, on findings naming what is responsible, and on the custom-G-code
// feedrate scan not being fooled by comments or placeholders.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/ToolchangerPreflight.hpp"

using namespace Slic3r;

namespace {

ToolPreflightState tool(int index, double nozzle = 0.4, double layer = 0.2)
{
    ToolPreflightState state;
    state.tool                   = index;
    state.nozzle_diameter_mm     = nozzle;
    state.min_layer_height_mm    = 0.05;
    state.max_layer_height_mm    = nozzle * 0.75;
    state.actual_layer_height_mm = layer;
    state.extrusion_width_mm     = nozzle;
    state.volumetric_limit_mm3_s = 12.;
    state.temperature_c          = 215.;
    state.filament_name          = "PLA";
    state.filament_selected      = true;
    state.offset_declared        = true;
    state.capabilities           = { "toolchange", "park" };
    return state;
}

PreflightInputs inputs()
{
    PreflightInputs value;
    value.tools      = { tool(0), tool(1, 0.2, 0.1) };
    value.used_tools = { 0, 1 };
    return value;
}

bool has_code(const PreflightReport &report, const std::string &code)
{
    return std::any_of(report.findings.begin(), report.findings.end(),
        [&code](const PreflightFinding &finding) { return finding.code == code; });
}

} // namespace

TEST_CASE("A well-formed toolchanger setup passes", "[preflight]")
{
    const PreflightReport report = run_toolchanger_preflight(inputs());
    CHECK_FALSE(report.blocked());
    CHECK(report.errors().empty());
}

TEST_CASE("Using an undefined tool blocks export", "[preflight]")
{
    PreflightInputs value = inputs();
    value.used_tools.push_back(3);

    const PreflightReport report = run_toolchanger_preflight(value);
    CHECK(report.blocked());
    CHECK(has_code(report, "tool_missing"));
}

TEST_CASE("Layer heights outside a tool's range block", "[preflight]")
{
    PreflightInputs too_thin = inputs();
    too_thin.tools[0].actual_layer_height_mm = 0.01;
    CHECK(has_code(run_toolchanger_preflight(too_thin), "layer_height_below_min"));

    PreflightInputs too_thick = inputs();
    too_thick.tools[0].actual_layer_height_mm = 0.9;
    CHECK(run_toolchanger_preflight(too_thick).blocked());
}

TEST_CASE("A layer taller than the nozzle blocks", "[preflight]")
{
    PreflightInputs value = inputs();
    // Raise the configured maximum so only the nozzle rule can catch this.
    value.tools[0].max_layer_height_mm    = 5.;
    value.tools[0].actual_layer_height_mm = 0.6;   // nozzle is 0.4

    const PreflightReport report = run_toolchanger_preflight(value);
    CHECK(has_code(report, "layer_height_exceeds_nozzle"));
    CHECK(report.blocked());
}

TEST_CASE("Missing filament, temperature, or offset blocks", "[preflight]")
{
    PreflightInputs no_filament = inputs();
    no_filament.tools[1].filament_selected = false;
    CHECK(has_code(run_toolchanger_preflight(no_filament), "filament_unselected"));

    PreflightInputs no_temp = inputs();
    no_temp.tools[1].temperature_c = 0.;
    CHECK(has_code(run_toolchanger_preflight(no_temp), "temperature_invalid"));

    PreflightInputs no_offset = inputs();
    no_offset.tools[1].offset_declared = false;
    CHECK(has_code(run_toolchanger_preflight(no_offset), "offset_undeclared"));
}

TEST_CASE("An unset flow limit warns but does not block", "[preflight]")
{
    PreflightInputs value = inputs();
    value.tools[0].volumetric_limit_mm3_s = 0.;

    const PreflightReport report = run_toolchanger_preflight(value);
    CHECK(has_code(report, "flow_limit_unset"));
    CHECK_FALSE(report.blocked());
}

TEST_CASE("Detail finer than the owning nozzle warns", "[preflight]")
{
    PreflightInputs value = inputs();
    value.smallest_detail_mm = { { 0, 0.15 } };   // T0 nozzle is 0.4

    const PreflightReport report = run_toolchanger_preflight(value);
    CHECK(has_code(report, "detail_not_representable"));
    // It is a quality problem, not an unsafe one, so it must stay a warning.
    CHECK_FALSE(report.blocked());
}

TEST_CASE("An F0 feedrate in custom G-code blocks export", "[preflight]")
{
    PreflightInputs value = inputs();
    value.macros.push_back({ "toolchange_gcode", "G1 X10 Y10 F0\nG1 Z5 F1200\n", {} });

    const PreflightReport report = run_toolchanger_preflight(value);
    CHECK(report.blocked());
    CHECK(has_code(report, "invalid_feedrate"));
}

TEST_CASE("The feedrate scan is not fooled by comments or placeholders", "[preflight]")
{
    // Commented-out and parenthesised feedrates are not emitted.
    CHECK(find_invalid_feedrates("; G1 X1 F0\nG1 X1 F600\n").empty());
    CHECK(find_invalid_feedrates("G1 X1 (F0) F600\n").empty());
    // A placeholder is resolved later; it is not a literal zero.
    CHECK(find_invalid_feedrates("G1 X1 F{travel_speed}\n").empty());
    // An F that is part of another word must not be read as a feedrate.
    CHECK(find_invalid_feedrates("SET_PRESSURE_ADVANCE ADVANCE=0\n").empty());

    // Real problems are still caught.
    CHECK(find_invalid_feedrates("G1 X1 F0\n").size() == 1);
    CHECK(find_invalid_feedrates("G1 X1 F-5\n").size() == 1);
}

TEST_CASE("A macro requiring an undeclared capability blocks", "[preflight]")
{
    PreflightInputs value = inputs();
    value.macros.push_back({ "toolchange_gcode", "G1 X10 F600\n", { "probe" } });

    const PreflightReport report = run_toolchanger_preflight(value);
    CHECK(report.blocked());
    CHECK(has_code(report, "capability_undeclared"));

    // A capability the tools do declare is accepted.
    PreflightInputs declared = inputs();
    declared.macros.push_back({ "toolchange_gcode", "G1 X10 F600\n", { "park" } });
    CHECK_FALSE(run_toolchanger_preflight(declared).blocked());
}

TEST_CASE("Findings name the responsible tool and setting", "[preflight]")
{
    PreflightInputs value = inputs();
    value.tools[1].temperature_c = 0.;

    const PreflightReport report = run_toolchanger_preflight(value);
    const auto it = std::find_if(report.findings.begin(), report.findings.end(),
        [](const PreflightFinding &finding) { return finding.code == "temperature_invalid"; });
    REQUIRE(it != report.findings.end());
    CHECK(it->tool.value_or(-1) == 1);
    CHECK_FALSE(it->setting.empty());
}

TEST_CASE("The report renders as exportable text", "[preflight]")
{
    PreflightInputs value = inputs();
    value.tools[0].temperature_c = 0.;

    const std::string text = run_toolchanger_preflight(value).to_text();
    CHECK(text.find("ERROR") != std::string::npos);
    CHECK(text.find("temperature_invalid") != std::string::npos);

    CHECK(run_toolchanger_preflight(inputs()).to_text().find("No findings") != std::string::npos);
}
