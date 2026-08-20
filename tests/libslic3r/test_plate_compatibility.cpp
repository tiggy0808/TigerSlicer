// Phase N: live plate-to-printer compatibility. This report gates upload and
// start, so the properties that matter are that proven incompatibility blocks,
// that unknown data never masquerades as a pass, and that severity only
// escalates.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/PlateCompatibility.hpp"

using namespace Slic3r;

namespace {

PrinterState printer()
{
    PrinterState state;
    state.profile.name           = "Reference toolchanger";
    state.profile.technology     = ptFFF;
    state.profile.tool_count     = 4;
    state.profile.bed_width_mm   = 250.;
    state.profile.bed_depth_mm   = 210.;
    state.profile.max_height_mm  = 220.;
    state.profile.output_formats = { "gcode" };
    state.installed_nozzles_mm   = std::vector<double>{ 0.4, 0.2, 0.6, 0.8 };
    state.loaded_materials       = std::vector<std::string>{ "PLA", "PLA", "PLA", "PLA" };
    state.min_layer_height_mm    = 0.05;
    state.max_layer_height_mm    = 0.45;
    return state;
}

PlateRequirements plate()
{
    PlateRequirements needs;
    needs.technology         = ptFFF;
    needs.width_mm           = 100.;
    needs.depth_mm           = 100.;
    needs.height_mm          = 50.;
    needs.used_tools         = { 0, 1 };
    needs.required_nozzle_mm = { 0.4, 0.2 };
    needs.required_materials = { "PLA", "PLA" };
    needs.layer_height_mm    = 0.2;
    needs.output_format      = "gcode";
    return needs;
}

bool has_area(const CompatibilityReport &report, const std::string &area,
              CompatibilityState state)
{
    return std::any_of(report.findings.begin(), report.findings.end(),
        [&](const CompatibilityFinding &finding) {
            return finding.area == area && finding.state == state;
        });
}

} // namespace

TEST_CASE("A fully satisfied plate passes", "[plate-compat]")
{
    const CompatibilityReport report = check_plate_compatibility(plate(), printer());
    CHECK(report.state == CompatibilityState::Pass);
    CHECK_FALSE(report.blocked());
}

TEST_CASE("Technology mismatch blocks", "[plate-compat]")
{
    PlateRequirements resin = plate();
    resin.technology = ptSLA;
    const CompatibilityReport report = check_plate_compatibility(resin, printer());
    CHECK(report.blocked());
    CHECK(has_area(report, "Technology", CompatibilityState::Blocked));
}

TEST_CASE("An object larger than the bed blocks", "[plate-compat]")
{
    PlateRequirements oversized = plate();
    oversized.width_mm = 400.;
    const CompatibilityReport report = check_plate_compatibility(oversized, printer());
    CHECK(report.blocked());
    CHECK(has_area(report, "Build volume", CompatibilityState::Blocked));
}

TEST_CASE("Using a tool the printer does not have blocks", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.used_tools         = { 0, 7 };
    needs.required_nozzle_mm = { 0.4, 0.4 };
    needs.required_materials.clear();
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.blocked());
    CHECK(has_area(report, "Tools", CompatibilityState::Blocked));
}

TEST_CASE("A wrong nozzle blocks rather than warns", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.required_nozzle_mm = { 0.8, 0.2 };  // T0 actually has 0.4
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.blocked());
    CHECK(has_area(report, "Nozzles", CompatibilityState::Blocked));
}

TEST_CASE("A material mismatch warns and can be acknowledged", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.required_materials = { "PETG", "PLA" };
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.state == CompatibilityState::Warning);
    CHECK_FALSE(report.blocked());
    CHECK(has_area(report, "Materials", CompatibilityState::Warning));
}

TEST_CASE("Unreported printer state warns instead of silently passing", "[plate-compat]")
{
    PrinterState unknown = printer();
    unknown.loaded_materials.reset();
    unknown.installed_nozzles_mm.reset();

    const CompatibilityReport report = check_plate_compatibility(plate(), unknown);
    // The distinction that matters: not verified is not the same as verified fine.
    CHECK(report.state == CompatibilityState::Warning);
    CHECK(has_area(report, "Materials", CompatibilityState::Warning));
    CHECK(has_area(report, "Nozzles", CompatibilityState::Warning));
}

TEST_CASE("An unknown plate extent warns", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.height_mm.reset();
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.state == CompatibilityState::Warning);
    CHECK(has_area(report, "Build volume", CompatibilityState::Warning));
}

TEST_CASE("Layer heights outside the printer's range block", "[plate-compat]")
{
    PlateRequirements too_fine = plate();
    too_fine.layer_height_mm = 0.01;
    CHECK(check_plate_compatibility(too_fine, printer()).blocked());

    PlateRequirements too_coarse = plate();
    too_coarse.layer_height_mm = 0.9;
    CHECK(check_plate_compatibility(too_coarse, printer()).blocked());
}

TEST_CASE("An unsupported output format blocks", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.output_format = "bgcode";
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.blocked());
    CHECK(has_area(report, "Output format", CompatibilityState::Blocked));
}

TEST_CASE("A missing required firmware capability blocks", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.required_capabilities = { "TOOLCHANGE" };
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.blocked());
    CHECK(has_area(report, "Firmware", CompatibilityState::Blocked));

    PrinterState capable = printer();
    capable.supported_capabilities = { "toolchange" };  // case-insensitive
    CHECK_FALSE(check_plate_compatibility(needs, capable).blocked());
}

TEST_CASE("A job larger than free storage blocks", "[plate-compat]")
{
    PrinterState full = printer();
    full.free_storage_bytes = 1024;
    full.job_size_bytes     = 4096;
    const CompatibilityReport report = check_plate_compatibility(plate(), full);
    CHECK(report.blocked());
    CHECK(has_area(report, "Storage", CompatibilityState::Blocked));
}

TEST_CASE("A block is never downgraded by later passing checks", "[plate-compat]")
{
    PlateRequirements needs = plate();
    needs.technology = ptSLA;          // blocks early
    needs.output_format = "gcode";     // would pass later
    const CompatibilityReport report = check_plate_compatibility(needs, printer());
    CHECK(report.state == CompatibilityState::Blocked);
}
