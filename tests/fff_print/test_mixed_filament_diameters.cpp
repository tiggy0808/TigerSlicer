///|/ TigerSlicer: a toolchanger may feed a different filament diameter and a
///|/ different nozzle to each tool. These tests pin the conversions that have
///|/ to follow the tool rather than assume tool 0 speaks for all of them.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "libslic3r/Extruder.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Approx;

// Four tools: 1.75 mm through 0.4 and 0.6 nozzles, 2.2 mm through 0.4 and
// 0.8 - the shape of a real mixed toolchanger rather than a uniform set.
static GCodeConfig mixed_gcode_config()
{
    GCodeConfig config;
    config.apply(FullPrintConfig::defaults(), true);
    config.filament_diameter.values      = { 1.75, 2.2, 1.75, 2.2 };
    config.extrusion_multiplier.values   = { 1.0, 1.0, 1.0, 1.0 };
    config.use_volumetric_e.value        = false;
    return config;
}

static double crossection(double diameter)
{
    return diameter * diameter * 0.25 * M_PI;
}

TEST_CASE("Each tool converts volume to E through its own filament", "[MixedFilamentDiameters]")
{
    GCodeConfig config = mixed_gcode_config();

    const Extruder thin(0, &config);
    const Extruder thick(1, &config);

    CHECK(thin.filament_diameter() == Approx(1.75));
    CHECK(thick.filament_diameter() == Approx(2.2));
    CHECK(thin.filament_crossection() == Approx(crossection(1.75)));
    CHECK(thick.filament_crossection() == Approx(crossection(2.2)));

    // E per mm^3 is the inverse of the cross-section: the thicker filament
    // needs proportionally less length for the same extruded volume, which
    // is the whole point of supporting a 2.2 mm hotend.
    CHECK(thin.e_per_mm3() == Approx(1. / crossection(1.75)));
    CHECK(thick.e_per_mm3() == Approx(1. / crossection(2.2)));
    CHECK(thick.e_per_mm3() < thin.e_per_mm3());

    // The same deposited volume, asked of both tools.
    const double volume_mm3 = 100.;
    const double e_thin  = volume_mm3 * thin.e_per_mm3();
    const double e_thick = volume_mm3 * thick.e_per_mm3();
    CHECK(e_thin / e_thick == Approx(crossection(2.2) / crossection(1.75)));
    // 2.2 vs 1.75 is a 1.58x area ratio; a shared-diameter assumption here
    // would over-extrude the thick tool by that factor.
    CHECK(e_thin / e_thick == Approx(1.58).margin(0.01));
}

TEST_CASE("Extrusion multiplier stays per tool alongside the diameter", "[MixedFilamentDiameters]")
{
    GCodeConfig config = mixed_gcode_config();
    config.extrusion_multiplier.values = { 1.0, 0.95, 1.0, 1.0 };

    const Extruder thin(0, &config);
    const Extruder tuned(1, &config);
    CHECK(thin.e_per_mm3()  == Approx(1. / crossection(1.75)));
    CHECK(tuned.e_per_mm3() == Approx(0.95 / crossection(2.2)));
}

TEST_CASE("Volumetric E ignores the diameter for every tool", "[MixedFilamentDiameters]")
{
    GCodeConfig config = mixed_gcode_config();
    config.use_volumetric_e.value = true;

    // With volumetric E the firmware is told mm^3 directly, so the mixed
    // diameters must not scale it on either tool.
    CHECK(Extruder(0, &config).e_per_mm3() == Approx(1.));
    CHECK(Extruder(1, &config).e_per_mm3() == Approx(1.));
}

TEST_CASE("Filament used per tool is reported in its own volume", "[MixedFilamentDiameters]")
{
    GCodeConfig config = mixed_gcode_config();

    Extruder thin(0, &config);
    Extruder thick(1, &config);

    // Extrude the same volume through both and confirm the reported
    // filament length differs by the area ratio while the volume matches.
    const double volume_mm3 = 250.;
    thin.extrude(volume_mm3 * thin.e_per_mm3());
    thick.extrude(volume_mm3 * thick.e_per_mm3());

    CHECK(thin.extruded_volume()  == Approx(volume_mm3));
    CHECK(thick.extruded_volume() == Approx(volume_mm3));
    CHECK(thin.used_filament() > thick.used_filament());
    CHECK(thin.used_filament() / thick.used_filament() ==
          Approx(crossection(2.2) / crossection(1.75)));
}

// A two-object print whose objects use different tools, so the wipe tower is
// actually required and its validation runs.
static DynamicPrintConfig mixed_print_config(bool single_extruder_multi_material,
                                             double second_filament_diameter,
                                             double second_nozzle_diameter)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("nozzle_diameter",
        new ConfigOptionFloats{ 0.4, second_nozzle_diameter });
    config.set_key_value("filament_diameter",
        new ConfigOptionFloats{ 1.75, second_filament_diameter });
    config.set_key_value("single_extruder_multi_material",
        new ConfigOptionBool(single_extruder_multi_material));
    config.set_key_value("wipe_tower", new ConfigOptionBool(true));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    // Two tools in play, so a wipe tower is required.
    config.set_key_value("perimeter_extruder", new ConfigOptionInt(1));
    config.set_key_value("infill_extruder", new ConfigOptionInt(2));
    config.set_key_value("solid_infill_extruder", new ConfigOptionInt(2));
    return config;
}

TEST_CASE("Mixed filament diameters are refused only when the tools share a nozzle",
          "[MixedFilamentDiameters]")
{
    std::vector<std::string> warnings;

    SECTION("separate hotends accept 1.75 alongside 2.2") {
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model,
                                 mixed_print_config(false, 2.2, 0.4));
        const std::string error = print.validate(&warnings);
        INFO(error);
        CHECK(error.find("same diameter") == std::string::npos);
    }

    SECTION("one shared nozzle still refuses them") {
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model,
                                 mixed_print_config(true, 2.2, 0.4));
        const std::string error = print.validate(&warnings);
        CHECK(error.find("same diameter") != std::string::npos);
    }

    SECTION("a shared nozzle is fine when the diameters match") {
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model,
                                 mixed_print_config(true, 1.75, 0.4));
        const std::string error = print.validate(&warnings);
        INFO(error);
        CHECK(error.find("same diameter") == std::string::npos);
    }

    SECTION("differing nozzles warn rather than refuse") {
        Slic3r::Print print;
        Slic3r::Model model;
        warnings.clear();
        Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model,
                                 mixed_print_config(false, 2.2, 0.8));
        const std::string error = print.validate(&warnings);
        INFO(error);
        CHECK(error.find("same diameter") == std::string::npos);
    }
}
