///|/ TigerSlicer five-axis kinematics tests.
///|/
///|/ These pin the machine-frame maths the multidirectional pipeline relies
///|/ on: the two kinematics reach the same build direction, a Stewart bed
///|/ does it without twisting the part, tilts are taken about the physical
///|/ pivot, and the machine description comes from the printer profile
///|/ rather than from a built-in default.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "libslic3r/FiveAxis.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Approx;

static FiveAxisMachine rotary_machine()
{
    FiveAxisMachine machine;
    machine.kinematics = FiveAxisKinematics::RotaryTable;
    machine.tilt_min = 0.;
    machine.tilt_max = 20.;
    machine.ab_feedrate = 25.;
    machine.park_y = -65.;
    return machine;
}

TEST_CASE("Both kinematics reach the same build direction", "[five-axis]")
{
    for (double theta : { 0., 10., 30., 45., 89. }) {
        for (double phi : { 0., 37., 90., 180., 270. }) {
            const Vec3d expected = five_axis_build_direction(theta, phi);
            for (FiveAxisKinematics k : { FiveAxisKinematics::RotaryTable,
                                          FiveAxisKinematics::StewartPlatform }) {
                const Vec3d d = five_axis_segment_rotation(theta, phi, k) * Vec3d::UnitZ();
                INFO("theta " << theta << " phi " << phi << " kinematics " << int(k));
                CHECK(d.x() == Approx(expected.x()).margin(1e-12));
                CHECK(d.y() == Approx(expected.y()).margin(1e-12));
                CHECK(d.z() == Approx(expected.z()).margin(1e-12));
            }
        }
    }
    // A level segment leaves the part exactly where it was.
    CHECK(five_axis_segment_rotation(0., 0., FiveAxisKinematics::RotaryTable)
              .isApprox(Matrix3d::Identity()));
}

TEST_CASE("A Stewart bed tilts without twisting the part", "[five-axis]")
{
    // The distinguishing property: a rotary table spins the part with the
    // azimuth, a Stewart platform does not. Track a vector lying in the bed.
    const double theta = 20., phi = 90.;
    const Vec3d in_plane = Vec3d::UnitX();

    const Vec3d rotary = five_axis_segment_rotation(
        theta, phi, FiveAxisKinematics::RotaryTable) * in_plane;
    const Vec3d stewart = five_axis_segment_rotation(
        theta, phi, FiveAxisKinematics::StewartPlatform) * in_plane;

    // Tilting toward +Y must leave a vector along X untouched on a no-twist
    // bed, while the trunnion has spun it a quarter turn onto +Y.
    CHECK(stewart.x() == Approx(1.).margin(1e-12));
    CHECK(stewart.y() == Approx(0.).margin(1e-12));
    CHECK(rotary.y() == Approx(std::cos(theta * M_PI / 180.)).margin(1e-12));
    CHECK(rotary.x() == Approx(0.).margin(1e-12));
}

TEST_CASE("The Euler decomposition reproduces the pose", "[five-axis]")
{
    for (double theta : { 0., 12.5, 20. }) {
        for (double phi : { 0., 45., 90., 200. }) {
            const Matrix3d R = five_axis_segment_rotation(
                theta, phi, FiveAxisKinematics::StewartPlatform);
            const Vec3d e = five_axis_euler_ry_rx_rz(R);
            const Matrix3d rebuilt = five_axis_rotation_y(e.y()) *
                                     five_axis_rotation_x(e.x()) *
                                     five_axis_rotation_z(e.z());
            INFO("theta " << theta << " phi " << phi);
            CHECK(rebuilt.isApprox(R, 1e-9));
        }
    }
    // Yaw vanishes on the 90 deg azimuths and is needed in between - the
    // reason a Stewart bed has a max_yaw at all.
    for (double phi : { 0., 90., 180., 270. }) {
        const Matrix3d R = five_axis_segment_rotation(
            15., phi, FiveAxisKinematics::StewartPlatform);
        CHECK(five_axis_euler_ry_rx_rz(R).z() == Approx(0.).margin(1e-9));
    }
    const Matrix3d off_axis = five_axis_segment_rotation(
        15., 45., FiveAxisKinematics::StewartPlatform);
    CHECK(std::abs(five_axis_euler_ry_rx_rz(off_axis).z()) > 0.1);
}

