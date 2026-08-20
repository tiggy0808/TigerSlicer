// Phase 11: calibration and offset maps. The tests concentrate on confidence
// rather than on arithmetic: an unrecorded uncertainty must never behave like
// a small one, a stale document must never undo a newer calibration, and a
// tool whose offsets the firmware owns must be recorded but not applied.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <cmath>

#include "libslic3r/CalibrationMap.hpp"

using namespace Slic3r;

namespace {

const char *DOCUMENT = R"(# written by the printer
schema 1
tool 0
version 3
firmware_authoritative 0
offset_x -0.125 0.010 printer 2026-07-01T10:00:00Z
offset_y 0.030 0.010 printer 2026-07-01T10:00:00Z
offset_z 0.000 0.020 printer 2026-07-01T10:00:00Z
flow 0.980 0.020 printer 2026-07-01T10:00:00Z
drift 200 0.000 0.000 0.000
drift 260 0.020 0.000 -0.040
bed 10 10 0.050
bed 200 200 -0.030
)";

CalibrationMap parsed(const char *text = DOCUMENT)
{
    CalibrationMap map;
    std::string    error;
    REQUIRE(parse_calibration_document(text, &map, &error));
    REQUIRE(error.empty());
    return map;
}

} // namespace

TEST_CASE("A printer-written document is imported without touching the printer",
          "[calibration-map]")
{
    const CalibrationMap map = parsed();

    REQUIRE(map.schema_version == 1);
    REQUIRE(map.tools.size() == 1);
    const ToolCalibration *tool = map.find(0);
    REQUIRE(tool != nullptr);

    CHECK(tool->version == 3);
    CHECK(tool->offset_x.value == Catch::Approx(-0.125));
    CHECK(tool->offset_x.uncertainty.has_value());
    CHECK(*tool->offset_x.uncertainty == Catch::Approx(0.010));
    CHECK(tool->offset_x.source == CalibrationSource::PrinterMeasured);
    CHECK(tool->offset_x.timestamp == "2026-07-01T10:00:00Z");
    REQUIRE(tool->first_layer_flow.has_value());
    CHECK(tool->first_layer_flow->value == Catch::Approx(0.980));
    CHECK(tool->thermal_drift.size() == 2);
    CHECK(tool->bed_corrections.size() == 2);
}

TEST_CASE("A document round-trips through serialization", "[calibration-map]")
{
    const CalibrationMap original = parsed();
    const std::string    text     = serialize_calibration_document(original);

    CalibrationMap again;
    std::string    error;
    REQUIRE(parse_calibration_document(text, &again, &error));

    const ToolCalibration *before = original.find(0);
    const ToolCalibration *after  = again.find(0);
    REQUIRE(after != nullptr);
    CHECK(after->version == before->version);
    CHECK(after->offset_x.value == Catch::Approx(before->offset_x.value));
    CHECK(after->thermal_drift.size() == before->thermal_drift.size());
    CHECK(after->bed_corrections.size() == before->bed_corrections.size());
}

TEST_CASE("An unrecorded uncertainty survives the round trip as unrecorded",
          "[calibration-map]")
{
    // A dash means nobody recorded the confidence. Writing 0 instead would
    // claim perfect knowledge the document never asserted.
    const char *sparse = "schema 1\ntool 0\nversion 1\n"
                         "offset_x 0.100 - user\noffset_y 0.000 0.010 user\n"
                         "offset_z 0.000 0.010 user\n";
    const CalibrationMap map = parsed(sparse);
    CHECK_FALSE(map.find(0)->offset_x.uncertainty.has_value());

    CalibrationMap again;
    std::string    error;
    REQUIRE(parse_calibration_document(serialize_calibration_document(map), &again, &error));
    CHECK_FALSE(again.find(0)->offset_x.uncertainty.has_value());
}

