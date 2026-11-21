#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

#include <boost/filesystem.hpp>

#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// The start G-code of the shipped Prusa machine profile `name`.
static std::string shipped_prusa_start_gcode(const std::string &name)
{
    const std::string path = std::string(PROFILES_DIR) + "/Prusa/machine/" + name + ".json";
    if (!boost::filesystem::exists(path))
        SKIP("shipped profile not present in this checkout: " << path);
    DynamicPrintConfig                 config;
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    config.load_from_json(path, ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
    INFO("profile: " << path << (reason.empty() ? "" : ("  load reason: " + reason)));
    REQUIRE(config.has("machine_start_gcode"));
    return config.opt_string("machine_start_gcode");
}

// A 20 mm cube sliced for the CORE One L with one filament of `type`, printed at `temperature` on the first layer.
static std::string slice_for_core_one_l(const std::string &type, int temperature)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("machine_start_gcode", new ConfigOptionString(shipped_prusa_start_gcode("Prusa CORE One L HF 0.4 nozzle")));
    config.set_key_value("filament_type", new ConfigOptionStrings({type}));
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts({temperature}));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({temperature}));
    config.set_deserialize_strict({{"gcode_flavor", "marlin2"}});
    return slice({make_cube(20, 20, 20)}, config);
}

static bool has_text(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

TEST_CASE("The CORE One L levels the bed at a nozzle temperature for the filament type", "[PrusaStartGCode][Profiles]")
{
    // The firmware's and PrusaSlicer's rules: 170 by default, 25 below the print for nylons and PC, 210 for flexibles,
    // and for the high temperature materials 40 below, 310 at most, with PEEK at 330.
    const auto [type, temperature, standby] = GENERATE(table<std::string, int, int>({
        {"PLA", 215, 170},    {"PETG-CF", 250, 170}, {"PCTG", 250, 170},  {"ABS", 255, 170},
        {"PA", 285, 260},     {"PA-CF", 290, 265},   {"PA6-GF", 280, 255}, {"PAHT-CF", 300, 275}, {"PA12-CF", 270, 245},
        {"PC", 275, 250},     {"PC-CF", 280, 255},
        {"PPA-CF", 310, 270}, {"PPA-GF", 300, 260},  {"PPS", 320, 280},   {"PPS-CF", 360, 310},  {"PEI-1010", 400, 310},
        {"PEEK", 420, 330},   {"PEEK-CF", 400, 330}, {"PEKK", 380, 310},
        {"TPU", 230, 210},    {"FLEX", 230, 210},
    }));
    CAPTURE(type, temperature);
    const std::string gcode = slice_for_core_one_l(type, temperature);
    CHECK(has_text(gcode, "M109 R" + std::to_string(standby) + " ; wait for temp\n"));
    CHECK(has_text(gcode, "M109 R" + std::to_string(standby) + " ; wait for MBL temp\n"));
    // Then it heats to the print temperature for the purge line.
    CHECK(has_text(gcode, "M109 S" + std::to_string(temperature) + "\n"));
}

TEST_CASE("The CORE One L retracts a flexible filament further before cleaning the nozzle", "[PrusaStartGCode][Profiles]")
{
    CHECK(has_text(slice_for_core_one_l("TPU", 230), "G1 E-4 F2400 ; retraction\n"));
    CHECK(has_text(slice_for_core_one_l("PLA", 215), "G1 E-2 F2400 ; retraction\n"));
}

TEST_CASE("G-code for Prusa's firmware ends with the firmware's filament names", "[PrusaStartGCode][Profiles]")
{
    // The firmware reads the last filament_type of the file.
    auto last_filament_type = [](const std::string &gcode) {
        const size_t pos = gcode.rfind("; filament_type = ");
        REQUIRE(pos != std::string::npos);
        return gcode.substr(pos, gcode.find('\n', pos) - pos);
    };
    CHECK(last_filament_type(slice_for_core_one_l("PPA-CF", 310)) == "; filament_type = PPA");
    CHECK(last_filament_type(slice_for_core_one_l("TPU", 230)) == "; filament_type = FLEX");
    // The configuration block keeps the slicer's type.
    CHECK(has_text(slice_for_core_one_l("PPA-CF", 310), "; filament_type = PPA-CF\n"));
    CHECK(last_filament_type(slice_for_core_one_l("PLA", 215)) == "; filament_type = PLA");

    // Other firmware gets the type as it is.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_type", new ConfigOptionStrings({"PPA-CF"}));
    CHECK(last_filament_type(slice({make_cube(20, 20, 20)}, config)) == "; filament_type = PPA-CF");
}