TEST_CASE("Machine and local frames round-trip through the pivot", "[five-axis]")
{
    const Matrix3d R = five_axis_segment_rotation(15., 30., FiveAxisKinematics::RotaryTable);
    const Vec3d origin(3., -4., 20.);

    for (double pivot : { 0., 12.5 }) {
        for (const Vec3d &p : { Vec3d(0., 0., 0.), Vec3d(10., -5., 25.), Vec3d(-7., 8., 2.) }) {
            const Vec3d local = five_axis_machine_to_local(p, R, origin, pivot);
            const Vec3d back  = five_axis_local_to_machine(local, R, origin, pivot);
            INFO("pivot " << pivot);
            CHECK(back.x() == Approx(p.x()).margin(1e-9));
            CHECK(back.y() == Approx(p.y()).margin(1e-9));
            CHECK(back.z() == Approx(p.z()).margin(1e-9));
        }
    }

    // The pivot has to matter: tilting about an axis 12.5 mm up does not put
    // the part where tilting about the bed surface would.
    const Vec3d p(10., 0., 0.);
    CHECK_FALSE(five_axis_machine_to_local(p, R, origin, 0.)
                    .isApprox(five_axis_machine_to_local(p, R, origin, 12.5), 1e-6));
}

TEST_CASE("A segment outside the printer's tilt range is refused", "[five-axis]")
{
    const FiveAxisMachine machine = rotary_machine();   // tilt_max 20

    CHECK(five_axis_validate_segment({ 15., 0., Vec3d::Zero() }, machine).empty());
    CHECK(five_axis_validate_segment({ 20., 0., Vec3d::Zero() }, machine).empty());

    const std::string refused =
        five_axis_validate_segment({ 40., 0., Vec3d::Zero() }, machine);
    CHECK_FALSE(refused.empty());
    CHECK(refused.find("tilt range") != std::string::npos);

    CHECK_FALSE(five_axis_validate_segment({ -5., 0., Vec3d::Zero() }, machine).empty());
}

TEST_CASE("A Stewart pose beyond the yaw limit is refused", "[five-axis]")
{
    FiveAxisMachine machine = rotary_machine();
    machine.kinematics = FiveAxisKinematics::StewartPlatform;
    machine.max_yaw = 30.;

    // On a 90 deg azimuth there is no residual yaw at all.
    CHECK(five_axis_validate_segment({ 20., 90., Vec3d::Zero() }, machine).empty());

    machine.max_yaw = 0.01;   // now even a small residual yaw is too much
    const std::string refused =
        five_axis_validate_segment({ 20., 45., Vec3d::Zero() }, machine);
    CHECK_FALSE(refused.empty());
    CHECK(refused.find("yaw") != std::string::npos);
}

TEST_CASE("The reorientation move uses the machine's words and template",
          "[five-axis]")
{
    FiveAxisMachine machine = rotary_machine();
    const std::string move = five_axis_reorient_move({ 15., 0., Vec3d::Zero() }, machine);
    // deg/s in the profile, deg/min in the move.
    CHECK(move == "G0 C0 B15 F1500 ; reorient table");

    // Custom axis words travel through.
    machine.tilt_word = "A";
    machine.rotary_word = "B";
    CHECK(five_axis_reorient_move({ 10., 45., Vec3d::Zero() }, machine) ==
          "G0 B45 A10 F1500 ; reorient table");

    // A refused segment produces no move at all rather than a bad one.
    CHECK(five_axis_reorient_move({ 40., 0., Vec3d::Zero() }, machine).empty());
}

TEST_CASE("A Stewart reorientation reports its residual yaw", "[five-axis]")
{
    FiveAxisMachine machine = rotary_machine();
    machine.kinematics = FiveAxisKinematics::StewartPlatform;

    std::string warning;
    const std::string level = five_axis_reorient_move({ 15., 90., Vec3d::Zero() },
                                                      machine, &warning);
    CHECK(level.find("C0 ") != std::string::npos);   // yaw exactly zero
    CHECK(warning.empty());

    const std::string off_axis = five_axis_reorient_move({ 15., 45., Vec3d::Zero() },
                                                         machine, &warning);
    CHECK_FALSE(off_axis.empty());
    CHECK_FALSE(warning.empty());
    CHECK(warning.find("yaw") != std::string::npos);
}

TEST_CASE("The machine description comes from the printer profile", "[five-axis]")
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("five_axis_tilt_max", new ConfigOptionFloat(20.));
    config.set_key_value("five_axis_park_y", new ConfigOptionFloat(-65.));
    config.set_key_value("five_axis_table_diameter", new ConfigOptionFloat(100.));
    config.set_key_value("five_axis_tilt_word", new ConfigOptionString("A"));
    config.set_key_value("five_axis_ab_feedrate", new ConfigOptionFloat(30.));

    const FiveAxisMachine machine = FiveAxisMachine::from_config(config);
    CHECK(machine.tilt_max == Approx(20.));
    CHECK(machine.park_y == Approx(-65.));
    CHECK(machine.table_diameter == Approx(100.));
    CHECK(machine.tilt_word == "A");
    CHECK(machine.ab_feedrate == Approx(30.));

    // And it reaches the emitted move: 30 deg/s is F1800.
    CHECK(five_axis_reorient_move({ 15., 0., Vec3d::Zero() }, machine)
              .find("F1800") != std::string::npos);
    // A tilt past the profile's limit is refused by the profile's value.
    CHECK_FALSE(five_axis_validate_segment({ 25., 0., Vec3d::Zero() }, machine).empty());
}