TEST_CASE("Malformed documents are refused, and nothing partial is returned",
          "[calibration-map]")
{
    CalibrationMap map;
    std::string    error;

    SECTION("no schema version") {
        CHECK_FALSE(parse_calibration_document("tool 0\nversion 1\n", &map, &error));
        CHECK(error.find("schema version") != std::string::npos);
    }

    SECTION("data before any tool") {
        CHECK_FALSE(parse_calibration_document("schema 1\nversion 2\n", &map, &error));
        CHECK(error.find("before any tool") != std::string::npos);
    }

    SECTION("an unrecognised keyword") {
        CHECK_FALSE(parse_calibration_document("schema 1\ntool 0\nwobble 1\n", &map, &error));
        CHECK(error.find("unrecognised keyword") != std::string::npos);
    }

    SECTION("an unrecognised source") {
        CHECK_FALSE(parse_calibration_document(
            "schema 1\ntool 0\noffset_x 0.1 0.01 telepathy\n", &map, &error));
        CHECK(error.find("not a recognised source") != std::string::npos);
    }

    SECTION("a negative uncertainty") {
        CHECK_FALSE(parse_calibration_document(
            "schema 1\ntool 0\noffset_x 0.1 -0.01 user\n", &map, &error));
        CHECK(error.find("cannot be negative") != std::string::npos);
    }

    SECTION("non-finite measurements") {
        CHECK_FALSE(parse_calibration_document(
            "schema 1\ntool 0\noffset_x nan 0.01 user\n", &map, &error));
        CHECK(error.find("finite measurement") != std::string::npos);
    }

    SECTION("non-finite uncertainty") {
        CHECK_FALSE(parse_calibration_document(
            "schema 1\ntool 0\noffset_x 0.1 inf user\n", &map, &error));
        CHECK(error.find("must be finite") != std::string::npos);
    }

    SECTION("a document with no tools") {
        CHECK_FALSE(parse_calibration_document("schema 1\n", &map, &error));
        CHECK(error.find("no tools") != std::string::npos);
    }
}

TEST_CASE("A stale document never undoes a newer calibration", "[calibration-map]")
{
    CalibrationMap current = parsed();   // T0 at version 3

    const char *older = "schema 1\ntool 0\nversion 2\n"
                        "offset_x 9.999 0.010 user\noffset_y 0.0 0.010 user\n"
                        "offset_z 0.0 0.010 user\n";
    const MergeOutcome outcome = merge_calibration(parsed(older), &current);

    CHECK(outcome.applied.empty());
    REQUIRE(outcome.rejected.size() == 1);
    CHECK(outcome.rejected.front().find("kept at version 3") != std::string::npos);
    CHECK(current.find(0)->offset_x.value == Catch::Approx(-0.125));
}

TEST_CASE("A newer document replaces an older calibration", "[calibration-map]")
{
    CalibrationMap current = parsed();

    const char *newer = "schema 1\ntool 0\nversion 4\n"
                        "offset_x -0.200 0.005 printer\noffset_y 0.0 0.005 printer\n"
                        "offset_z 0.0 0.005 printer\n";
    const MergeOutcome outcome = merge_calibration(parsed(newer), &current);

    REQUIRE(outcome.applied.size() == 1);
    CHECK(outcome.applied.front().find("version 3 to 4") != std::string::npos);
    CHECK(current.find(0)->offset_x.value == Catch::Approx(-0.200));
}

TEST_CASE("A tool absent from the map is added", "[calibration-map]")
{
    CalibrationMap current = parsed();
    const char *second = "schema 1\ntool 1\nversion 1\n"
                         "offset_x 0.5 0.01 printer\noffset_y 0.0 0.01 printer\n"
                         "offset_z 0.0 0.01 printer\n";
    const MergeOutcome outcome = merge_calibration(parsed(second), &current);

    REQUIRE(outcome.applied.size() == 1);
    CHECK(current.tools.size() == 2);
    CHECK(current.find(1) != nullptr);
}

TEST_CASE("Offsets are applied with drift interpolated between measurements",
          "[calibration-map]")
{
    // Halfway between the 200 C and 260 C drift samples.
    const ResolvedOffset resolved = resolve_offset(parsed(), 0, 230., 10., 10.);

    CHECK(resolved.status == OffsetStatus::Applied);
    CHECK(resolved.dx == Catch::Approx(-0.125 + 0.010));
    CHECK(resolved.dz == Catch::Approx(0.000 - 0.020 + 0.050));   // drift plus bed correction
    CHECK_FALSE(resolved.drift_uncharacterized);
    REQUIRE(resolved.uncertainty.has_value());
    // 10, 10 and 20 micron terms in quadrature.
    CHECK(*resolved.uncertainty == Catch::Approx(std::sqrt(0.0001 + 0.0001 + 0.0004)));
}

TEST_CASE("Drift outside the characterized range is not extrapolated",
          "[calibration-map]")
{
    const ResolvedOffset resolved = resolve_offset(parsed(), 0, 300., 10., 10.);

    CHECK(resolved.drift_uncharacterized);
    // No drift was applied, so the offset is the base value plus bed only.
    CHECK(resolved.dx == Catch::Approx(-0.125));
    // And the uncertainty becomes unknown rather than staying reassuringly
    // small when part of the correction could not be determined.
    CHECK_FALSE(resolved.uncertainty.has_value());
}

