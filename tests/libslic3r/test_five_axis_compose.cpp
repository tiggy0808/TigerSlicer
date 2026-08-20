///|/ TigerSlicer five-axis G-code composition tests.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <sstream>
#include <vector>

#include "libslic3r/FiveAxisCompose.hpp"

using namespace Slic3r;
using Catch::Approx;

static FiveAxisMachine machine_20deg()
{
    FiveAxisMachine m;
    m.kinematics = FiveAxisKinematics::RotaryTable;
    m.tilt_max = 20.;
    m.table_diameter = 200.;
    m.min_bed_clearance = 0.;   // exercised on its own below
    m.z_clearance = 10.;
    m.park_x = 0.;
    m.park_y = -65.;
    m.ab_feedrate = 25.;
    return m;
}

// A flat first segment and one tilted segment, both sliced at the bed centre.
static std::vector<FiveAxisSegmentProgram> two_segments(double theta, double phi,
                                                        const Vec3d &origin)
{
    FiveAxisSegmentProgram flat;
    flat.segment = { 0., 0., Vec3d::Zero() };
    flat.gcode = "G1 X0 Y0 Z0.2 F1200\nG1 X10 Y0 E0.5\n";

    FiveAxisSegmentProgram tilted;
    tilted.segment = { theta, phi, origin };
    tilted.gcode = "G1 X0 Y0 Z0.2 F1200\nG1 X10 Y0 Z0.2 E0.5\n";

    return { flat, tilted };
}

static std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        out.push_back(line);
    return out;
}

