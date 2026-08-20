// Phase N: loaded tool/material synchronization. The distinction these tests
// protect is provenance: a value the user typed must never be reported with
// the same confidence as one the printer sensed, and unknown state must never
// read as agreement.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/ToolStateSync.hpp"

using namespace Slic3r;

namespace {

ToolState tool(int index, double nozzle, const std::string &material,
               StateProvenance provenance = StateProvenance::Sensed)
{
    ToolState state;
    state.index                = index;
    state.nozzle_mm.value      = nozzle;
    state.nozzle_mm.provenance = provenance;
    state.material.value       = material;
    state.material.provenance  = provenance;
    state.available.value      = true;
    state.available.provenance = provenance;
    return state;
}

ToolRequirement need(int index, double nozzle, const std::string &material)
{
    ToolRequirement requirement;
    requirement.index     = index;
    requirement.nozzle_mm = nozzle;
    requirement.material  = material;
    return requirement;
}

const ToolMismatch *field(const ToolSyncReport &report, int tool, const std::string &name)
{
    const auto it = std::find_if(report.mismatches.begin(), report.mismatches.end(),
        [&](const ToolMismatch &mismatch) {
            return mismatch.tool == tool && mismatch.field == name;
        });
    return it == report.mismatches.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("Matching tool state produces no findings", "[tool-sync]")
{
    const ToolSyncReport report = compare_tool_state(
        { need(0, 0.4, "PLA") }, { tool(0, 0.4, "PLA") });

    CHECK(report.mismatches.empty());
    CHECK_FALSE(report.blocked());
}

TEST_CASE("A missing tool blocks", "[tool-sync]")
{
    const ToolSyncReport report = compare_tool_state(
        { need(3, 0.4, "PLA") }, { tool(0, 0.4, "PLA") });

    CHECK(report.blocked());
    REQUIRE(field(report, 3, "tool") != nullptr);
    CHECK(field(report, 3, "tool")->severity == MismatchSeverity::Blocking);
}

TEST_CASE("An unavailable tool blocks", "[tool-sync]")
{
    ToolState faulted = tool(0, 0.4, "PLA");
    faulted.available.value = false;

    const ToolSyncReport report = compare_tool_state({ need(0, 0.4, "PLA") }, { faulted });
    CHECK(report.blocked());
    CHECK(field(report, 0, "availability")->severity == MismatchSeverity::Blocking);
}

TEST_CASE("A wrong nozzle blocks but a wrong material only warns", "[tool-sync]")
{
    const ToolSyncReport nozzle = compare_tool_state(
        { need(0, 0.4, "PLA") }, { tool(0, 0.8, "PLA") });
    CHECK(nozzle.blocked());

    // A material difference may be intentional, so it must stay recoverable.
    const ToolSyncReport material = compare_tool_state(
        { need(0, 0.4, "PLA") }, { tool(0, 0.4, "PETG") });
    CHECK_FALSE(material.blocked());
    CHECK(material.has_warnings());
    CHECK(field(material, 0, "material")->severity == MismatchSeverity::Warning);
}

TEST_CASE("Unknown state warns rather than passing silently", "[tool-sync]")
{
    ToolState silent;
    silent.index = 0;   // nothing known about it at all
    silent.available.value      = true;
    silent.available.provenance = StateProvenance::Configured;

    const ToolSyncReport report = compare_tool_state({ need(0, 0.4, "PLA") }, { silent });
    CHECK_FALSE(report.blocked());
    CHECK(report.has_warnings());
    REQUIRE(field(report, 0, "nozzle") != nullptr);
    CHECK(field(report, 0, "nozzle")->provenance == StateProvenance::Unknown);
    REQUIRE(field(report, 0, "material") != nullptr);
}

TEST_CASE("Provenance travels with the finding", "[tool-sync]")
{
    // The same disagreement, believed on different grounds.
    const ToolSyncReport sensed = compare_tool_state(
        { need(0, 0.4, "PLA") }, { tool(0, 0.4, "PETG", StateProvenance::Sensed) });
    CHECK(field(sensed, 0, "material")->provenance == StateProvenance::Sensed);

    const ToolSyncReport assumed = compare_tool_state(
        { need(0, 0.4, "PLA") }, { tool(0, 0.4, "PETG", StateProvenance::Assumed) });
    // Same severity, but the UI can now say how much to trust it.
    CHECK(field(assumed, 0, "material")->provenance == StateProvenance::Assumed);
}

TEST_CASE("A colour difference is informational only", "[tool-sync]")
{
    ToolRequirement requirement = need(0, 0.4, "PLA");
    requirement.color = "#FF0000";

    ToolState state = tool(0, 0.4, "PLA");
    state.color.value      = "#00FF00";
    state.color.provenance = StateProvenance::Assumed;

    const ToolSyncReport report = compare_tool_state({ requirement }, { state });
    CHECK_FALSE(report.blocked());
    CHECK_FALSE(report.has_warnings());
    CHECK(field(report, 0, "color")->severity == MismatchSeverity::Info);
}

TEST_CASE("Insufficient remaining material warns", "[tool-sync]")
{
    ToolRequirement requirement = need(0, 0.4, "PLA");
    requirement.needed_g = 500.;

    ToolState state = tool(0, 0.4, "PLA");
    state.remaining_g.value      = 100.;
    state.remaining_g.provenance = StateProvenance::Sensed;

    const ToolSyncReport report = compare_tool_state({ requirement }, { state });
    CHECK(report.has_warnings());
    CHECK(field(report, 0, "remaining")->severity == MismatchSeverity::Warning);
}

TEST_CASE("Unknown remaining quantity is not treated as insufficient", "[tool-sync]")
{
    ToolRequirement requirement = need(0, 0.4, "PLA");
    requirement.needed_g = 500.;

    // Nothing reported about remaining material.
    const ToolSyncReport report = compare_tool_state({ requirement }, { tool(0, 0.4, "PLA") });
    CHECK(field(report, 0, "remaining") == nullptr);
}

TEST_CASE("A different calibration set warns", "[tool-sync]")
{
    ToolRequirement requirement = need(0, 0.4, "PLA");
    requirement.calibration_id = "cal-2026-01";

    ToolState state = tool(0, 0.4, "PLA");
    state.calibration_id = "cal-2025-06";

    const ToolSyncReport report = compare_tool_state({ requirement }, { state });
    CHECK(report.has_warnings());
    CHECK(field(report, 0, "calibration")->severity == MismatchSeverity::Warning);
}
