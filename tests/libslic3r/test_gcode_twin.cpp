// Phase 26: the digital G-code twin. What is under test is mostly what the
// twin refuses to claim - it will not say a file is fine because it did not
// recognize the command that would have broken it, it will not treat a
// commanded temperature as a reached one, and it will not follow a macro whose
// meaning nobody declared. Every check runs against something the machine
// actually declared; where nothing was declared, that is the finding.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/GcodeTwin.hpp"

using namespace Slic3r;

namespace {

TwinTool tool(int index, const std::string &material, double minimum_c)
{
    TwinTool value;
    value.index               = index;
    value.material            = material;
    value.minimum_extrusion_c = minimum_c;
    value.nozzle_mm           = 0.4;
    value.offset_x_mm         = 0.;
    value.offset_y_mm         = 0.;
    value.offset_z_mm         = 0.;
    return value;
}

TwinMachine machine()
{
    TwinMachine value;
    value.tools               = { tool(0, "PLA", 170.), tool(1, "PLA", 170.) };
    value.min_x_mm            = 0.;
    value.max_x_mm            = 250.;
    value.min_y_mm            = 0.;
    value.max_y_mm            = 210.;
    value.min_z_mm            = 0.;
    value.max_z_mm            = 220.;
    value.safe_travel_z_mm    = 0.4;
    value.max_layer_height_mm = 0.3;
    value.filament_diameter_mm = 1.75;
    return value;
}

// A short, well formed file: home, declare the modes, heat and wait, select a
// tool, print a line, travel above the safe height.
const char *GOOD = R"(G90
M83
G28
M104 T0 S210
M109 T0 S210
T0
;LAYER:0
G1 F1200 X10 Y10 Z0.2
G1 X20 Y10 E0.5
G1 Z1.0 F600
G1 X50 Y50
;CHECKPOINT:7
)";

bool mentions(const TwinResult &result, const std::string &fragment)
{
    return std::any_of(result.findings.begin(), result.findings.end(),
                       [&](const TwinFinding &finding) {
                           return finding.message.find(fragment) != std::string::npos;
                       });
}

size_t issues(const TwinResult &result, TwinIssue issue)
{
    return size_t(std::count_if(result.findings.begin(), result.findings.end(),
                                [&](const TwinFinding &finding) {
                                    return finding.issue == issue;
                                }));
}

} // namespace

TEST_CASE("A well formed file interprets cleanly", "[gcode-twin]")
{
    const TwinResult result = interpret_gcode(GOOD, machine(), TwinPolicy {});

    CHECK(result.error.empty());
    CHECK(result.acceptable());
    CHECK(result.count(TwinSeverity::Error) == 0);

    REQUIRE(result.final_state.active_tool.has_value());
    CHECK(*result.final_state.active_tool == 0);
    CHECK(result.final_state.xyz_mode == PositioningMode::Absolute);
    CHECK(result.final_state.e_mode == PositioningMode::Relative);
    REQUIRE(result.final_state.x_mm.has_value());
    CHECK(*result.final_state.x_mm == 50.);
    REQUIRE(result.final_state.z_mm.has_value());
    CHECK(*result.final_state.z_mm == 1.0);
    REQUIRE(result.final_state.feedrate_mm_min.has_value());
    CHECK(*result.final_state.feedrate_mm_min == 600.);
    REQUIRE(result.final_state.layer.has_value());
    CHECK(*result.final_state.layer == 0);

    CHECK(result.extrusions == 1);
    CHECK(result.extruded_mm.at(0) == 0.5);
    REQUIRE(result.checkpoints.size() == 1);
    CHECK(result.checkpoints.front() == 7);
}