TEST_CASE("Firmware-owned offsets are recorded but never applied",
          "[calibration-map]")
{
    const char *firmware = "schema 1\ntool 0\nversion 1\nfirmware_authoritative 1\n"
                           "offset_x -0.125 0.010 printer\noffset_y 0.0 0.010 printer\n"
                           "offset_z 0.0 0.010 printer\n";
    const CalibrationMap map = parsed(firmware);

    // The value is still in the map for inspection.
    CHECK(map.find(0)->offset_x.value == Catch::Approx(-0.125));

    const ResolvedOffset resolved = resolve_offset(map, 0, 220., 10., 10.);
    CHECK(resolved.status == OffsetStatus::FirmwareAuthoritative);
    // Applying it here as well would double-correct.
    CHECK(resolved.dx == Catch::Approx(0.));
    CHECK(resolved.notes.front().find("not applied") != std::string::npos);
}

TEST_CASE("A tool with no calibration resolves to nothing, explicitly",
          "[calibration-map]")
{
    const ResolvedOffset resolved = resolve_offset(parsed(), 7, 220., 10., 10.);
    CHECK(resolved.status == OffsetStatus::NoCalibration);
    CHECK(resolved.dx == Catch::Approx(0.));
    CHECK(resolved.notes.front().find("no calibration is recorded") != std::string::npos);
}

TEST_CASE("The bed correction names how far the nearest sample was",
          "[calibration-map]")
{
    const ResolvedOffset resolved = resolve_offset(parsed(), 0, 220., 195., 195.);
    // The (200, 200) sample is nearer than the (10, 10) one, and the Z drift
    // interpolates a third of the way from 200 C to 260 C.
    CHECK(resolved.dz == Catch::Approx(-0.030 + (-0.040) * ((220. - 200.) / 60.)));
    bool named = false;
    for (const std::string &note : resolved.notes)
        if (note.find("mm away at (200.0, 200.0)") != std::string::npos)
            named = true;
    CHECK(named);
}

TEST_CASE("An uncharacterized bed or drift is stated, not assumed zero",
          "[calibration-map]")
{
    const char *bare = "schema 1\ntool 0\nversion 1\n"
                       "offset_x 0.1 0.01 user\noffset_y 0.0 0.01 user\noffset_z 0.0 0.01 user\n";
    const ResolvedOffset resolved = resolve_offset(parsed(bare), 0, 220., 10., 10.);

    CHECK(resolved.drift_uncharacterized);
    CHECK(resolved.bed_correction_uncharacterized);
    bool drift = false, bed = false;
    for (const std::string &note : resolved.notes) {
        if (note.find("no thermal drift is characterized") != std::string::npos)
            drift = true;
        if (note.find("no bed-location correction") != std::string::npos)
            bed = true;
    }
    CHECK(drift);
    CHECK(bed);
}

TEST_CASE("Required accuracy is checked against real confidence",
          "[calibration-map]")
{
    const ResolvedOffset resolved = resolve_offset(parsed(), 0, 230., 10., 10.);
    REQUIRE(resolved.uncertainty.has_value());

    // The combined confidence is about 0.024 mm.
    CHECK(check_accuracy_against_calibration(resolved, 0.05).empty());

    const std::string warning = check_accuracy_against_calibration(resolved, 0.01);
    CHECK_FALSE(warning.empty());
    CHECK(warning.find("only confident to") != std::string::npos);
}

TEST_CASE("Unknown confidence never passes an accuracy check",
          "[calibration-map]")
{
    const char *sparse = "schema 1\ntool 0\nversion 1\n"
                         "offset_x 0.1 - user\noffset_y 0.0 0.001 user\n"
                         "offset_z 0.0 0.001 user\ndrift 200 0 0 0\ndrift 260 0 0 0\n"
                         "bed 10 10 0\n";
    const ResolvedOffset resolved = resolve_offset(parsed(sparse), 0, 220., 10., 10.);
    REQUIRE_FALSE(resolved.uncertainty.has_value());

    // Even a very loose requirement cannot be shown to be met.
    const std::string warning = check_accuracy_against_calibration(resolved, 1.0);
    CHECK_FALSE(warning.empty());
    CHECK(warning.find("never recorded") != std::string::npos);
}

TEST_CASE("A tool with no calibration fails any accuracy requirement",
          "[calibration-map]")
{
    const ResolvedOffset resolved = resolve_offset(parsed(), 7, 220., 10., 10.);
    const std::string warning = check_accuracy_against_calibration(resolved, 0.5);
    CHECK(warning.find("no calibration at all") != std::string::npos);
}
