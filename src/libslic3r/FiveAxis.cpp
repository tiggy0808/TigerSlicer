///|/ TigerSlicer five-axis kinematics. See FiveAxis.hpp.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FiveAxis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <boost/algorithm/string/replace.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

const char *five_axis_kinematics_name(FiveAxisKinematics kinematics)
{
    return kinematics == FiveAxisKinematics::StewartPlatform ? "stewart_platform"
                                                             : "rotary_table";
}

// A profile stores a multi-line G-code field with literal \n escapes.
static std::string unescape_gcode_field(const std::string &value)
{
    std::string out = value;
    boost::replace_all(out, "\\n", "\n");
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

static std::string format_number(double value)
{
    // %g, matching how the templates are written and keeping the words
    // short: 15 rather than 15.000000.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

FiveAxisMachine FiveAxisMachine::from_config(const DynamicPrintConfig &config)
{
    FiveAxisMachine machine;
    auto real = [&config](const char *key, double &target) {
        if (const ConfigOption *opt = config.option(key); opt != nullptr)
            target = opt->getFloat();
    };
    auto text = [&config](const char *key, std::string &target) {
        if (const ConfigOption *opt = config.option(key); opt != nullptr)
            target = opt->serialize();
    };

    if (const ConfigOption *opt = config.option("five_axis_kinematics"); opt != nullptr)
        machine.kinematics = opt->getInt() == int(FiveAxisKinematics::StewartPlatform)
            ? FiveAxisKinematics::StewartPlatform
            : FiveAxisKinematics::RotaryTable;

    text("five_axis_tilt_word", machine.tilt_word);
    text("five_axis_rotary_word", machine.rotary_word);
    real("five_axis_tilt_min", machine.tilt_min);
    real("five_axis_tilt_max", machine.tilt_max);
    real("five_axis_max_yaw", machine.max_yaw);
    real("five_axis_pivot_offset_z", machine.pivot_offset_z);
    real("five_axis_table_diameter", machine.table_diameter);
    real("five_axis_ab_feedrate", machine.ab_feedrate);
    real("five_axis_transition_step", machine.transition_step);
    real("five_axis_z_clearance", machine.z_clearance);
    real("five_axis_park_x", machine.park_x);
    real("five_axis_park_y", machine.park_y);
    real("five_axis_min_bed_clearance", machine.min_bed_clearance);

    if (const ConfigOption *opt = config.option("five_axis_rotary_continuous"); opt != nullptr)
        machine.rotary_continuous = opt->getBool();

    std::string reorient, stewart;
    text("five_axis_reorient_gcode", reorient);
    text("five_axis_stewart_pose_gcode", stewart);
    if (!reorient.empty())
        machine.reorient_gcode = unescape_gcode_field(reorient);
    if (!stewart.empty())
        machine.stewart_pose_gcode = unescape_gcode_field(stewart);

    return machine;
}

// --- rotations --------------------------------------------------------------

Matrix3d five_axis_rotation_x(double degrees)
{
    const double r = degrees * M_PI / 180.;
    const double c = std::cos(r), s = std::sin(r);
    Matrix3d m;
    m << 1., 0., 0.,
         0., c, -s,
         0., s,  c;
    return m;
}

Matrix3d five_axis_rotation_y(double degrees)
{
    const double r = degrees * M_PI / 180.;
    const double c = std::cos(r), s = std::sin(r);
    Matrix3d m;
    m <<  c, 0., s,
         0., 1., 0.,
         -s, 0., c;
    return m;
}

Matrix3d five_axis_rotation_z(double degrees)
{
    const double r = degrees * M_PI / 180.;
    const double c = std::cos(r), s = std::sin(r);
    Matrix3d m;
    m << c, -s, 0.,
         s,  c, 0.,
         0., 0., 1.;
    return m;
}

Matrix3d five_axis_segment_rotation(double theta_deg, double phi_deg,
                                    FiveAxisKinematics kinematics)
{
    Matrix3d rotation = five_axis_rotation_z(phi_deg) * five_axis_rotation_y(theta_deg);
    if (kinematics == FiveAxisKinematics::StewartPlatform)
        // Undo the spin: the bed reaches the same build direction without
        // twisting the part about it.
        rotation = rotation * five_axis_rotation_z(-phi_deg);
    return rotation;
}

Vec3d five_axis_build_direction(double theta_deg, double phi_deg)
{
    const double th = theta_deg * M_PI / 180.;
    const double ph = phi_deg * M_PI / 180.;
    return Vec3d(std::sin(th) * std::cos(ph),
                 std::sin(th) * std::sin(ph),
                 std::cos(th));
}

Vec3d five_axis_euler_ry_rx_rz(const Matrix3d &R)
{
    // Ry(b) * Rx(a) * Rz(g) has row 1 = (cos a sin g, cos a cos g, -sin a)
    // and third column (sin b cos a, -sin a, cos b cos a). Choosing
    // cos(a) >= 0 makes the decomposition unique away from gimbal lock.
    const double rx  = std::atan2(-R(1, 2), std::hypot(R(1, 0), R(1, 1)));
    const double ry  = std::atan2(R(0, 2), R(2, 2));
    const double yaw = std::atan2(R(1, 0), R(1, 1));
    return Vec3d(rx * 180. / M_PI, ry * 180. / M_PI, yaw * 180. / M_PI);
}

Vec3d five_axis_machine_to_local(const Vec3d &point, const Matrix3d &rotation,
                                 const Vec3d &origin, double pivot_offset_z)
{
    // The table pivots about an axis pivot_offset_z above the bed, so the
    // rotation is taken about that height rather than about z = 0.
    const Vec3d pivot(0., 0., pivot_offset_z);
    return rotation.transpose() * (point - origin - pivot) + pivot;
}

Vec3d five_axis_local_to_machine(const Vec3d &point, const Matrix3d &rotation,
                                 const Vec3d &origin, double pivot_offset_z)
{
    const Vec3d pivot(0., 0., pivot_offset_z);
    return rotation * (point - pivot) + origin + pivot;
}

// --- validation and output --------------------------------------------------

std::string five_axis_validate_segment(const FiveAxisSegment &segment,
                                       const FiveAxisMachine &machine)
{
    if (std::isnan(segment.theta) || std::isnan(segment.phi))
        return "Five-axis segment has no build direction.";

    if (segment.theta < machine.tilt_min - EPSILON ||
        segment.theta > machine.tilt_max + EPSILON) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
            "Five-axis segment tilts %g deg, outside this printer's tilt range "
            "[%g, %g]. Reorient the segment or raise the range on the printer.",
            segment.theta, machine.tilt_min, machine.tilt_max);
        return buffer;
    }

    if (machine.kinematics == FiveAxisKinematics::StewartPlatform) {
        const Matrix3d rotation = five_axis_segment_rotation(
            segment.theta, segment.phi, machine.kinematics);
        // Rounded as the emitted move rounds it, so a pose is judged by the
        // yaw the machine is actually asked for.
        const double yaw = std::round(
            five_axis_euler_ry_rx_rz(rotation).z() * 1e6) / 1e6;
        if (std::abs(yaw) > machine.max_yaw + EPSILON) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                "A no-twist tilt of %g deg toward %g deg needs %g deg of residual "
                "yaw, beyond this platform's limit of %g deg.",
                segment.theta, segment.phi, yaw, machine.max_yaw);
            return buffer;
        }
    }

    return {};
}

