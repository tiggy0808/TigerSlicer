#include "PrusaFirmware.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace Slic3r::PrusaFirmware {

namespace {

// The firmware's preset filaments (PresetFilamentType in its filament.hpp). PPS and PPA need the high temperature hotend.
constexpr std::array<std::string_view, 14> kPresetNames = {"PLA", "PETG", "ASA", "PC",  "PVB", "ABS", "HIPS",
                                                           "PP",  "FLEX", "PA",  "PPS", "PPA", "PVA", "BVOH"};
constexpr std::string_view                 kFlexName    = "FLEX";
constexpr std::string_view                 kNylonName   = "PA";

bool is_preset(std::string_view name) { return std::find(kPresetNames.begin(), kPresetNames.end(), name) != kPresetNames.end(); }

bool starts_with(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

// PA6, PA11, PA12, PA612, PAHT: the nylons.
bool is_nylon_grade(std::string_view base)
{
    if (!starts_with(base, kNylonName) || base.size() == kNylonName.size())
        return false;
    const std::string_view grade = base.substr(kNylonName.size());
    return grade == "HT" || std::all_of(grade.begin(), grade.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

} // namespace

bool is_prusa_start_gcode(const std::string &machine_start_gcode) { return machine_start_gcode.find("M862.3") != std::string::npos; }

std::string filament_name(const std::string &filament_type)
{
    const std::string_view type = filament_type;
    if (is_preset(type))
        return filament_type;
    if (starts_with(type, "TPU") || starts_with(type, "TPE"))
        return std::string(kFlexName);
    // Without what it is filled or blended with: PPA-CF, PETG-GF, PLA-AERO, PC-ABS.
    const std::string_view base = type.substr(0, type.find('-'));
    if (is_preset(base))
        return std::string(base);
    if (is_nylon_grade(base))
        return std::string(kNylonName);
    return filament_type;
}

std::string filament_type_line(const std::string &machine_start_gcode, const std::vector<std::string> &filament_types)
{
    if (!is_prusa_start_gcode(machine_start_gcode))
        return {};
    std::string names;
    bool        changed = false;
    for (const std::string &type : filament_types) {
        const std::string name = filament_name(type);
        changed                = changed || name != type;
        if (!names.empty())
            names += ';';
        names += name;
    }
    return changed ? "; filament_type = " + names + "\n" : std::string();
}

} // namespace Slic3r::PrusaFirmware
