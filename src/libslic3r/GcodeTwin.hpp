///|/ TigerSlicer Phase 26: digital G-code twin and verifier.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GcodeTwin_hpp_
#define slic3r_GcodeTwin_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// What the twin found. Error is reserved for something that would damage the
// print or the machine; Unknown is its own thing, because a command nobody
// described is not a command known to be safe.
enum class TwinSeverity : uint8_t
{
    Info,
    Warning,
    Error,
};

const char *twin_severity_name(TwinSeverity severity);

TwinSeverity worse_twin(TwinSeverity a, TwinSeverity b);

enum class TwinIssue : uint8_t
{
    ZeroFeedrate,
    ColdExtrusion,
    WrongTool,
    MissingWait,
    BadMode,
    UnsafeZ,
    OutOfRange,
    HeightViolation,
    OffsetViolation,
    StateMismatch,
    UnknownCommand,
    MaterialVolume,
};

const char *twin_issue_name(TwinIssue issue);

struct TwinFinding
{
    TwinIssue    issue { TwinIssue::UnknownCommand };
    TwinSeverity severity { TwinSeverity::Warning };

    // 1-based, as an editor counts them.
    size_t      line { 0 };
    std::string source;
    std::string message;
};

// Distance and extrusion can be in absolute or relative mode independently.
// Unknown is the state before anything declared one, and extruding in it is a
// defect rather than a guess.
enum class PositioningMode : uint8_t
{
    Unknown,
    Absolute,
    Relative,
};

const char *positioning_mode_name(PositioningMode mode);

// Everything the twin owns carries the Twin prefix. These two used to be
// ToolThermalState and MacroContract, which are also the names of unrelated
// structs in ToolchangeCostModel.hpp and ToolchangerPreflight.hpp, in this same
// namespace and with different layouts. Nothing warns: each translation unit
// compiles against whichever definition it saw, the linker merges them, and a
// caller that included both headers reads one layout through the other. Prefer
// a specific name over a natural one for anything at namespace scope here.
struct TwinToolThermalState
{
    // What was commanded, and whether the command waited for it.
    std::optional<double> target_c;
    bool                  waited { false };
    // The last target that was actually waited for, which is the only
    // temperature the machine can be said to have reached.
    std::optional<double> reached_c;
};

// What one tool is declared to be, so the twin can check against something
// rather than assume.
struct TwinTool
{
    int         index { 0 };
    std::string material;

    // Below this, extruding is cold extrusion. Absent means nobody
    // characterized the material, and the check reports that rather than
    // passing.
    std::optional<double> minimum_extrusion_c;

    std::optional<double> nozzle_mm;
    // The tool's offset from the machine origin, if it was measured.
    std::optional<double> offset_x_mm;
    std::optional<double> offset_y_mm;
    std::optional<double> offset_z_mm;
};

// A macro the firmware author declared the meaning of. The twin interprets
// these symbolically and nothing else: an undeclared macro is not guessed at.
struct TwinMacroContract
{
    std::string name;

    // What the macro is declared to do, in terms the twin can check.
    bool               changes_tool { false };
    std::optional<int> selects_tool;
    bool               moves_axes { false };
    bool               changes_temperature { false };
    bool               waits_for_temperature { false };
    bool               changes_position_mode { false };
    // True when the macro leaves the machine somewhere the contract does not
    // describe. The twin then stops claiming to know the state.
    bool leaves_state_undescribed { false };

    std::string description;
};

struct TwinMachine
{
    std::vector<TwinTool>      tools;
    std::vector<TwinMacroContract> macros;

    // The build volume. Absent bounds are not checked, and the twin says so
    // rather than treating an unbounded axis as verified.
    std::optional<double> min_x_mm, max_x_mm;
    std::optional<double> min_y_mm, max_y_mm;
    std::optional<double> min_z_mm, max_z_mm;

    // The lowest z a travel move may cross printed geometry at. Absent means
    // no such limit was declared.
    std::optional<double> safe_travel_z_mm;

    // The largest layer height any tool may print. Absent means nobody said.
    std::optional<double> max_layer_height_mm;

    // Filament cross-section, for turning E millimetres into volume. Absent
    // means the material total cannot be computed and is reported as unknown
    // rather than as zero.
    std::optional<double> filament_diameter_mm;
};

enum class UnknownCommandPolicy : uint8_t
{
    // An undeclared command that could change state is a warning.
    Warn,
    // ... or an error. Which one is a policy decision, not the twin's.
    Error,
};

struct TwinPolicy
{
    UnknownCommandPolicy unknown_state_changing { UnknownCommandPolicy::Warn };

    // Require a wait (M109/M190 or a declared waiting macro) before the first
    // extrusion on a tool.
    bool require_wait_before_extrusion { true };

    double tolerance_mm { 1e-6 };
    size_t max_lines { 5000000 };
};

// The machine state after one line, as the twin believes it to be.
struct TwinState
{
    size_t line { 0 };

    std::optional<int> active_tool;

    std::optional<double> x_mm, y_mm, z_mm;
    std::optional<double> e_mm;
    std::optional<double> feedrate_mm_min;

    PositioningMode xyz_mode { PositioningMode::Unknown };
    PositioningMode e_mode { PositioningMode::Unknown };

    bool retracted { false };

    std::optional<double> fan_fraction;
    std::optional<double> pressure_advance;
    std::optional<double> flow_fraction;

    std::optional<size_t> layer;

    // False once a macro left the state undescribed. Everything after that is
    // reported as unknown rather than tracked as if it were still known.
    bool position_known { true };
};

struct TwinResult
{
    std::vector<TwinFinding> findings;

    // The final state the twin believes the machine is in.
    TwinState final_state;

    size_t lines_read { 0 };
    size_t moves { 0 };
    size_t extrusions { 0 };
    size_t toolchanges { 0 };

    // Total extruded length and volume per tool. Volume is absent when the
    // filament diameter was never declared: it is not computable, not zero.
    std::map<int, double>                extruded_mm;
    std::optional<std::map<int, double>> extruded_mm3;

    // The checkpoints the twin saw, by the operation index in their comment.
    std::vector<size_t> checkpoints;

    // Non-empty when the file could not be interpreted at all.
    std::string error;

    TwinSeverity worst() const;
    bool         acceptable() const;
    size_t       count(TwinSeverity severity) const;
    bool         has(TwinIssue issue) const;
};

// Interpret exported G-code and report what it would actually do.
//
// The twin tracks tool identity, coordinates, positioning modes, feedrate,
// retraction, temperatures and whether they were waited for, fans, offsets,
// pressure advance, flow, layers, checkpoints and extruded material, and
// reports invalid or zero feedrates, cold extrusion, extruding on a tool that
// was never selected, missing waits, extruding before a positioning mode was
// declared, unsafe z, moves outside the declared volume, layer heights and
// tool offsets beyond what was declared, and states that do not match what the
// file itself claims.
//
// Declared macro contracts are interpreted symbolically. An undeclared command
// that could change state is never guessed at: it is reported at whatever
// severity the policy sets, and a macro declared to leave the state
// undescribed stops the twin claiming to know the position at all.
TwinResult interpret_gcode(const std::string &gcode, const TwinMachine &machine,
                           const TwinPolicy &policy);

} // namespace Slic3r

#endif // slic3r_GcodeTwin_hpp_
