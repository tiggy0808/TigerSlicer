///|/ TigerSlicer five-axis kinematics.
///|/
///|/ Multidirectional printing on a reorientable bed: a fixed vertical nozzle
///|/ prints onto a bed that can tilt and spin between segments. A part is
///|/ decomposed into segments, each with its own build direction; every
///|/ segment is sliced upright in its own local frame and its toolpaths are
///|/ mapped back into the machine frame.
///|/
///|/ This header is the kinematic core: rotations, build directions, the
///|/ machine description read from the printer profile, segment validation
///|/ and the reorientation move. It performs no slicing and touches no
///|/ files, so it is exercised directly by tests.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FiveAxis_hpp_
#define slic3r_FiveAxis_hpp_

#include <string>

#include "libslic3r/Point.hpp"
// FiveAxisKinematics lives with the configuration options it is stored in:
// RotaryTable is a tilt+spin trunnion, R = Rz(phi) * Ry(theta); Stewart-
// Platform is a hexapod bed reaching the same direction without twisting
// the part, R = Rz(phi) * Ry(theta) * Rz(-phi).
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

const char *five_axis_kinematics_name(FiveAxisKinematics kinematics);

// The machine, as the printer profile describes it. Defaults match the
// option defaults so a machine with an incomplete profile still behaves.
struct FiveAxisMachine
{
    FiveAxisKinematics kinematics { FiveAxisKinematics::RotaryTable };

    // Axis words. CNC convention: A/B/C rotate about X/Y/Z, so a trunnion
    // tilting about Y and spinning about Z is B and C.
    std::string tilt_word { "B" };
    std::string rotary_word { "C" };

    double tilt_min { 0. };            // deg
    double tilt_max { 90. };           // deg
    double max_yaw { 30. };            // deg, Stewart only
    double pivot_offset_z { 0. };      // tilt-axis height above the bed, mm
    double table_diameter { 120. };    // mm
    double ab_feedrate { 25. };        // deg/s
    double transition_step { 2. };     // deg of tilt per layer across a flat->tilted
                                       // boundary on a Stewart bed; 0 = single reorientation
    double z_clearance { 10. };        // mm above the print before reorienting
    double park_x { 0. };              // XY park position while reorienting
    double park_y { -120. };
    double min_bed_clearance { 12. };  // mm
    bool   rotary_continuous { true }; // may the rotary axis pass 360 deg

    std::string reorient_gcode { "G0 {ROTARY}{A} {TILT}{B} F{F} ; reorient table" };
    std::string stewart_pose_gcode { "G0 A{RX} B{RY} C{YAW} F{F} ; platform pose" };

    // Read the five_axis_* keys off a printer profile. Keys the profile does
    // not carry keep the defaults above.
    static FiveAxisMachine from_config(const DynamicPrintConfig &config);
};

// One segment's build direction. theta tilts from vertical, phi is the
// azimuth; both in degrees. Segment 0 of a print is theta = phi = 0.
struct FiveAxisSegment
{
    double theta { 0. };
    double phi { 0. };
    // A point on the segment's slice plane, in the machine frame (bed-centre
    // origin, z from the bed surface).
    Vec3d  origin { Vec3d::Zero() };
};

// --- rotations --------------------------------------------------------------

Matrix3d five_axis_rotation_x(double degrees);
Matrix3d five_axis_rotation_y(double degrees);
Matrix3d five_axis_rotation_z(double degrees);

// The local -> machine rotation for one segment.
Matrix3d five_axis_segment_rotation(double theta_deg, double phi_deg,
                                    FiveAxisKinematics kinematics);

// d = R * (0,0,1). The same for both kinematics: the two differ only in
// whether the part twists on the way there.
Vec3d five_axis_build_direction(double theta_deg, double phi_deg);

// Exact Euler decomposition R = Ry(RY) * Rx(RX) * Rz(YAW), in degrees,
// choosing cos(RX) >= 0. Returned as (RX, RY, YAW).
Vec3d five_axis_euler_ry_rx_rz(const Matrix3d &rotation);

// Map a point between the machine frame and a segment's local frame. The
// pivot offset is the height of the physical tilt axis above the bed, so a
// tilt swings the part about that axis rather than about the bed surface.
Vec3d five_axis_machine_to_local(const Vec3d &point, const Matrix3d &rotation,
                                 const Vec3d &origin, double pivot_offset_z);
Vec3d five_axis_local_to_machine(const Vec3d &point, const Matrix3d &rotation,
                                 const Vec3d &origin, double pivot_offset_z);

// --- validation and output --------------------------------------------------

// Empty when the segment can be printed on this machine; otherwise the
// reason, ready to show. Checks the tilt range and, for a Stewart bed, that
// the residual yaw of a no-twist tilt stays within max_yaw.
std::string five_axis_validate_segment(const FiveAxisSegment &segment,
                                       const FiveAxisMachine &machine);

// The reorientation line for a segment, with the machine's own template and
// axis words filled in. `warning` receives the residual-yaw note when a
// Stewart pose needs one; it is left untouched otherwise. Returns an empty
// string when the segment is not printable (see validate).
//
// Both templates may also use {THETA} and {PHI} (the build direction, in
// degrees) and, when `position` is given, {X} {Y} {Z} - the toolhead
// position the pose is taken at, in machine millimetres.
std::string five_axis_reorient_move(const FiveAxisSegment &segment,
                                    const FiveAxisMachine &machine,
                                    std::string *warning = nullptr,
                                    const Vec3d *position = nullptr);

} // namespace Slic3r

#endif // slic3r_FiveAxis_hpp_
