///|/ TigerSlicer Phase N: loaded tool and material synchronization.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ToolStateSync_hpp_
#define slic3r_ToolStateSync_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// How we came to believe a piece of tool state. Keeping these apart is the
// whole point: a filament colour typed into a profile must never be presented
// as though a sensor reported it.
enum class StateProvenance
{
    Sensed,      // the printer actively reported it
    Configured,  // read from the printer's own configuration
    Assumed,     // the user told TigerSlicer
    Unknown,     // nobody has said
};

template<class T>
struct Attributed
{
    std::optional<T> value;
    StateProvenance  provenance { StateProvenance::Unknown };

    bool known() const { return value.has_value() && provenance != StateProvenance::Unknown; }
};

// State of one tool as the printer presents it.
struct ToolState
{
    int index { 0 };

    Attributed<double>      nozzle_mm;
    Attributed<std::string> material;
    Attributed<std::string> color;          // display colour, e.g. "#FF00AA"
    Attributed<double>      remaining_g;
    Attributed<bool>        temperature_capable;

    // A tool the printer reports as faulty or absent cannot be used.
    Attributed<bool> available;

    std::string calibration_id;
};

// What the sliced plate needs from each tool.
struct ToolRequirement
{
    int         index { 0 };
    std::optional<double>      nozzle_mm;
    std::optional<std::string> material;
    std::optional<std::string> color;
    std::optional<double>      needed_g;
    std::string calibration_id;
};

enum class MismatchSeverity
{
    Info,      // differs, but harmless (e.g. display colour only)
    Warning,   // proceed at the user's discretion
    Blocking,  // would produce a wrong or failed print
};

struct ToolMismatch
{
    int              tool { 0 };
    std::string      field;
    MismatchSeverity severity { MismatchSeverity::Warning };
    std::string      detail;
    // Provenance of the printer-side value that produced this finding, so the
    // UI can say how confident the comparison is.
    StateProvenance  provenance { StateProvenance::Unknown };
};

struct ToolSyncReport
{
    std::vector<ToolMismatch> mismatches;
    bool blocked() const;
    bool has_warnings() const;
};

// Compare what the plate needs against what the printer reports.
//
// Unknown state never silently passes: it produces a finding that says the
// value could not be verified, so the user can decide rather than being told
// everything is fine.
ToolSyncReport compare_tool_state(const std::vector<ToolRequirement> &required,
                                  const std::vector<ToolState> &actual);

const char *to_string(StateProvenance provenance);

} // namespace Slic3r

#endif // slic3r_ToolStateSync_hpp_