TEST_CASE("A commanded temperature is not a reached one", "[gcode-twin]")
{
    // M104 sets it and does not wait. Extruding after that is extruding into
    // a temperature nobody confirmed.
    const char *impatient = "G90\nM83\nM104 T0 S210\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n";

    const TwinResult result = interpret_gcode(impatient, machine(), TwinPolicy {});
    CHECK_FALSE(result.acceptable());
    CHECK(result.has(TwinIssue::MissingWait));
    CHECK(mentions(result, "commanded, not reached"));

    // The same file with the wait is clean.
    const char *patient =
        "G90\nM83\nM104 T0 S210\nM109 T0 S210\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n";
    CHECK(interpret_gcode(patient, machine(), TwinPolicy {}).acceptable());
}

TEST_CASE("Cold extrusion is measured against the material, not guessed",
          "[gcode-twin]")
{
    SECTION("below what the material needs") {
        const char *cold =
            "G90\nM83\nM104 T0 S150\nM109 T0 S150\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n";
        const TwinResult result = interpret_gcode(cold, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::ColdExtrusion));
        CHECK(mentions(result, "below the 170.00 C its material needs"));
    }

    SECTION("no temperature commanded at all") {
        const char      *never  = "G90\nM83\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n";
        const TwinResult result = interpret_gcode(never, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::ColdExtrusion));
        CHECK(mentions(result, "without any temperature ever having been commanded"));
    }

    SECTION("a material nobody characterized cannot be shown to be hot enough") {
        TwinMachine uncharacterized       = machine();
        uncharacterized.tools[0].minimum_extrusion_c.reset();

        const char      *file   = "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n";
        const TwinResult result = interpret_gcode(file, uncharacterized, TwinPolicy {});
        CHECK(result.has(TwinIssue::ColdExtrusion));
        CHECK(mentions(result, "cannot be shown not to be a cold extrusion"));
        // Unknown, not wrong: it does not fail the file on its own.
        CHECK(result.acceptable());
    }
}

TEST_CASE("A move with no speed is a defect, not a default", "[gcode-twin]")
{
    SECTION("an explicit zero") {
        const char *zero =
            "G90\nM83\nM109 T0 S210\nT0\nG1 F0 X10 Y10\n";
        const TwinResult result = interpret_gcode(zero, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::ZeroFeedrate));
        CHECK(mentions(result, "never finishes or runs at whatever the firmware last had"));
    }

    SECTION("never set at all") {
        const char *unset = "G90\nM83\nM109 T0 S210\nT0\nG1 X10 Y10\n";
        const TwinResult result = interpret_gcode(unset, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::ZeroFeedrate));
        CHECK(mentions(result, "whatever the firmware happened to be left at"));
    }
}

TEST_CASE("Extruding before the modes are declared is a defect", "[gcode-twin]")
{
    SECTION("no G90 or G91") {
        const char      *file   = "M83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10 E0.5\n";
        const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::BadMode));
        CHECK(mentions(result, "whether coordinates are absolute or relative"));
    }

    SECTION("no M82 or M83") {
        // G90 sets XYZ absolute and, on machines that do, E with it; this file
        // uses G91, which says nothing about E.
        const char      *file   = "G91\nM109 T0 S210\nT0\nG1 F1200 X10 Y10 E0.5\n";
        const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
        CHECK(mentions(result, "whether E is absolute or relative"));
    }
}

TEST_CASE("A tool the machine does not have, and extruding with none held",
          "[gcode-twin]")
{
    SECTION("a tool that was never declared") {
        const char      *file   = "G90\nM83\nT7\nG1 F1200 X10 Y10\n";
        const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::WrongTool));
        CHECK(mentions(result, "declares no such tool"));
    }

    SECTION("extruding before any tool was selected") {
        const char      *file   = "G90\nM83\nM109 T0 S210\nG1 F1200 X10 Y10 E0.5\n";
        const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
        CHECK(mentions(result, "whatever the machine was left holding"));
    }

    SECTION("an unrepresentably large tool number is rejected without throwing") {
        const char *file =
            "G90\nM83\nT999999999999999999999999999999999999999999999\n";
        TwinResult result;
        CHECK_NOTHROW(result = interpret_gcode(file, machine(), TwinPolicy {}));
        CHECK(result.has(TwinIssue::WrongTool));
        CHECK_FALSE(result.final_state.active_tool.has_value());
    }
}

