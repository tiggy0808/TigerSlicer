// Phase N: printer/profile capability matching. The rules here decide what
// TigerSlicer proposes when a printer appears on the network, so the important
// property is that it never over-claims: no silent selection, no invented
// capability, and blocking mismatches actually block.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrinterCapabilityMatch.hpp"

using namespace Slic3r;

namespace {

PrinterProfileSummary fdm_profile(const std::string &name, const std::string &model,
                                  int tools = 1)
{
    PrinterProfileSummary profile;
    profile.name          = name;
    profile.vendor        = "Prusa";
    profile.model         = model;
    profile.technology    = ptFFF;
    profile.tool_count    = tools;
    profile.bed_width_mm  = 250.;
    profile.bed_depth_mm  = 210.;
    profile.max_height_mm = 220.;
    profile.output_formats = { "gcode" };
    return profile;
}

PrinterDeviceCapabilities fdm_device(const std::string &model)
{
    PrinterDeviceCapabilities device;
    device.vendor           = "Prusa";
    device.model            = model;
    device.technology_known = true;
    device.technology       = ptFFF;
    device.bed_width_mm     = 250.;
    device.bed_depth_mm     = 210.;
    device.max_height_mm    = 220.;
    device.output_format    = "gcode";
    return device;
}

} // namespace

TEST_CASE("An entirely agreeing profile matches exactly", "[printer-match]")
{
    const ProfileMatch match = match_printer_profile(
        fdm_device("MK4"), { fdm_profile("Original Prusa MK4", "MK4") });

    CHECK(match.level == ProfileMatchLevel::Exact);
    CHECK(match.best_profile == "Original Prusa MK4");
    CHECK(match.warnings.empty());
    CHECK_FALSE(match.evidence.empty());
}

TEST_CASE("A technology mismatch blocks rather than warns", "[printer-match]")
{
    PrinterDeviceCapabilities resin = fdm_device("SL1S");
    resin.technology = ptSLA;
    resin.output_format.clear();

    const ProfileMatch match = match_printer_profile(
        resin, { fdm_profile("Original Prusa MK4", "MK4") });

    // Offering an FDM profile for a resin printer would be unsafe, so the
    // only acceptable outcome is no compatible profile.
    CHECK(match.level == ProfileMatchLevel::NoCompatibleProfile);
    CHECK(match.best_profile.empty());
    // The rejection must still explain itself.
    CHECK_FALSE(match.evidence.empty());
}

TEST_CASE("A profile larger than the machine is rejected", "[printer-match]")
{
    PrinterProfileSummary oversized = fdm_profile("Oversized", "MK4");
    oversized.bed_width_mm = 400.;

    const ProfileMatch match = match_printer_profile(fdm_device("MK4"), { oversized });

    CHECK(match.level == ProfileMatchLevel::NoCompatibleProfile);
}

TEST_CASE("A format the printer cannot read is rejected", "[printer-match]")
{
    PrinterProfileSummary binary_only = fdm_profile("Binary only", "MK4");
    binary_only.output_formats = { "bgcode" };

    const ProfileMatch match = match_printer_profile(fdm_device("MK4"), { binary_only });

    CHECK(match.level == ProfileMatchLevel::NoCompatibleProfile);
}

TEST_CASE("Equally good candidates are reported as ambiguous, never guessed", "[printer-match]")
{
    // Two profiles that agree on everything the device advertised.
    const ProfileMatch match = match_printer_profile(
        fdm_device("MK4"),
        { fdm_profile("MK4 copy A", "MK4"), fdm_profile("MK4 copy B", "MK4") });

    CHECK(match.level == ProfileMatchLevel::Ambiguous);
    // Crucially, nothing is chosen on the user's behalf.
    CHECK(match.best_profile.empty());
    CHECK(match.tied_profiles.size() == 2);
}

TEST_CASE("A non-blocking disagreement yields a warning, not a rejection", "[printer-match]")
{
    PrinterDeviceCapabilities device = fdm_device("MK4");
    device.tool_count = 5; // profile declares 1

    const ProfileMatch match = match_printer_profile(
        device, { fdm_profile("Original Prusa MK4", "MK4") });

    CHECK(match.level == ProfileMatchLevel::CompatibleWithWarnings);
    CHECK(match.best_profile == "Original Prusa MK4");
    CHECK_FALSE(match.warnings.empty());
}

TEST_CASE("Facts the device never advertised are not treated as agreement", "[printer-match]")
{
    // A device that says nothing about itself beyond being reachable.
    PrinterDeviceCapabilities silent;
    const ProfileMatch match = match_printer_profile(
        silent, { fdm_profile("Original Prusa MK4", "MK4") });

    // With nothing checked, there is no basis to claim an exact match.
    CHECK(match.level != ProfileMatchLevel::Exact);
    CHECK(match.evidence.empty());
}

TEST_CASE("Cosmetic model spelling differences still match", "[printer-match]")
{
    PrinterDeviceCapabilities device = fdm_device("MK4 IS");
    const ProfileMatch match = match_printer_profile(
        device, { fdm_profile("Original Prusa MK4IS", "MK4IS") });

    CHECK(match.level == ProfileMatchLevel::Exact);
}

TEST_CASE("With no profiles installed nothing is proposed", "[printer-match]")
{
    const ProfileMatch match = match_printer_profile(fdm_device("MK4"), {});
    CHECK(match.level == ProfileMatchLevel::NoCompatibleProfile);
    CHECK(match.best_profile.empty());
    CHECK(match.tied_profiles.empty());
}