std::string five_axis_reorient_move(const FiveAxisSegment &segment,
                                    const FiveAxisMachine &machine,
                                    std::string *warning,
                                    const Vec3d *position)
{
    if (!five_axis_validate_segment(segment, machine).empty())
        return {};

    // The templates speak deg/min, the profile stores deg/s.
    const std::string feed = format_number(machine.ab_feedrate * 60.);

    // Placeholders both kinematics share: the build direction itself, and the
    // toolhead position the pose is taken at when the caller knows it.
    auto fill_shared = [&](std::string &line) {
        // {PHI180} wraps the azimuth into [-180, 180) for firmwares whose
        // axis limits reject anything past half a turn (RRF hexapods take
        // the pose as G1 A<theta> B<phi> with B limited to -180:180).
        double phi180 = std::fmod(segment.phi, 360.);
        if (phi180 >= 180.)
            phi180 -= 360.;
        else if (phi180 < -180.)
            phi180 += 360.;
        boost::replace_all(line, "{THETA}", format_number(segment.theta));
        boost::replace_all(line, "{PHI180}", format_number(phi180));
        boost::replace_all(line, "{PHI}", format_number(segment.phi));
        if (position != nullptr) {
            boost::replace_all(line, "{X}", format_number(position->x()));
            boost::replace_all(line, "{Y}", format_number(position->y()));
            boost::replace_all(line, "{Z}", format_number(position->z()));
        }
    };

    if (machine.kinematics == FiveAxisKinematics::RotaryTable) {
        std::string line = machine.reorient_gcode;
        boost::replace_all(line, "{ROTARY}", machine.rotary_word);
        boost::replace_all(line, "{TILT}", machine.tilt_word);
        boost::replace_all(line, "{A}", format_number(segment.phi));
        boost::replace_all(line, "{B}", format_number(segment.theta));
        boost::replace_all(line, "{F}", feed);
        fill_shared(line);
        return line;
    }

    const Matrix3d rotation = five_axis_segment_rotation(
        segment.theta, segment.phi, machine.kinematics);
    Vec3d euler = five_axis_euler_ry_rx_rz(rotation);
    // Round to the precision a machine can act on before formatting. Without
    // this the residue of the rotation product prints as an axis word like
    // C-1.42109e-14, and a pose that is exactly level reads as a tiny twist.
    // The rounding also kills negative zero, so a level pose reads "0".
    for (int i = 0; i < 3; ++i)
        euler(i) = std::round(euler(i) * 1e6) / 1e6 + 0.;

    if (warning != nullptr && std::abs(euler.z()) > 1e-9) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
            "A no-twist tilt of %g deg toward %g deg needs %g deg of residual yaw.",
            segment.theta, segment.phi, euler.z());
        *warning = buffer;
    }

    std::string line = machine.stewart_pose_gcode;
    boost::replace_all(line, "{RX}", format_number(euler.x()));
    boost::replace_all(line, "{RY}", format_number(euler.y()));
    boost::replace_all(line, "{YAW}", format_number(euler.z()));
    boost::replace_all(line, "{F}", feed);
    fill_shared(line);
    return line;
}

} // namespace Slic3r