// Named distinctly: libslic3r has a contains(container, item) template that
// otherwise wins the overload and tries to std::find a string as a element.
static bool has_text(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

TEST_CASE("A single flat segment passes through untouched", "[five-axis-compose]")
{
    std::vector<FiveAxisSegmentProgram> segments(1);
    segments[0].gcode = "G1 X1 Y2 Z0.2 E0.1\nM106 S255\n";

    const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
    REQUIRE(r.ok());
    CHECK(has_text(r.gcode, "G1 X1 Y2 Z0.2 E0.1"));
    CHECK(has_text(r.gcode, "M106 S255"));
    CHECK(r.moves_transformed == 0);      // nothing to rotate
    // No reorientation at all for a one-segment print.
    CHECK_FALSE(has_text(r.gcode, "reorient"));
}

TEST_CASE("A tilted segment is mapped into the machine frame", "[five-axis-compose]")
{
    const double theta = 20.;
    const Vec3d origin(0., 0., 20.);
    const FiveAxisComposeResult r =
        five_axis_compose(two_segments(theta, 0., origin), machine_20deg());
    REQUIRE(r.ok());
    CHECK(r.moves_transformed == 2);

    // Local (10, 0, 0.2) with phi = 0 tilts about Y: x and z mix, y is kept,
    // and the segment origin is added.
    const double th = theta * M_PI / 180.;
    const double x = 10. * std::cos(th) + 0.2 * std::sin(th);
    const double z = -10. * std::sin(th) + 0.2 * std::cos(th) + origin.z();

    char expected[64];
    std::snprintf(expected, sizeof(expected), "X%.3f Y%.3f Z%.3f", x, 0., z);
    INFO(r.gcode);
    CHECK(has_text(r.gcode, expected));
}

TEST_CASE("Segments are joined by retract, lift, park and reorient",
          "[five-axis-compose]")
{
    const FiveAxisComposeResult r =
        five_axis_compose(two_segments(15., 0., Vec3d(0., 0., 20.)), machine_20deg());
    REQUIRE(r.ok());

    const std::vector<std::string> lines = lines_of(r.gcode);
    int retract = -1, lift = -1, park = -1, reorient = -1, prime = -1;
    // First occurrence of each: the closing "level the table" move carries
    // the same template text as the segment's own reorientation, so taking
    // the last match would compare against the wrong line.
    auto note = [&lines](int &slot, const char *needle) {
        if (slot >= 0)
            return;
        for (size_t i = 0; i < lines.size(); ++i)
            if (has_text(lines[i], needle)) { slot = int(i); return; }
    };
    note(retract, "retract before reorienting");
    note(lift, "lift clear");
    note(park, "; park");
    note(reorient, "reorient table");
    note(prime, "; prime");
    // The order matters: pulling back before lifting, and reorienting only
    // once the head is clear and out of the way.
    CHECK(retract >= 0);
    CHECK(lift > retract);
    CHECK(park > lift);
    CHECK(reorient > park);
    CHECK(prime > reorient);

    // The lift clears what the flat segment printed, by z_clearance.
    CHECK(has_text(r.gcode, "Z10.200"));       // flat max z 0.2 + 10
    CHECK(has_text(r.gcode, "Y-65.000"));      // the machine's park position
    CHECK(has_text(r.gcode, "G0 C0 B15 F1500"));
    // And the table is left level at the end.
    CHECK(has_text(r.gcode, "level the table"));
}

TEST_CASE("Absolute E is rebased at every reorientation", "[five-axis-compose]")
{
    FiveAxisComposeOptions options;
    options.relative_e = false;
    const FiveAxisComposeResult r =
        five_axis_compose(two_segments(15., 0., Vec3d(0., 0., 20.)), machine_20deg(), options);
    REQUIRE(r.ok());
    CHECK(has_text(r.gcode, "G92 E0"));

    options.relative_e = true;
    const FiveAxisComposeResult rel =
        five_axis_compose(two_segments(15., 0., Vec3d(0., 0., 20.)), machine_20deg(), options);
    REQUIRE(rel.ok());
    CHECK_FALSE(has_text(rel.gcode, "G92 E0"));
}

TEST_CASE("Moves that cannot be rotated are refused, not approximated",
          "[five-axis-compose]")
{
    SECTION("arcs") {
        auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
        segments[1].gcode = "G1 X0 Y0 Z0.2\nG2 X5 Y5 I2 J2 E0.3\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "arc"));
    }
    SECTION("relative positioning") {
        auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
        segments[1].gcode = "G1 X0 Y0 Z0.2\nG91\nG1 X5\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "relative"));
    }
    SECTION("G92 that moves the frame") {
        auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
        segments[1].gcode = "G1 X0 Y0 Z0.2\nG92 X0 Y0\nG1 X5 Y0 Z0.2\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "G92"));
    }
    SECTION("a retract keeps its E even before the position is known") {
        auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
        segments[1].gcode = "G1 E-0.8 F2400\nG1 X0 Y0 Z0.2\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
        REQUIRE(r.ok());
        CHECK(has_text(r.gcode, "G1 E-0.8 F2400"));
    }
}

TEST_CASE("A toolpath that would hit the machine is refused", "[five-axis-compose]")
{
    SECTION("below the bed") {
        // Origin at the bed with a big tilt drives the far end under z = 0.
        auto segments = two_segments(20., 0., Vec3d(0., 0., 0.));
        segments[1].gcode = "G1 X0 Y0 Z0.2\nG1 X40 Y0 Z0.2 E1\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "below the bed"));
    }
    SECTION("off the table") {
        FiveAxisMachine small = machine_20deg();
        small.table_diameter = 40.;
        auto segments = two_segments(0.,  0., Vec3d(0., 0., 20.));
        segments[1].segment.theta = 5.;
        segments[1].gcode = "G1 X0 Y0 Z0.2\nG1 X100 Y0 Z0.2 E1\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, small);
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "table"));
    }
    SECTION("inside the tilted bed clearance") {
        FiveAxisMachine guarded = machine_20deg();
        guarded.min_bed_clearance = 12.;
        auto segments = two_segments(20., 0., Vec3d(0., 0., 2.));
        segments[1].gcode = "G1 X0 Y0 Z0.2\nG1 X1 Y0 Z0.2 E0.1\n";
        const FiveAxisComposeResult r = five_axis_compose(segments, guarded);
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "clearance"));
    }
    SECTION("a segment past the tilt limit") {
        auto segments = two_segments(40., 0., Vec3d(0., 0., 20.));
        const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
        CHECK_FALSE(r.ok());
        CHECK(has_text(r.error, "tilt range"));
    }
}

TEST_CASE("The slicer's bed placement is taken back off", "[five-axis-compose]")
{
    // A part sliced centred on a 180x180 bed: the sliced X90 Y90 is the
    // part's own origin and must map to the segment origin, not to a point
    // 90 mm away.
    auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
    segments[1].bed_center = Vec2d(90., 90.);
    segments[1].gcode = "G1 X90 Y90 Z0 F1200\nG1 X90 Y90 Z0 E0.1\n";

    const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
    REQUIRE(r.ok());
    // Local (0,0,0) maps to the origin itself.
    CHECK(has_text(r.gcode, "X0.000 Y0.000 Z20.000"));
}

