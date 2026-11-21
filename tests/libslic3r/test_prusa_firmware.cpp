#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/PrusaFirmware.hpp"

using namespace Slic3r;

TEST_CASE("Filament types get the names Prusa's firmware has for them", "[PrusaFirmware]")
{
    const auto [type, name] = GENERATE(table<std::string, std::string>({
        // The firmware's own names stay.
        {"PLA", "PLA"}, {"PETG", "PETG"}, {"PA", "PA"}, {"PC", "PC"}, {"PPS", "PPS"}, {"FLEX", "FLEX"}, {"PVA", "PVA"},
        // Filled and blended types are their base polymer.
        {"PPA-CF", "PPA"}, {"PPA-GF", "PPA"}, {"PPS-CF", "PPS"}, {"PETG-CF", "PETG"}, {"PLA-AERO", "PLA"}, {"ABS-GF", "ABS"},
        {"PC-ABS", "PC"}, {"PC-CF", "PC"}, {"PP-GF", "PP"},
        // Every nylon is PA.
        {"PA-CF", "PA"}, {"PA6", "PA"}, {"PA6-CF", "PA"}, {"PA11-CF", "PA"}, {"PA12-GF", "PA"}, {"PAHT-CF", "PA"},
        // Flexibles are FLEX.
        {"TPU", "FLEX"}, {"TPU-AMS", "FLEX"}, {"TPE", "FLEX"},
        // No firmware name: unchanged, for a filament added to the printer under that name.
        {"PCTG", "PCTG"}, {"PEEK-CF", "PEEK-CF"}, {"PET-CF", "PET-CF"}, {"PPSU", "PPSU"}, {"TPI", "TPI"}, {"PE-CF", "PE-CF"},
        {"", ""},
    }));
    CAPTURE(type);
    CHECK(PrusaFirmware::filament_name(type) == name);
}

TEST_CASE("The firmware's filament type line is only written when it is needed", "[PrusaFirmware]")
{
    const std::string prusa = "M17\nM862.3 P \"COREONEL\" ; printer model check\nG28\n";
    const std::string other = "G28\nG29\n";

    CHECK(PrusaFirmware::is_prusa_start_gcode(prusa));
    CHECK(!PrusaFirmware::is_prusa_start_gcode(other));

    CHECK(PrusaFirmware::filament_type_line(prusa, {"PPA-CF"}) == "; filament_type = PPA\n");
    // One name per filament, in the project's order, as the firmware assigns them to its tools.
    CHECK(PrusaFirmware::filament_type_line(prusa, {"PLA", "PA6-CF", "TPU"}) == "; filament_type = PLA;PA;FLEX\n");
    // Nothing to rename.
    CHECK(PrusaFirmware::filament_type_line(prusa, {"PLA", "PETG"}).empty());
    CHECK(PrusaFirmware::filament_type_line(prusa, {}).empty());
    // Other firmware reads the type as it is.
    CHECK(PrusaFirmware::filament_type_line(other, {"PPA-CF"}).empty());
}
