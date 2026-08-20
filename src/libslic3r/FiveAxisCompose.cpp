///|/ TigerSlicer five-axis G-code composition. See FiveAxisCompose.hpp.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FiveAxisCompose.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>

namespace Slic3r {

namespace {

struct Words
{
    std::map<char, double> value;
    std::map<char, std::string> text;   // as written, so E/F pass through
    bool has(char axis) const { return value.find(axis) != value.end(); }
};

// Split "G1 X10.5 Y-2 E.04 F1200" into its words. Only the letters that
// matter here are collected.
Words parse_words(const std::string &code)
{
    Words words;
    for (size_t i = 0; i < code.size(); ++i) {
        const char axis = char(std::toupper(static_cast<unsigned char>(code[i])));
        if (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'E' && axis != 'F')
            continue;
        // Only a word start counts: the X of "MAX" is not an axis.
        if (i > 0 && !std::isspace(static_cast<unsigned char>(code[i - 1])))
            continue;
        size_t j = i + 1;
        const size_t start = j;
        if (j < code.size() && (code[j] == '-' || code[j] == '+'))
            ++j;
        bool digits = false;
        while (j < code.size() && (std::isdigit(static_cast<unsigned char>(code[j])) || code[j] == '.')) {
            digits = digits || std::isdigit(static_cast<unsigned char>(code[j]));
            ++j;
        }
        if (!digits)
            continue;
        const std::string text = code.substr(start, j - start);
        words.value[axis] = std::atof(text.c_str());
        words.text[axis] = text;
        i = j - 1;
    }
    return words;
}

std::string first_token(const std::string &code)
{
    size_t begin = code.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return {};
    size_t end = code.find_first_of(" \t", begin);
    return code.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string format_coord(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

std::string format_number(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

} // namespace

FiveAxisComposeResult five_axis_compose(
    const std::vector<FiveAxisSegmentProgram> &segments,
    const FiveAxisMachine                     &machine,
    const FiveAxisComposeOptions              &options)
{
    FiveAxisComposeResult result;
    if (segments.empty()) {
        result.error = "A five-axis print needs at least one segment.";
        return result;
    }

    std::ostringstream out;
    const double table_radius = machine.table_diameter * 0.5;

    for (size_t k = 0; k < segments.size(); ++k) {
        const FiveAxisSegmentProgram &program = segments[k];
        const FiveAxisSegment &segment = program.segment;

        if (const std::string bad = five_axis_validate_segment(segment, machine); !bad.empty()) {
            char prefix[64];
            std::snprintf(prefix, sizeof(prefix), "Segment %zu: ", k);
            result.error = prefix + bad;
            return result;
        }

        // The opening segment prints as sliced when it is upright: its
        // coordinates are already the machine's. Anything else is mapped.
        const bool upright = k == 0 && std::abs(segment.theta) < 1e-9;

        // A Stewart bed can walk into a tilt instead of jumping to it: the
        // first layers of a tilted segment following a flat one are printed
        // with the tilt ramped by transition_step per layer, hinged at the
        // segment origin. Each layer maps through its own rotation, so Z
        // follows the changing angle and the boundary becomes a small wedge
        // of slanted layers instead of one reorientation.
        const bool fanned = !upright && k > 0 &&
            machine.kinematics == FiveAxisKinematics::StewartPlatform &&
            machine.transition_step > 1e-9 && segment.theta > 1e-9 &&
            std::abs(segments[k - 1].segment.theta) < 1e-9;

        double   theta_cur = fanned ? 0. : segment.theta;
        Matrix3d rotation  = five_axis_segment_rotation(
            theta_cur, segment.phi, machine.kinematics);
        double   sin_theta = std::sin(theta_cur * M_PI / 180.);
        size_t   fan_layers_seen = 0;

        // Where the toolhead last was, in machine coordinates - the position
        // a pose template's {X} {Y} {Z} speak of.
        Vec3d last_machine(machine.park_x, machine.park_y, machine.z_clearance);
        // Absolute E of the last move naming E, so a mid-segment tilt step can
        // retract and prime without a G92 that would break the segment's own
        // absolute extrusion words.
        double last_e = 0.;

        // A tilted opening segment has nothing to lift clear of, so the table
        // is brought to its build direction only once the start G-code has
        // homed and primed on a level table - held until the first mapped move.
        std::string pending_orient;
        if (k == 0 && !upright) {
            std::string warning;
            const Vec3d at(machine.park_x, machine.park_y, machine.z_clearance);
            const std::string move = five_axis_reorient_move(segment, machine, &warning, &at);
            if (move.empty()) {
                result.error = "Segment 0 cannot be reoriented on this machine.";
                return result;
            }
            if (!warning.empty())
                result.warnings.push_back(warning);
            pending_orient = "; five-axis: orient the table for segment 0\n" + move + "\n";
        }

        if (k > 0) {
            // Join the previous segment to this one: pull the filament back,
            // lift clear of what is already printed, move away from the
            // table, then reorient. A fanned segment starts on the still-level
            // table, so its join keeps the retract and lift but neither parks
            // nor reorients; the tilt arrives layer by layer below.
            std::string move;
            if (!fanned) {
                std::string warning;
                const Vec3d at(machine.park_x, machine.park_y,
                               result.max_z + machine.z_clearance);
                move = five_axis_reorient_move(segment, machine, &warning, &at);
                if (move.empty()) {
                    char prefix[64];
                    std::snprintf(prefix, sizeof(prefix), "Segment %zu", k);
                    result.error = std::string(prefix) + " cannot be reoriented on this machine.";
                    return result;
                }
                if (!warning.empty())
                    result.warnings.push_back(warning);
            }

            out << (fanned ? "; five-axis: slanted transition into segment "
                           : "; five-axis: reorient for segment ") << k << "\n";
            if (!options.relative_e)
                out << "G92 E0 ; absolute E rebase\n";
            out << "G1 E-1 F" << options.retract_feedrate << " ; retract before reorienting\n";
            out << "G0 Z" << format_coord(result.max_z + machine.z_clearance)
                << " F" << options.travel_feedrate << " ; lift clear\n";
            if (!fanned) {
                out << "G0 X" << format_coord(machine.park_x)
                    << " Y" << format_coord(machine.park_y)
                    << " F" << options.travel_feedrate << " ; park\n";
                out << move << "\n";
            }
            // Restore this block's retract and the previous segment's own,
            // so the next segment starts primed.
            out << "G1 E" << format_number(1. + options.retract_length)
                << " F" << options.deretract_feedrate << " ; prime\n";
        }

        // Modal position in the segment's local frame. A move that does not
        // name every axis still has a full position once one has been seen.
        bool have[3] = { false, false, false };
        double pos[3] = { 0., 0., 0. };
        // The slicer's own custom G-code blocks - start, end, layer change -
        // are written in the machine's frame and are left exactly as they are.
        bool in_custom = false;

        std::istringstream in(program.gcode);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            const size_t semi = line.find(';');
            const std::string code = semi == std::string::npos ? line : line.substr(0, semi);
            const std::string comment = semi == std::string::npos ? std::string() : line.substr(semi);
            const std::string cmd = first_token(code);

            if (upright) {       // an upright segment 0 prints as sliced
                out << line << "\n";
                continue;
            }

            if (boost::starts_with(line, ";TYPE:")) {
                const bool custom = line == ";TYPE:Custom";
                if (in_custom && !custom) {
                    // Leaving machine-frame G-code: whatever position it left
                    // behind says nothing about this segment's own frame.
                    have[0] = have[1] = have[2] = false;
                }
                in_custom = custom;
                out << line << "\n";
                continue;
            }
            if (in_custom) {
                out << line << "\n";
                continue;
            }

            // A fanned segment tilts a step further at each layer boundary,
            // until the target angle is reached. The first marker precedes the
            // first layer, which prints level on the still-level table.
            if (fanned && boost::starts_with(line, ";LAYER_CHANGE")) {
                ++fan_layers_seen;
                const double theta_next = std::min(
                    double(fan_layers_seen - 1) * machine.transition_step, segment.theta);
                out << line << "\n";
                if (theta_next > theta_cur + 1e-9) {
                    FiveAxisSegment step_pose = segment;
                    step_pose.theta = theta_next;
                    Vec3d at = last_machine;
                    at.z() = result.max_z + machine.z_clearance;
                    std::string warning;
                    const std::string move = five_axis_reorient_move(step_pose, machine, &warning, &at);
                    if (move.empty()) {
                        char buffer[224];
                        std::snprintf(buffer, sizeof(buffer),
                            "Segment %zu: the transition pose at %g deg cannot be reached on this "
                            "machine. Lower the transition step or set it to zero.",
                            k, theta_next);
                        result.error = buffer;
                        return result;
                    }
                    // One yaw note for the whole ramp is enough; it peaks at
                    // the target angle.
                    if (!warning.empty() && std::abs(theta_next - segment.theta) < 1e-9)
                        result.warnings.push_back(warning);
                    out << "; five-axis: transition tilt " << format_number(theta_next) << " deg\n";
                    out << "G1 E" << (options.relative_e ? "-1" : format_number(last_e - 1.))
                        << " F" << options.retract_feedrate << " ; retract for tilt step\n";
                    out << "G0 Z" << format_coord(at.z())
                        << " F" << options.travel_feedrate << " ; lift clear\n";
                    out << move << "\n";
                    out << "G1 E" << (options.relative_e ? "1" : format_number(last_e))
                        << " F" << options.deretract_feedrate << " ; prime\n";
                    theta_cur  = theta_next;
                    rotation   = five_axis_segment_rotation(theta_cur, segment.phi, machine.kinematics);
                    sin_theta  = std::sin(theta_cur * M_PI / 180.);
                }
                continue;
            }

            if (cmd == "G2" || cmd == "G3") {
                char buffer[192];
                std::snprintf(buffer, sizeof(buffer),
                    "Segment %zu contains an arc move (%s), which cannot be rotated into the "
                    "machine frame. Slice this segment with arc fitting disabled.",
                    k, cmd.c_str());
                result.error = buffer;
                return result;
            }
            if (cmd == "G91") {
                char buffer[160];
                std::snprintf(buffer, sizeof(buffer),
                    "Segment %zu switches to relative positioning (G91), which cannot be "
                    "mapped into the machine frame.", k);
                result.error = buffer;
                return result;
            }
            if (cmd != "G0" && cmd != "G1") {
                if (cmd == "G92") {
                    const Words w = parse_words(code);
                    if (w.has('X') || w.has('Y') || w.has('Z')) {
                        char buffer[160];
                        std::snprintf(buffer, sizeof(buffer),
                            "Segment %zu sets a position with G92 X/Y/Z, which would break the "
                            "mapping into the machine frame.", k);
                        result.error = buffer;
                        return result;
                    }
                }
                out << line << "\n";
                continue;
            }

            const Words words = parse_words(code);
            if (words.has('E'))
                last_e = words.value.at('E');
            if (!words.has('X') && !words.has('Y') && !words.has('Z')) {
                out << line << "\n";     // retract, deretract, feedrate only
                continue;
            }

            if (words.has('X')) { pos[0] = words.value.at('X') - program.bed_center.x() - program.local_offset.x(); have[0] = true; }
            if (words.has('Y')) { pos[1] = words.value.at('Y') - program.bed_center.y() - program.local_offset.y(); have[1] = true; }
            if (words.has('Z')) { pos[2] = words.value.at('Z') - program.local_offset.z(); have[2] = true; }

            std::string tail;
            for (char axis : { 'E', 'F' })
                if (words.has(axis))
                    tail += std::string(" ") + axis + words.text.at(axis);

            if (!have[0] || !have[1] || !have[2]) {
                // A rotated coordinate needs all three local components; until
                // then only the extrusion and feedrate carry over.
                if (!tail.empty())
                    out << cmd << tail << comment << "\n";
                continue;
            }

            const Vec3d machine_point = five_axis_local_to_machine(
                Vec3d(pos[0], pos[1], pos[2]), rotation, segment.origin, machine.pivot_offset_z);

            // Near the bed a tilted table brings the nozzle body close to it;
            // require the in-plane standoff to keep its distance.
            if (sin_theta > 1e-9 && machine_point.z() <= machine.min_bed_clearance) {
                const double standoff = std::abs(pos[2]) / sin_theta;
                if (standoff < machine.min_bed_clearance) {
                    char buffer[256];
                    std::snprintf(buffer, sizeof(buffer),
                        "Segment %zu passes %.2f mm from the tilted bed at machine "
                        "(%.2f, %.2f, %.2f), inside this printer's %g mm clearance.",
                        k, standoff, machine_point.x(), machine_point.y(),
                        machine_point.z(), machine.min_bed_clearance);
                    result.error = buffer;
                    return result;
                }
            }
            if (machine_point.z() < 0.5) {
                char buffer[224];
                std::snprintf(buffer, sizeof(buffer),
                    "Segment %zu reaches machine z = %.2f mm, below the bed: the toolhead "
                    "would collide with it.", k, machine_point.z());
                result.error = buffer;
                return result;
            }
            if (std::hypot(machine_point.x(), machine_point.y()) > table_radius) {
                char buffer[224];
                std::snprintf(buffer, sizeof(buffer),
                    "Segment %zu reaches (%.2f, %.2f), outside this printer's %g mm table.",
                    k, machine_point.x(), machine_point.y(), machine.table_diameter);
                result.error = buffer;
                return result;
            }

            if (!pending_orient.empty()) {
                out << pending_orient;
                pending_orient.clear();
            }

            out << cmd
                << " X" << format_coord(machine_point.x())
                << " Y" << format_coord(machine_point.y())
                << " Z" << format_coord(machine_point.z())
                << tail << comment << "\n";
            ++result.moves_transformed;
            last_machine = machine_point;
            result.max_z = std::max(result.max_z, machine_point.z());
        }

        // A fan that ran out of layers would leave the segment printed at the
        // wrong angle - refuse rather than approximate.
        if (fanned && theta_cur < segment.theta - 1e-9) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                "Segment %zu has too few layers for a %g deg transition at %g deg per layer. "
                "Raise the transition step, or set it to zero for a single reorientation.",
                k, segment.theta, machine.transition_step);
            result.error = buffer;
            return result;
        }

        if (upright) {
            // Track how high the flat segment reached, so the first lift
            // clears it.
            std::istringstream scan(program.gcode);
            std::string scan_line;
            while (std::getline(scan, scan_line)) {
                const size_t semi = scan_line.find(';');
                const std::string code = semi == std::string::npos ? scan_line : scan_line.substr(0, semi);
                const std::string cmd = first_token(code);
                if (cmd != "G0" && cmd != "G1")
                    continue;
                const Words words = parse_words(code);
                if (words.has('Z'))
                    result.max_z = std::max(result.max_z, words.value.at('Z'));
            }
        }
    }

    // Leave the table level so the next job starts from a known pose.
    if (segments.size() > 1) {
        FiveAxisSegment level;
        const Vec3d at(machine.park_x, machine.park_y, result.max_z + machine.z_clearance);
        const std::string move = five_axis_reorient_move(level, machine, nullptr, &at);
        if (!move.empty())
            out << "; five-axis: level the table\n" << move << "\n";
    }

    result.gcode = out.str();
    return result;
}

} // namespace Slic3r