TEST_CASE("The printer's own G-code blocks stay in the machine frame",
          "[five-axis-compose]")
{
    // The prime line in a start block runs along the edge of a level bed.
    // Rotated into a tilted segment's frame it would dive under the table,
    // so a custom block is copied out exactly as the slicer wrote it.
    auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
    segments[1].gcode =
        ";TYPE:Custom\n"
        "G1 X-45 Y-40 Z0.2 F1000 ; prime line\n"
        ";TYPE:External perimeter\n"
        "G1 X0 Y0 Z0 E0.1\n";

    const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
    REQUIRE(r.ok());
    CHECK(has_text(r.gcode, "G1 X-45 Y-40 Z0.2 F1000 ; prime line"));
    // Only the move after the block was mapped.
    CHECK(r.moves_transformed == 1);
    CHECK(has_text(r.gcode, "X0.000 Y0.000 Z20.000"));
}

TEST_CASE("A machine-frame block does not seed the segment's own position",
          "[five-axis-compose]")
{
    // A move that names only some axes takes the rest from the last position.
    // The position a custom block left behind is the machine's, not this
    // segment's, so it must not be carried into the mapping.
    auto segments = two_segments(15., 0., Vec3d(0., 0., 20.));
    segments[1].gcode =
        ";TYPE:Custom\n"
        "G1 X-45 Y-40 Z0.2 F1000\n"
        ";TYPE:Perimeter\n"
        "G1 X5 E0.1\n"                 // incomplete: no frame to resolve it in
        "G1 X0 Y0 Z0 E0.2\n";

    const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
    REQUIRE(r.ok());
    CHECK(r.moves_transformed == 1);
    CHECK_FALSE(has_text(r.gcode, "Y-40.000"));
}

TEST_CASE("A tilted opening segment tilts the table after its start block",
          "[five-axis-compose]")
{
    // Every object leaning: there is no flat segment to begin on. Nothing is
    // printed yet, so no lift or park is needed - but the table still has to
    // reach the build direction, and only once the start block has run level.
    std::vector<FiveAxisSegmentProgram> segments(1);
    segments[0].segment = { 15., 0., Vec3d(0., 0., 20.) };
    segments[0].gcode =
        ";TYPE:Custom\n"
        "G28 ; home\n"
        ";TYPE:External perimeter\n"
        "G1 X0 Y0 Z0 E0.1\n";

    const FiveAxisComposeResult r = five_axis_compose(segments, machine_20deg());
    REQUIRE(r.ok());
    const std::vector<std::string> lines = lines_of(r.gcode);
    int home = -1, orient = -1, first_move = -1;
    for (int i = 0; i < int(lines.size()); ++i) {
        if (home < 0 && has_text(lines[i], "G28"))                    home = i;
        if (orient < 0 && has_text(lines[i], "five-axis: orient"))    orient = i;
        if (first_move < 0 && has_text(lines[i], "X0.000 Y0.000 Z20.000")) first_move = i;
    }
    REQUIRE(home >= 0);
    REQUIRE(orient > home);
    REQUIRE(first_move > orient);
    CHECK(r.moves_transformed == 1);
    // Nothing has been printed, so there is nothing to lift clear of.
    CHECK_FALSE(has_text(r.gcode, "lift clear"));
}

static FiveAxisMachine stewart_machine(double transition_step)
{
    FiveAxisMachine m = machine_20deg();
    m.kinematics = FiveAxisKinematics::StewartPlatform;
    m.max_yaw = 30.;
    m.transition_step = transition_step;
    return m;
}

