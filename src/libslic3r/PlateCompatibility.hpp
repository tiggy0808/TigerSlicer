///|/ TigerSlicer Phase N: live plate-to-printer compatibility reporting.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PlateCompatibility_hpp_
#define slic3r_PlateCompatibility_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrinterCapabilityMatch.hpp"

namespace Slic3r {

// What the current plate/project needs from a printer. Optional fields mean
// "not determined", which must never be reported as satisfied.
struct PlateRequirements
{
    PrinterTechnology technology { ptFFF };

    // Bounding box of everything placed, in mm, relative to the bed origin.
    std::optional<double> width_mm;
    std::optional<double> depth_mm;
    std::optional<double> height_mm;

    // Tools the plate actually uses (0-based ids), and the nozzle diameter
    // each one needs.
    std::vector<int>    used_tools;
    std::vector<double> required_nozzle_mm;   // parallel to used_tools

    // Materials the plate expects per used tool, when known.
    std::vector<std::string> required_materials;

    std::optional<double> layer_height_mm;
    std::string           output_format;

    // Firmware macros/capabilities the generated output relies on.
    std::vector<std::string> required_capabilities;
};

// What a specific printer offers. Mirrors PrinterProfileSummary but adds the
// live state a connected machine can report.
struct PrinterState
{
    PrinterProfileSummary profile;

    // Loaded materials per tool, when the connector reports them. An empty
    // optional means unknown - which is different from "none loaded".
    std::optional<std::vector<std::string>> loaded_materials;
    std::optional<std::vector<double>>      installed_nozzles_mm;

    std::optional<double> min_layer_height_mm;
    std::optional<double> max_layer_height_mm;

    std::vector<std::string> supported_capabilities;

    // Whether the printer is currently able to accept a job at all.
    bool ready { true };
    std::optional<uint64_t> free_storage_bytes;
    std::optional<uint64_t> job_size_bytes;
};

enum class CompatibilityState
{
    Pass,
    Warning,  // usable; the user may acknowledge and proceed
    Blocked,  // proven incompatible; upload/start must be refused
};

struct CompatibilityFinding
{
    CompatibilityState state { CompatibilityState::Pass };
    std::string        area;      // e.g. "Build volume", "Tools"
    std::string        detail;    // human-readable explanation
};

struct CompatibilityReport
{
    CompatibilityState state { CompatibilityState::Pass };
    std::vector<CompatibilityFinding> findings;

    // Convenience for the upload/start guard.
    bool blocked() const { return state == CompatibilityState::Blocked; }
};

// Compare the plate against the printer. Unknown data yields a warning, never
// a silent pass: the caller must be able to distinguish "checked and fine"
// from "could not be checked".
CompatibilityReport check_plate_compatibility(const PlateRequirements &plate,
                                              const PrinterState &printer);

} // namespace Slic3r

#endif // slic3r_PlateCompatibility_hpp_
