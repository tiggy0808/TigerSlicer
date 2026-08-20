///|/ TigerSlicer Phase 26: digital G-code twin and verifier.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GcodeTwin.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Slic3r {

const char *twin_severity_name(TwinSeverity severity)
{
    switch (severity) {
    case TwinSeverity::Info:    return "info";
    case TwinSeverity::Warning: return "warning";
    case TwinSeverity::Error:   return "error";
    }
    return "info";
}

TwinSeverity worse_twin(TwinSeverity a, TwinSeverity b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

const char *twin_issue_name(TwinIssue issue)
{
    switch (issue) {
    case TwinIssue::ZeroFeedrate:    return "zero feedrate";
    case TwinIssue::ColdExtrusion:   return "cold extrusion";
    case TwinIssue::WrongTool:       return "wrong tool";
    case TwinIssue::MissingWait:     return "missing wait";
    case TwinIssue::BadMode:         return "bad mode";
    case TwinIssue::UnsafeZ:         return "unsafe z";
    case TwinIssue::OutOfRange:      return "out of range";
    case TwinIssue::HeightViolation: return "height violation";
    case TwinIssue::OffsetViolation: return "offset violation";
    case TwinIssue::StateMismatch:   return "state mismatch";
    case TwinIssue::UnknownCommand:  return "unknown command";
    case TwinIssue::MaterialVolume:  return "material volume";
    }
    return "unknown command";
}

const char *positioning_mode_name(PositioningMode mode)
{
    switch (mode) {
    case PositioningMode::Unknown:  return "undeclared";
    case PositioningMode::Absolute: return "absolute";
    case PositioningMode::Relative: return "relative";
    }
    return "undeclared";
}

TwinSeverity TwinResult::worst() const
{
    TwinSeverity value = TwinSeverity::Info;
    for (const TwinFinding &finding : findings)
        value = worse_twin(value, finding.severity);
    return value;
}

bool TwinResult::acceptable() const
{
    return error.empty() && worst() != TwinSeverity::Error;
}

size_t TwinResult::count(TwinSeverity severity) const
{
    size_t total = 0;
    for (const TwinFinding &finding : findings)
        if (finding.severity == severity)
            ++total;
    return total;
}

bool TwinResult::has(TwinIssue issue) const
{
    return std::any_of(findings.begin(), findings.end(),
                       [&](const TwinFinding &finding) { return finding.issue == issue; });
}

namespace {

std::string number(double value, int precision = 2)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

std::string upper(std::string value)
{
    for (char &c : value)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::string trim(const std::string &value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    return value.substr(first, last - first);
}

// One parsed line: the word (G1, M104, T2, MACRO_NAME) and its parameters.
struct Parsed
{
    std::string           word;
    std::map<char, double> params;
    // Parameters that were present but not a number, e.g. S in "M117 hello".
    bool malformed_parameter { false };
    std::string comment;
    bool        blank { false };
};

Parsed parse_line(const std::string &raw)
{
    Parsed parsed;

    std::string body = raw;
    const size_t semi = body.find(';');
    if (semi != std::string::npos) {
        parsed.comment = trim(body.substr(semi + 1));
        body           = body.substr(0, semi);
    }
    body = trim(body);
    if (body.empty()) {
        parsed.blank = true;
        return parsed;
    }

    std::istringstream stream(body);
    std::string        token;
    bool               first = true;
    while (stream >> token) {
        if (first) {
            parsed.word = upper(token);
            first       = false;
            continue;
        }
        const char letter = char(std::toupper(static_cast<unsigned char>(token[0])));
        if (token.size() < 2) {
            parsed.params[letter] = 0.;
            continue;
        }
        try {
            size_t     used  = 0;
            const double value = std::stod(token.substr(1), &used);
            if (used + 1 != token.size() || ! std::isfinite(value))
                parsed.malformed_parameter = true;
            else
                parsed.params[letter] = value;
        } catch (...) {
            parsed.malformed_parameter = true;
        }
    }
    return parsed;
}

bool has(const Parsed &parsed, char letter)
{
    return parsed.params.count(letter) > 0;
}

double value(const Parsed &parsed, char letter, double fallback = 0.)
{
    auto found = parsed.params.find(letter);
    return found == parsed.params.end() ? fallback : found->second;
}

const TwinTool *tool_of(const TwinMachine &machine, int index)
{
    for (const TwinTool &tool : machine.tools)
        if (tool.index == index)
            return &tool;
    return nullptr;
}

const TwinMacroContract *macro_of(const TwinMachine &machine, const std::string &word)
{
    for (const TwinMacroContract &macro : machine.macros)
        if (upper(macro.name) == word)
            return &macro;
    return nullptr;
}

// Commands the twin understands well enough not to have to warn about, and
// which do not change anything it tracks.
bool harmless(const std::string &word)
{
    static const char *known[] = { "G4",   "M117", "M400", "M73",  "M75",  "M76",
                                   "M77",  "M84",  "M107", "M105", "M115", "M300" };
    for (const char *entry : known)
        if (word == entry)
            return true;
    return false;
}

// Whether an unrecognized word looks like it could change machine state. A
// comment or a display command cannot; anything else has to be assumed to.
bool could_change_state(const std::string &word)
{
    if (word.empty())
        return false;
    return ! (word == "M117" || word == "M118");
}

} // namespace

TwinResult interpret_gcode(const std::string &gcode, const TwinMachine &machine,
                           const TwinPolicy &policy)
{
    TwinResult result;

    auto report = [&](TwinIssue issue, TwinSeverity severity, size_t line,
                      const std::string &source, std::string message) {
        TwinFinding finding;
        finding.issue    = issue;
        finding.severity = severity;
        finding.line     = line;
        finding.source   = source;
        finding.message  = std::move(message);
        result.findings.push_back(std::move(finding));
    };

    TwinState state;

    // Per-tool thermal state, and whether anything has been extruded on it.
    std::map<int, TwinToolThermalState> thermal;
    std::map<int, bool>             extruded_on;
    std::map<int, bool>             offset_checked;

    // The z the last extrusion happened at. Layer height is the step between
    // two extrusions, not the step a travel happens to take.
    std::optional<double> last_extrusion_z;
    bool                  seen_any_line = false;

    std::istringstream stream(gcode);
    std::string        raw;
    size_t             line_number = 0;

    while (std::getline(stream, raw)) {
        ++line_number;
        if (line_number > policy.max_lines) {
            result.error = "the file is longer than the " +
                           std::to_string(policy.max_lines) +
                           " lines this twin will read, so it was not interpreted; a partial "
                           "interpretation would look like a complete one";
            return result;
        }
        ++result.lines_read;

        const Parsed parsed = parse_line(raw);
        const std::string source = trim(raw);

        // Layer and checkpoint markers live in comments, which is the only
        // place the exporter puts them.
        if (! parsed.comment.empty()) {
            const std::string comment = upper(parsed.comment);
            if (comment.rfind("LAYER:", 0) == 0) {
                try {
                    state.layer = size_t(std::stoul(parsed.comment.substr(6)));
                } catch (...) {
                    report(TwinIssue::StateMismatch, TwinSeverity::Warning, line_number,
                           source,
                           "a layer marker was written that does not carry a layer number, "
                           "so the twin cannot follow the layer count from here");
                }
            } else if (comment.rfind("CHECKPOINT:", 0) == 0) {
                try {
                    result.checkpoints.push_back(
                        size_t(std::stoul(parsed.comment.substr(11))));
                } catch (...) {
                    report(TwinIssue::StateMismatch, TwinSeverity::Warning, line_number,
                           source,
                           "a checkpoint marker was written without an operation number, so "
                           "nothing could resume from it");
                }
            }
        }

        if (parsed.blank)
            continue;
        seen_any_line = true;

        if (parsed.malformed_parameter)
            report(TwinIssue::BadMode, TwinSeverity::Warning, line_number, source,
                   "a parameter on this line is not a number, so what it does is not "
                   "something the twin can claim to know");

        const std::string &word = parsed.word;

        // --- tool selection --------------------------------------------------
        if (word.size() >= 2 && word[0] == 'T' &&
            std::all_of(word.begin() + 1, word.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            int index = -1;
            try {
                size_t used = 0;
                const long parsed_index = std::stol(word.substr(1), &used);
                if (used != word.size() - 1 ||
                    parsed_index > std::numeric_limits<int>::max())
                    throw std::out_of_range("tool index");
                index = int(parsed_index);
            } catch (...) {
                report(TwinIssue::WrongTool, TwinSeverity::Error, line_number, source,
                       "the selected tool number is outside the range TigerSlicer can "
                       "represent, so the command cannot be verified");
                state.active_tool.reset();
                continue;
            }
            if (tool_of(machine, index) == nullptr)
                report(TwinIssue::WrongTool, TwinSeverity::Error, line_number, source,
                       "T" + std::to_string(index) +
                           " is selected but this machine declares no such tool, so nothing "
                           "about what it would do can be checked");
            if (state.active_tool.has_value() && *state.active_tool != index)
                ++result.toolchanges;
            state.active_tool = index;
            continue;
        }

        // --- positioning and units ------------------------------------------
        if (word == "G90") { state.xyz_mode = PositioningMode::Absolute; state.e_mode = state.e_mode == PositioningMode::Unknown ? PositioningMode::Absolute : state.e_mode; continue; }
        if (word == "G91") { state.xyz_mode = PositioningMode::Relative; continue; }
        if (word == "M82") { state.e_mode = PositioningMode::Absolute; continue; }
        if (word == "M83") { state.e_mode = PositioningMode::Relative; continue; }
        if (word == "G92") {
            if (has(parsed, 'E'))
                state.e_mm = value(parsed, 'E');
            if (has(parsed, 'X')) state.x_mm = value(parsed, 'X');
            if (has(parsed, 'Y')) state.y_mm = value(parsed, 'Y');
            if (has(parsed, 'Z')) state.z_mm = value(parsed, 'Z');
            continue;
        }
        if (word == "G28") {
            state.x_mm = state.y_mm = state.z_mm = 0.;
            state.position_known                 = true;
            continue;
        }

        // --- temperature ------------------------------------------------------
        if (word == "M104" || word == "M109") {
            const int tool = has(parsed, 'T') ? int(value(parsed, 'T'))
                                              : state.active_tool.value_or(0);
            TwinToolThermalState &t = thermal[tool];
            if (has(parsed, 'S')) {
                t.target_c = value(parsed, 'S');
                t.waited   = word == "M109";
                if (t.waited)
                    t.reached_c = t.target_c;
            }
            continue;
        }
        if (word == "M140" || word == "M190")
            continue;

        // --- fans, flow, pressure advance -------------------------------------
        if (word == "M106") {
            state.fan_fraction = has(parsed, 'S') ? value(parsed, 'S') / 255. : 1.;
            continue;
        }
        if (word == "M107") { state.fan_fraction = 0.; continue; }
        if (word == "M221") {
            if (has(parsed, 'S'))
                state.flow_fraction = value(parsed, 'S') / 100.;
            continue;
        }
        if (word == "M572" || word == "SET_PRESSURE_ADVANCE") {
            if (has(parsed, 'S'))
                state.pressure_advance = value(parsed, 'S');
            continue;
        }

        // --- motion -----------------------------------------------------------
        if (word == "G0" || word == "G1") {
            ++result.moves;

            const bool extruding =
                has(parsed, 'E') &&
                (state.e_mode == PositioningMode::Relative
                     ? value(parsed, 'E') > 0.
                     : ! state.e_mm.has_value() || value(parsed, 'E') > *state.e_mm);

            if (has(parsed, 'F')) {
                const double feed = value(parsed, 'F');
                if (feed <= 0.)
                    report(TwinIssue::ZeroFeedrate, TwinSeverity::Error, line_number, source,
                           "a feedrate of " + number(feed) +
                               " was commanded; a move with no speed either never finishes or "
                               "runs at whatever the firmware last had");
                else
                    state.feedrate_mm_min = feed;
            }
            if (! state.feedrate_mm_min.has_value() &&
                (has(parsed, 'X') || has(parsed, 'Y') || has(parsed, 'Z') || has(parsed, 'E')))
                report(TwinIssue::ZeroFeedrate, TwinSeverity::Error, line_number, source,
                       "this is the first move and no feedrate has ever been set, so its "
                       "speed is whatever the firmware happened to be left at");

            if (extruding) {
                ++result.extrusions;

                if (! state.active_tool.has_value()) {
                    report(TwinIssue::WrongTool, TwinSeverity::Error, line_number, source,
                           "material is extruded before any tool was selected, so which tool "
                           "does it is whatever the machine was left holding");
                } else {
                    const int             tool = *state.active_tool;
                    const TwinTool       *described = tool_of(machine, tool);
                    TwinToolThermalState     &t         = thermal[tool];

                    if (! t.target_c.has_value()) {
                        report(TwinIssue::ColdExtrusion, TwinSeverity::Error, line_number,
                               source,
                               "T" + std::to_string(tool) +
                                   " extrudes without any temperature ever having been "
                                   "commanded for it");
                    } else if (described == nullptr ||
                               ! described->minimum_extrusion_c.has_value()) {
                        if (! extruded_on[tool])
                            report(TwinIssue::ColdExtrusion, TwinSeverity::Warning,
                                   line_number, source,
                                   "T" + std::to_string(tool) + " extrudes at " +
                                       number(*t.target_c) +
                                       " C, and the temperature its material stops extruding "
                                       "below was never characterized, so this cannot be "
                                       "shown not to be a cold extrusion");
                    } else if (*t.target_c < *described->minimum_extrusion_c) {
                        report(TwinIssue::ColdExtrusion, TwinSeverity::Error, line_number,
                               source,
                               "T" + std::to_string(tool) + " extrudes at " +
                                   number(*t.target_c) + " C, below the " +
                                   number(*described->minimum_extrusion_c) +
                                   " C its material needs");
                    }

                    if (policy.require_wait_before_extrusion && ! extruded_on[tool] &&
                        t.target_c.has_value() && ! t.reached_c.has_value())
                        report(TwinIssue::MissingWait, TwinSeverity::Error, line_number,
                               source,
                               "T" + std::to_string(tool) + " was set to " +
                                   number(*t.target_c) +
                                   " C and extrudes without ever waiting for it; the "
                                   "temperature was commanded, not reached");

                    if (state.e_mode == PositioningMode::Unknown)
                        report(TwinIssue::BadMode, TwinSeverity::Error, line_number, source,
                               "material is extruded before M82 or M83 declared whether E is "
                               "absolute or relative, so how much comes out depends on what "
                               "the firmware defaulted to");

                    const double delta = state.e_mode == PositioningMode::Relative
                                             ? value(parsed, 'E')
                                             : value(parsed, 'E') - state.e_mm.value_or(0.);
                    result.extruded_mm[tool] += delta;
                    extruded_on[tool] = true;

                    if (described != nullptr && ! offset_checked[tool]) {
                        offset_checked[tool] = true;
                        if (! described->offset_x_mm.has_value() ||
                            ! described->offset_y_mm.has_value() ||
                            ! described->offset_z_mm.has_value())
                            report(TwinIssue::OffsetViolation, TwinSeverity::Warning,
                                   line_number, source,
                                   "T" + std::to_string(tool) +
                                       " prints, and its offset from the machine origin was "
                                       "never fully measured, so where it actually puts "
                                       "material is not something the twin can check");
                    }
                }
            }

            if (state.xyz_mode == PositioningMode::Unknown &&
                (has(parsed, 'X') || has(parsed, 'Y') || has(parsed, 'Z')))
                report(TwinIssue::BadMode, TwinSeverity::Error, line_number, source,
                       "an axis is moved before G90 or G91 declared whether coordinates are "
                       "absolute or relative, so where the toolhead goes depends on what the "
                       "firmware defaulted to");

            auto advance = [&](char letter, std::optional<double> &axis) {
                if (! has(parsed, letter))
                    return;
                const double given = value(parsed, letter);
                if (state.xyz_mode == PositioningMode::Relative)
                    axis = axis.value_or(0.) + given;
                else
                    axis = given;
            };
            advance('X', state.x_mm);
            advance('Y', state.y_mm);
            advance('Z', state.z_mm);
            if (has(parsed, 'E'))
                state.e_mm = state.e_mode == PositioningMode::Relative
                                 ? state.e_mm.value_or(0.) + value(parsed, 'E')
                                 : value(parsed, 'E');

            if (has(parsed, 'E') && ! extruding && value(parsed, 'E') != 0.)
                state.retracted = true;
            else if (extruding)
                state.retracted = false;

            if (! state.position_known)
                report(TwinIssue::StateMismatch, TwinSeverity::Warning, line_number, source,
                       "this move follows a macro whose contract does not describe where it "
                       "leaves the machine, so the position it starts from is not known");

            // Build volume.
            auto in_range = [&](char name, const std::optional<double> &axis,
                                const std::optional<double> &low,
                                const std::optional<double> &high) {
                if (! axis.has_value())
                    return;
                if (low.has_value() && *axis < *low - policy.tolerance_mm)
                    report(TwinIssue::OutOfRange, TwinSeverity::Error, line_number, source,
                           std::string(1, name) + " goes to " + number(*axis) +
                               " mm, below the declared minimum of " + number(*low) + " mm");
                if (high.has_value() && *axis > *high + policy.tolerance_mm)
                    report(TwinIssue::OutOfRange, TwinSeverity::Error, line_number, source,
                           std::string(1, name) + " goes to " + number(*axis) +
                               " mm, past the declared maximum of " + number(*high) + " mm");
            };
            in_range('X', state.x_mm, machine.min_x_mm, machine.max_x_mm);
            in_range('Y', state.y_mm, machine.min_y_mm, machine.max_y_mm);
            in_range('Z', state.z_mm, machine.min_z_mm, machine.max_z_mm);

            // A travel that crosses at less than the declared safe height.
            // Only once something has been printed: before that there is
            // nothing on the bed to hit, and the approach to the first layer
            // legitimately happens low.
            if (! extruding && last_extrusion_z.has_value() &&
                machine.safe_travel_z_mm.has_value() && state.z_mm.has_value() &&
                (has(parsed, 'X') || has(parsed, 'Y')) &&
                *state.z_mm < *machine.safe_travel_z_mm - policy.tolerance_mm)
                report(TwinIssue::UnsafeZ, TwinSeverity::Error, line_number, source,
                       "a travel move crosses at " + number(*state.z_mm) +
                           " mm, below the " + number(*machine.safe_travel_z_mm) +
                           " mm this machine declares safe to travel at");

            // Layer height: the step between the z one extrusion happened at
            // and the z the next one happens at.
            if (extruding && state.z_mm.has_value()) {
                if (last_extrusion_z.has_value() &&
                    *state.z_mm > *last_extrusion_z + policy.tolerance_mm) {
                    const double height = *state.z_mm - *last_extrusion_z;
                    if (machine.max_layer_height_mm.has_value() &&
                        height > *machine.max_layer_height_mm + policy.tolerance_mm)
                        report(TwinIssue::HeightViolation, TwinSeverity::Error, line_number,
                               source,
                               "this layer is " + number(height) + " mm thick, past the " +
                                   number(*machine.max_layer_height_mm) +
                                   " mm this machine declares it can print");
                }
                last_extrusion_z = *state.z_mm;
            }
            continue;
        }

        if (word == "G2" || word == "G3") {
            ++result.moves;
            if (! state.feedrate_mm_min.has_value())
                report(TwinIssue::ZeroFeedrate, TwinSeverity::Error, line_number, source,
                       "an arc move is commanded before any feedrate was set");
            if (has(parsed, 'F') && value(parsed, 'F') <= 0.)
                report(TwinIssue::ZeroFeedrate, TwinSeverity::Error, line_number, source,
                       "an arc move is commanded with a feedrate of " +
                           number(value(parsed, 'F')));
            continue;
        }

        if (word == "G10") { state.retracted = true; continue; }
        if (word == "G11") { state.retracted = false; continue; }

        // --- declared macros ---------------------------------------------------
        if (const TwinMacroContract *macro = macro_of(machine, word)) {
            if (macro->changes_tool) {
                if (macro->selects_tool.has_value()) {
                    if (state.active_tool.has_value() &&
                        *state.active_tool != *macro->selects_tool)
                        ++result.toolchanges;
                    state.active_tool = *macro->selects_tool;
                } else {
                    report(TwinIssue::WrongTool, TwinSeverity::Warning, line_number, source,
                           macro->name +
                               " is declared to change the tool but not which tool it "
                               "selects, so which one is held afterwards is not known");
                    state.active_tool.reset();
                }
            }
            if (macro->waits_for_temperature && state.active_tool.has_value()) {
                TwinToolThermalState &t = thermal[*state.active_tool];
                if (t.target_c.has_value())
                    t.reached_c = t.target_c;
            }
            if (macro->changes_position_mode) {
                state.xyz_mode = PositioningMode::Unknown;
                state.e_mode   = PositioningMode::Unknown;
            }
            if (macro->leaves_state_undescribed) {
                state.position_known = false;
                state.x_mm.reset();
                state.y_mm.reset();
                state.z_mm.reset();
                report(TwinIssue::StateMismatch, TwinSeverity::Warning, line_number, source,
                       macro->name +
                           " is declared not to describe where it leaves the machine, so "
                           "everything after it is unverified rather than assumed unchanged");
            } else if (macro->moves_axes) {
                state.position_known = false;
                report(TwinIssue::StateMismatch, TwinSeverity::Info, line_number, source,
                       macro->name + " is declared to move the axes; the twin follows its "
                                     "contract, not its body");
            }
            continue;
        }

        if (harmless(word))
            continue;

        // --- anything else -----------------------------------------------------
        if (could_change_state(word))
            report(TwinIssue::UnknownCommand,
                   policy.unknown_state_changing == UnknownCommandPolicy::Error
                       ? TwinSeverity::Error
                       : TwinSeverity::Warning,
                   line_number, source,
                   word +
                       " is not a command this twin understands and no contract declares what "
                       "it does; what it changes is unknown, and pretending otherwise is how "
                       "a verifier passes a file it never checked");
    }

    if (! seen_any_line) {
        result.error = "there is nothing in this file to interpret";
        return result;
    }

    // --- material volume ---------------------------------------------------
    if (machine.filament_diameter_mm.has_value() && *machine.filament_diameter_mm > 0.) {
        const double radius = *machine.filament_diameter_mm / 2.;
        const double area   = 3.14159265358979323846 * radius * radius;
        std::map<int, double> volumes;
        for (const auto &entry : result.extruded_mm)
            volumes[entry.first] = entry.second * area;
        result.extruded_mm3 = volumes;
    } else if (! result.extruded_mm.empty()) {
        report(TwinIssue::MaterialVolume, TwinSeverity::Warning, 0, std::string {},
               "the filament diameter was never declared, so how much material this file "
               "uses cannot be computed; it is unknown, not zero");
    }

    result.final_state = state;
    return result;
}

} // namespace Slic3r
