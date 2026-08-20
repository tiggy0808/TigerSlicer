///|/ TigerSlicer Phase 2: toolchanger preflight validation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ToolchangerPreflight_hpp_
#define slic3r_ToolchangerPreflight_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/MultiRateLayerSchedule.hpp"

namespace Slic3r {

enum class PreflightSeverity
{
    Warning,   // recorded in the manifest; export may proceed
    Error,     // blocks export
};

// A finding always names what is responsible, so the user can go and fix it
// rather than hunting through settings.
struct PreflightFinding
{
    PreflightSeverity severity { PreflightSeverity::Warning };
    std::string       code;        // stable identifier for tests/manifests
    std::string       message;
    // Responsible entity, as far as it is known.
    std::string       object;
    std::string       region;
    std::string       feature;
    std::optional<int> tool;
    std::string       setting;
};

// Per-tool configuration as it will actually be used.
struct ToolPreflightState
{
    int    tool { 0 };
    double nozzle_diameter_mm { 0. };
    double min_layer_height_mm { 0. };
    double max_layer_height_mm { 0. };
    // Layer height this tool will actually print at.
    double actual_layer_height_mm { 0. };
    double extrusion_width_mm { 0. };
    double volumetric_limit_mm3_s { 0. };
    double temperature_c { 0. };

    std::string filament_name;
    bool        filament_selected { true };

    // Offsets and capabilities declared by the printer profile.
    bool        offset_declared { true };
    std::vector<std::string> capabilities;
};

// Custom G-code that will be emitted, keyed by the contract it fulfils.
struct MacroContract
{
    std::string name;      // e.g. "toolchange_gcode"
    std::string gcode;
    // Capabilities this macro requires the printer to declare.
    std::vector<std::string> required_capabilities;
};

struct PreflightInputs
{
    std::vector<ToolPreflightState> tools;
    // Tools the sliced plate actually uses.
    std::vector<int> used_tools;
    std::vector<MacroContract> macros;
    // Smallest painted detail the plate contains, per used tool, if measured.
    std::vector<std::pair<int, double>> smallest_detail_mm;
};

struct PreflightReport
{
    std::vector<PreflightFinding> findings;

    bool blocked() const;
    std::vector<PreflightFinding> errors() const;
    std::vector<PreflightFinding> warnings() const;
    // Human-readable report suitable for export alongside the G-code.
    std::string to_text() const;
};

// Validate everything that must hold before a toolchanger job may be exported.
PreflightReport run_toolchanger_preflight(const PreflightInputs &inputs);

// Scan a custom G-code block for feedrates that would stall or fault the
// machine. Returns the offending F-word values (e.g. 0 from "G1 X10 F0").
std::vector<double> find_invalid_feedrates(const std::string &gcode);

} // namespace Slic3r

#endif // slic3r_ToolchangerPreflight_hpp_