TEST_CASE("Non-finite G-code parameters are malformed, not machine state",
          "[gcode-twin]")
{
    const char *file = "G90\nM83\nT0\nG1 Fnan Xinf Y10\n";
    const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});

    CHECK(result.has(TwinIssue::BadMode));
    CHECK_FALSE(result.final_state.feedrate_mm_min.has_value());
    CHECK_FALSE(result.final_state.x_mm.has_value());
}

TEST_CASE("Moves are checked against the declared build volume", "[gcode-twin]")
{
    const char      *file   = "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X400 Y10 Z1\n";
    const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
    CHECK(result.has(TwinIssue::OutOfRange));
    CHECK(mentions(result, "past the declared maximum of 250.00 mm"));

    // An axis with no declared bound is not checked, and is not claimed to be.
    TwinMachine unbounded = machine();
    unbounded.max_x_mm.reset();
    CHECK_FALSE(interpret_gcode(file, unbounded, TwinPolicy {}).has(TwinIssue::OutOfRange));
}

TEST_CASE("A travel below the declared safe height is refused", "[gcode-twin]")
{
    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10 Z0.2\nG1 X20 E0.5\nG1 X90 Y90\n";
    const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
    CHECK(result.has(TwinIssue::UnsafeZ));
    CHECK(mentions(result, "declares safe to travel at"));
}

TEST_CASE("A layer thicker than the machine declares it can print",
          "[gcode-twin]")
{
    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10 Z0.2\nG1 X20 E0.5\n"
        "G1 Z0.8\nG1 X30 E0.5\n";
    const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
    CHECK(result.has(TwinIssue::HeightViolation));
    CHECK(mentions(result, "past the 0.30 mm this machine declares it can print"));
}

TEST_CASE("An unmeasured tool offset is reported rather than assumed zero",
          "[gcode-twin]")
{
    TwinMachine unmeasured = machine();
    unmeasured.tools[0].offset_z_mm.reset();

    const char      *file = "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n";
    const TwinResult result = interpret_gcode(file, unmeasured, TwinPolicy {});
    CHECK(result.has(TwinIssue::OffsetViolation));
    CHECK(mentions(result, "never fully measured"));
}

TEST_CASE("An undeclared command is never assumed harmless", "[gcode-twin]")
{
    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\nSOME_CUSTOM_MACRO PARK=1\nG1 F1200 X10 Y10\n";

    SECTION("a warning by default") {
        const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
        CHECK(result.has(TwinIssue::UnknownCommand));
        CHECK(mentions(result, "how a verifier passes a file it never checked"));
        CHECK(result.count(TwinSeverity::Warning) > 0);
        CHECK(result.acceptable());
    }

    SECTION("an error where the safety policy says so") {
        TwinPolicy strict;
        strict.unknown_state_changing = UnknownCommandPolicy::Error;
        const TwinResult result       = interpret_gcode(file, machine(), strict);
        CHECK_FALSE(result.acceptable());
    }
}

TEST_CASE("A declared macro contract is interpreted, and only as declared",
          "[gcode-twin]")
{
    TwinMacroContract change;
    change.name         = "TOOLCHANGE_T1";
    change.changes_tool = true;
    change.selects_tool = 1;
    change.moves_axes   = true;
    change.waits_for_temperature = true;

    TwinMachine declared = machine();
    declared.macros      = { change };

    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10\nG1 X20 E0.5\n"
        "M104 T1 S215\nTOOLCHANGE_T1\nG1 X30 E0.5\n";

    const TwinResult result = interpret_gcode(file, declared, TwinPolicy {});

    CHECK_FALSE(result.has(TwinIssue::UnknownCommand));
    REQUIRE(result.final_state.active_tool.has_value());
    CHECK(*result.final_state.active_tool == 1);
    CHECK(result.toolchanges == 1);
    // The macro is declared to wait, so extruding on T1 afterwards is not a
    // missing wait.
    CHECK_FALSE(result.has(TwinIssue::MissingWait));
    CHECK(result.extruded_mm.at(1) == 0.5);
}

