// TigerSlicer: what Prusa's firmware (MK3.5, MK4, MINI, XL, CORE One) reads from a G-code file.
#ifndef slic3r_GCode_PrusaFirmware_hpp_
#define slic3r_GCode_PrusaFirmware_hpp_

#include <string>
#include <vector>

namespace Slic3r::PrusaFirmware {

// Whether the start G-code is for Prusa's firmware: it checks the printer model with M862.3.
bool is_prusa_start_gcode(const std::string &machine_start_gcode);

// The firmware's name for a filament type. Before a print it compares the G-code's filament_type to the name of the
// loaded filament, letter for letter, and warns of a wrong filament when they differ. It has no names for filled or
// numbered types: "PPA-CF" is its "PPA", "PA6-GF" its "PA" and "TPU" its "FLEX". A type it has no name for is returned
// unchanged, for a filament the user added to the printer under that name.
std::string filament_name(const std::string &filament_type);

// The "; filament_type = ..." line for the end of the G-code, where the firmware reads it after the one of the
// configuration block. Empty when the start G-code isn't for Prusa's firmware or every type already has its
// firmware name.
std::string filament_type_line(const std::string &machine_start_gcode, const std::vector<std::string> &filament_types);

} // namespace Slic3r::PrusaFirmware

#endif // slic3r_GCode_PrusaFirmware_hpp_