TEST_CASE("A Stewart bed walks into a tilt one layer at a time",
          "[five-axis-compose]")
{
    // A tilted segment after a flat one is not reached by one reorientation:
    // the tilt ramps by transition_step at each layer boundary, each layer is
    // mapped through its own rotation, and Z follows the changing angle. The
    // boundary prints as a small wedge of slanted layers.
    FiveAxisSegmentProgram flat;
    flat.segment = { 0., 0., Vec3d::Zero() };
    flat.gcode = "G1 X0 Y0 Z0.2 F1200\nG1 X10 Y0 E0.5\n";

    FiveAxisSegmentProgram tilted;
    tilted.segment = { 10., 0., Vec3d(0., 0., 20.) };
    tilted.gcode =
        ";LAYER_CHANGE\n"
        "G1 X0 Y0 Z0.2 F1200\n"
        ";LAYER_CHANGE\n"
        "G1 X0 Y0 Z0.4 E0.5\n"
        ";LAYER_CHANGE\n"
        "G1 X0 Y0 Z0.6 E0.5\n";

    const FiveAxisComposeResult r =
        five_axis_compose({ flat, tilted }, stewart_machine(5.));
    REQUIRE(r.ok());

    // The join keeps its retract and lift but neither parks nor reorients.
    CHECK(has_text(r.gcode, "five-axis: slanted transition into segment 1"));
    CHECK_FALSE(has_text(r.gcode, "five-axis: reorient for segment 1"));
    CHECK_FALSE(has_text(r.gcode, "; park"));

    // The tilt arrives in steps of five degrees at the layer boundaries.
    CHECK(has_text(r.gcode, "; five-axis: transition tilt 5 deg"));
    CHECK(has_text(r.gcode, "; five-axis: transition tilt 10 deg"));
    CHECK(has_text(r.gcode, "B5"));
    CHECK(has_text(r.gcode, "B10"));

    // The first layer prints level exactly where the flat frame puts it, and
    // the later layers' Z moves with the angle: the same local height maps
    // lower and to the side as the bed tilts.
    CHECK(has_text(r.gcode, "X0.000 Y0.000 Z20.200"));                  // theta 0
    CHECK(has_text(r.gcode, "X0.035 Y0.000 Z20.398"));                  // theta 5
    CHECK(has_text(r.gcode, "X0.104 Y0.000 Z20.591"));                  // theta 10
}

TEST_CASE("A transition with too few layers is refused, not approximated",
          "[five-axis-compose]")
{
    FiveAxisSegmentProgram flat;
    flat.segment = { 0., 0., Vec3d::Zero() };
    flat.gcode = "G1 X0 Y0 Z0.2 F1200\nG1 X10 Y0 E0.5\n";

    FiveAxisSegmentProgram tilted;
    tilted.segment = { 10., 0., Vec3d(0., 0., 20.) };
    tilted.gcode = ";LAYER_CHANGE\nG1 X0 Y0 Z0.2 F1200\n";

    const FiveAxisComposeResult r =
        five_axis_compose({ flat, tilted }, stewart_machine(5.));
    REQUIRE_FALSE(r.ok());
    CHECK(has_text(r.error, "too few layers"));
}

TEST_CASE("Pose templates may speak x, y, z, theta and phi",
          "[five-axis-compose]")
{
    FiveAxisMachine m = stewart_machine(0.);   // single reorientation
    m.stewart_pose_gcode = "POSE X{X} Y{Y} Z{Z} T{THETA} P{PHI} B{RY}";

    const FiveAxisComposeResult r =
        five_axis_compose(two_segments(15., 0., Vec3d(0., 0., 20.)), m);
    REQUIRE(r.ok());
    // The pose is taken at the park position, lifted clear of the print.
    CHECK(has_text(r.gcode, "POSE X0 Y-65 Z10.2 T15 P0 B15"));
}

TEST_CASE("{PHI180} wraps the azimuth into a half turn either way",
          "[five-axis-compose]")
{
    FiveAxisMachine m = stewart_machine(0.);   // single reorientation
    m.stewart_pose_gcode = "POSE A{THETA} B{PHI180} RAW{PHI}";

    // An azimuth of 270 degrees is -90 to a firmware limited to -180..180.
    const FiveAxisComposeResult r =
        five_axis_compose(two_segments(15., 270., Vec3d(0., 0., 20.)), m);
    REQUIRE(r.ok());
    CHECK(has_text(r.gcode, "POSE A15 B-90 RAW270"));
}

TEST_CASE("The rotary template also knows the direction and position",
          "[five-axis-compose]")
{
    FiveAxisMachine m = machine_20deg();
    m.reorient_gcode = "ORIENT T{THETA} P{PHI} AT X{X} Y{Y} Z{Z}";

    const FiveAxisComposeResult r =
        five_axis_compose(two_segments(12., 45., Vec3d(0., 0., 20.)), m);
    REQUIRE(r.ok());
    CHECK(has_text(r.gcode, "ORIENT T12 P45 AT X0 Y-65 Z10.2"));
}