TEST_CASE("A macro that leaves the state undescribed stops the twin claiming to know it",
          "[gcode-twin]")
{
    TwinMacroContract park;
    park.name                     = "PARK_AND_PURGE";
    park.moves_axes               = true;
    park.leaves_state_undescribed = true;

    TwinMachine declared = machine();
    declared.macros      = { park };

    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10\nPARK_AND_PURGE\nG1 X20 Y20\n";

    const TwinResult result = interpret_gcode(file, declared, TwinPolicy {});

    CHECK(mentions(result, "unverified rather than assumed unchanged"));
    CHECK(mentions(result, "the position it starts from is not known"));
    CHECK_FALSE(result.final_state.position_known);
}

TEST_CASE("A macro declared to change the tool but not which one", "[gcode-twin]")
{
    TwinMacroContract vague;
    vague.name         = "NEXT_TOOL";
    vague.changes_tool = true;

    TwinMachine declared = machine();
    declared.macros      = { vague };

    const char      *file   = "G90\nM83\nM109 T0 S210\nT0\nNEXT_TOOL\nG1 F1200 X10 Y10 E0.5\n";
    const TwinResult result = interpret_gcode(file, declared, TwinPolicy {});

    CHECK(result.has(TwinIssue::WrongTool));
    CHECK(mentions(result, "which one is held afterwards is not known"));
    CHECK_FALSE(result.final_state.active_tool.has_value());
}

TEST_CASE("Material is totalled per tool, and volume needs a diameter",
          "[gcode-twin]")
{
    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\nG1 F1200 X10 Y10\nG1 X20 E1.0\nG1 X30 E2.0\n";

    const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
    CHECK(result.extruded_mm.at(0) == 3.0);
    REQUIRE(result.extruded_mm3.has_value());
    CHECK(result.extruded_mm3->at(0) > 7.2);
    CHECK(result.extruded_mm3->at(0) < 7.3);

    TwinMachine no_diameter = machine();
    no_diameter.filament_diameter_mm.reset();
    const TwinResult unknown = interpret_gcode(file, no_diameter, TwinPolicy {});
    CHECK_FALSE(unknown.extruded_mm3.has_value());
    CHECK(unknown.has(TwinIssue::MaterialVolume));
    CHECK(mentions(unknown, "unknown, not zero"));
}

TEST_CASE("An empty file is not a clean file", "[gcode-twin]")
{
    const TwinResult result = interpret_gcode("\n\n;just a comment\n", machine(),
                                              TwinPolicy {});
    CHECK_FALSE(result.error.empty());
    CHECK_FALSE(result.acceptable());
}

TEST_CASE("A file longer than the twin will read is not partly interpreted",
          "[gcode-twin]")
{
    std::string long_file = "G90\nM83\n";
    for (int i = 0; i < 50; ++i)
        long_file += "G1 F1200 X10 Y10\n";

    TwinPolicy small;
    small.max_lines = 10;

    const TwinResult result = interpret_gcode(long_file, machine(), small);
    CHECK_FALSE(result.error.empty());
    CHECK(result.error.find("a partial interpretation would look like a complete one") !=
          std::string::npos);
}

TEST_CASE("Checkpoint and layer markers are followed, and bad ones reported",
          "[gcode-twin]")
{
    const char *file =
        "G90\nM83\nM109 T0 S210\nT0\n;LAYER:3\nG1 F1200 X10 Y10\n;CHECKPOINT:12\n"
        ";CHECKPOINT:\n;LAYER:top\n";

    const TwinResult result = interpret_gcode(file, machine(), TwinPolicy {});
    REQUIRE(result.checkpoints.size() == 1);
    CHECK(result.checkpoints.front() == 12);
    REQUIRE(result.final_state.layer.has_value());
    CHECK(*result.final_state.layer == 3);
    CHECK(issues(result, TwinIssue::StateMismatch) == 2);
    CHECK(mentions(result, "nothing could resume from it"));
}
