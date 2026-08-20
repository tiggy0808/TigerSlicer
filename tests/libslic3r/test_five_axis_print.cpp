///|/ TigerSlicer five-axis printing: the whole path, in the application.
///|/
///|/ A flat base and a tilted segment go in as a model; one machine program
///|/ comes out, with the tilted segment sliced in its own frame and mapped
///|/ back. Nothing here shells out to an external toolchain.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <sstream>

#include "libslic3r/FiveAxisPrint.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using Catch::Approx;

static bool has_text(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

static DynamicPrintConfig five_axis_config()
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("five_axis_printing", new ConfigOptionBool(true));
    config.set_key_value("five_axis_tilt_max", new ConfigOptionFloat(30.));
    config.set_key_value("five_axis_table_diameter", new ConfigOptionFloat(300.));
    config.set_key_value("five_axis_park_y", new ConfigOptionFloat(-65.));
    config.set_key_value("five_axis_min_bed_clearance", new ConfigOptionFloat(0.));
    config.set_key_value("five_axis_ab_feedrate", new ConfigOptionFloat(25.));
    config.set_key_value("use_relative_e_distances", new ConfigOptionBool(true));
    // Relative E is only accepted alongside a per-layer reset.
    config.set_key_value("layer_gcode", new ConfigOptionString("G92 E0"));
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    // Keep the programs small and the geometry unambiguous.
    config.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    config.set_key_value("perimeters", new ConfigOptionInt(1));
    config.set_key_value("top_solid_layers", new ConfigOptionInt(1));
    config.set_key_value("bottom_solid_layers", new ConfigOptionInt(1));
    config.set_key_value("fill_density", new ConfigOptionPercent(0));
    config.set_key_value("skirts", new ConfigOptionInt(0));
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    return config;
}

// A cube object placed at the plate centre, optionally given a build
// direction.
static ModelObject *add_cube(Model &model, double size, double theta,
                             double origin_z, const char *name)
{
    ModelObject *object = model.add_object();
    object->name = name;
    object->add_volume(TriangleMesh(its_make_cube(size, size, size)));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(0., 0., 0.));
    if (theta != 0. || origin_z != 0.) {
        object->config.set("five_axis_segment_theta", theta);
        object->config.set("five_axis_segment_origin_z", origin_z);
    }
    object->ensure_on_bed();
    return object;
}

TEST_CASE("A model's build directions are read off its objects", "[five-axis-print]")
{
    Model model;
    ModelObject *flat = add_cube(model, 10., 0., 0., "base");
    CHECK(five_axis_segment_of(*flat).theta == Approx(0.));
    CHECK_FALSE(five_axis_model_has_tilted_segments(model));

    ModelObject *tilted = add_cube(model, 10., 15., 20., "tower");
    const FiveAxisSegment segment = five_axis_segment_of(*tilted);
    CHECK(segment.theta == Approx(15.));
    CHECK(segment.origin.z() == Approx(20.));
    CHECK(five_axis_model_has_tilted_segments(model));
}

TEST_CASE("A flat plate still slices as an ordinary print", "[five-axis-print]")
{
    Model model;
    add_cube(model, 10., 0., 0., "base");

    const FiveAxisPrintResult r = five_axis_slice(model, five_axis_config());
    INFO(r.error);
    REQUIRE(r.ok());
    CHECK(r.segments == 1);
    CHECK(r.moves_transformed == 0);       // nothing tilted, nothing rotated
    CHECK_FALSE(r.gcode.empty());
    // The sliced program ends with the slicer's own config dump, which
    // quotes the reorient template, so look for the block this code emits
    // rather than for the template text.
    CHECK_FALSE(has_text(r.gcode, "; five-axis: reorient"));
    CHECK_FALSE(has_text(r.gcode, "; five-axis: level"));
}

TEST_CASE("A tilted object is sliced in its own frame and mapped back",
          "[five-axis-print]")
{
    Model model;
    add_cube(model, 20., 0., 0., "base");
    add_cube(model, 10., 15., 20., "tower");

    const FiveAxisPrintResult r = five_axis_slice(model, five_axis_config());
    INFO(r.error);
    REQUIRE(r.ok());
    CHECK(r.segments == 2);
    // The tilted segment's moves all went through the machine-frame map.
    CHECK(r.moves_transformed > 0);
    // And the join is there, using this machine's words and park position.
    CHECK(has_text(r.gcode, "; five-axis: reorient for segment"));
    CHECK(has_text(r.gcode, "G0 C0 B15 F1500"));
    CHECK(has_text(r.gcode, "Y-65.000"));
    CHECK(has_text(r.gcode, "; five-axis: level the table"));
    // The tilted segment is grown from its origin, so it reaches above it.
    CHECK(r.max_z > 20.);
}

TEST_CASE("A build direction the machine cannot reach stops the export",
          "[five-axis-print]")
{
    Model model;
    add_cube(model, 20., 0., 0., "base");
    add_cube(model, 10., 60., 20., "tower");   // tilt_max is 30

    const FiveAxisPrintResult r = five_axis_slice(model, five_axis_config());
    CHECK_FALSE(r.ok());
    CHECK(has_text(r.error, "tilt range"));
    // The object is named in the message, so it is clear which one is wrong.
    CHECK(has_text(r.error, "tower"));
}

TEST_CASE("Segments are ordered so the plate is built up first",
          "[five-axis-print]")
{
    // The tilted object is added first; it must still print second.
    Model model;
    add_cube(model, 10., 15., 20., "tower");
    add_cube(model, 20., 0., 0., "base");

    const FiveAxisPrintResult r = five_axis_slice(model, five_axis_config());
    INFO(r.error);
    REQUIRE(r.ok());
    REQUIRE(r.segments == 2);
    // The first reorientation comes after some printing has happened.
    const size_t first_move = r.gcode.find("G1 X");
    const size_t reorient = r.gcode.find("reorient table");
    CHECK(first_move != std::string::npos);
    CHECK(reorient != std::string::npos);
    CHECK(first_move < reorient);
}

TEST_CASE("The job homes and primes once, not once per segment",
          "[five-axis-print]")
{
    // Each segment is a print of its own, so each would otherwise carry the
    // printer's start and end block. Homing after the base is down drives the
    // head through it, and the prime line runs along a bed that has turned.
    Model model;
    add_cube(model, 20., 0., 0., "base");
    add_cube(model, 10., 15., 20., "tower");

    DynamicPrintConfig config = five_axis_config();
    config.set_key_value("start_gcode",
        new ConfigOptionString("G28 ; home\nG1 X-45 Y-40 Z0.2 F1000 ; prime line"));
    config.set_key_value("end_gcode", new ConfigOptionString("M104 S0 ; nozzle off"));

    const FiveAxisPrintResult r = five_axis_slice(model, config);
    INFO(r.error);
    REQUIRE(r.ok());
    REQUIRE(r.segments == 2);

    // Count commands, not mentions: each segment ends with the slicer's
    // config dump, which quotes the start and end blocks in a comment.
    const auto count = [&r](const std::string &needle) {
        size_t n = 0;
        std::istringstream in(r.gcode);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty() && line[0] != ';' && line.find(needle) != std::string::npos)
                ++n;
        return n;
    };
    CHECK(count("G28 ; home") == 1);
    CHECK(count("; prime line") == 1);
    CHECK(count("M104 S0 ; nozzle off") == 1);
    // The prime line belongs to the level bed it was written for, so it is
    // left exactly as it is rather than rotated with the segment.
    CHECK(count("G1 X-45 Y-40 Z0.2 F1000") == 1);
    // ... and it runs before anything tilts.
    CHECK(r.gcode.find("; prime line") < r.gcode.find("reorient table"));
}

// A tower standing on a base, on a machine with the real 5X's limits: the
// case the feature exists for. The tower leans by `lean` degrees about Y
// about the point where it meets the base, so with lean = theta its foot is
// square to its own build direction.
static Model tower_on_base(double origin_z, double lean)
{
    Model model;

    ModelObject *base = model.add_object();
    base->name = "base";
    TriangleMesh slab(its_make_cube(40., 40., 4.));
    slab.translate(-20.f, -20.f, 0.f);
    base->add_volume(slab);
    base->add_instance();
    base->instances.front()->set_offset(Vec3d(20., 20., 0.));

    ModelObject *tower = model.add_object();
    tower->name = "tilted tower";
    TriangleMesh post(its_make_cube(12., 12., 25.));
    post.translate(-6.f, -6.f, 0.f);
    post.rotate_y(float(lean * M_PI / 180.));
    tower->add_volume(post);
    tower->add_instance();
    tower->instances.front()->set_offset(Vec3d(20., 20., 4.));
    tower->config.set("five_axis_segment_theta", 15.);
    tower->config.set("five_axis_segment_origin_z", origin_z);
    return model;
}

// The extent of the moves the mapping produced, taken from the composed
// program: everything after the join that starts the tilted segment.
static BoundingBoxf3 mapped_extent(const std::string &gcode)
{
    BoundingBoxf3 box;
    std::istringstream in(gcode);
    std::string line;
    bool started = false;
    while (std::getline(in, line)) {
        if (!started) {
            started = line.find("; prime") != std::string::npos;
            continue;
        }
        if (line.find("level the table") != std::string::npos)
            break;
        if (line.rfind("G0 X", 0) != 0 && line.rfind("G1 X", 0) != 0)
            continue;
        double x = 0., y = 0., z = 0.;
        if (std::sscanf(line.c_str(), "%*s X%lf Y%lf Z%lf", &x, &y, &z) == 3)
            box.merge(Vec3d(x, y, z));
    }
    return box;
}

TEST_CASE("A tower on a base exports on a machine with the 5X's limits",
          "[five-axis-print]")
{
    DynamicPrintConfig config = five_axis_config();
    config.set_key_value("five_axis_tilt_max", new ConfigOptionFloat(20.));
    config.set_key_value("five_axis_table_diameter", new ConfigOptionFloat(100.));

    // The tower leans the same 15 degrees it is to be printed along, so its
    // foot is square to its build direction and rests on the slice plane.
    Model model = tower_on_base(4., 15.);
    const FiveAxisPrintResult r = five_axis_slice(model, config);
    INFO(r.error);
    REQUIRE(r.ok());
    CHECK(r.segments == 2);
    CHECK(r.moves_transformed > 0);
    CHECK(r.warnings.empty());          // it already rests on its plane

    // The mapped moves land where the part was modelled: standing on the
    // segment's origin at (20, 20, 4), leaning 15 degrees, 25 mm long.
    const BoundingBoxf3 box = mapped_extent(r.gcode);
    INFO("mapped extent " << box.min.x() << "," << box.min.y() << "," << box.min.z()
         << " .. " << box.max.x() << "," << box.max.y() << "," << box.max.z());
    // The foot is square to the build direction, so it is a slanted face
    // through the origin: half of it sits below z = 4, by half the tower's
    // width times sin(15).
    CHECK(box.min.z() == Approx(4. - 6. * std::sin(15. * M_PI / 180.)).margin(1.5));
    CHECK(box.max.z() == Approx(4. + 25. * std::cos(15. * M_PI / 180.)).margin(1.5));
    CHECK(box.center().y() == Approx(20.).margin(0.5));
    // Leaning 15 degrees over 25 mm carries the top about 6.5 mm in +x, so the
    // footprint's centre sits downstream of the origin by about half that.
    CHECK(box.center().x() == Approx(23.2).margin(1.5));
}

TEST_CASE("A segment that does not rest on its slice plane is seated and said so",
          "[five-axis-print]")
{
    DynamicPrintConfig config = five_axis_config();
    config.set_key_value("five_axis_tilt_max", new ConfigOptionFloat(20.));
    config.set_key_value("five_axis_table_diameter", new ConfigOptionFloat(100.));

    // The tower stands at z = 4 but its build direction is measured from
    // z = 10: six millimetres of daylight that nothing would be printed
    // across. It is pulled onto the plane rather than left hanging.
    Model model = tower_on_base(10., 15.);
    const FiveAxisPrintResult r = five_axis_slice(model, config);
    INFO(r.error);
    REQUIRE(r.ok());
    REQUIRE(r.warnings.size() == 1);
    CHECK(has_text(r.warnings.front(), "off its slice plane"));
    CHECK(has_text(r.warnings.front(), "tilted tower"));
    // Seated on the plane it was told to grow from, six millimetres above the
    // one it was drawn on - so its foot straddles z = 10, not z = 4.
    CHECK(mapped_extent(r.gcode).min.z() > 7.5);
}

TEST_CASE("An empty plate is refused rather than producing an empty file",
          "[five-axis-print]")
{
    Model model;
    const FiveAxisPrintResult r = five_axis_slice(model, five_axis_config());
    CHECK_FALSE(r.ok());
    CHECK(has_text(r.error, "nothing on the plate"));
}

TEST_CASE("Two build directions in one print compose into one Stewart program",
          "[five-axis-print]")
{
    // A flat base, a segment leaning 10 degrees toward +X, and a segment
    // leaning 20 degrees toward +Y - all in one job on a Stewart bed. The
    // first tilt is walked into layer by layer; the second, starting from an
    // already tilted table, arrives by a single reorientation.
    DynamicPrintConfig config = five_axis_config();
    config.set_key_value("five_axis_kinematics",
        new ConfigOptionEnum<FiveAxisKinematics>(FiveAxisKinematics::StewartPlatform));
    config.set_key_value("five_axis_max_yaw", new ConfigOptionFloat(30.));
    config.set_key_value("five_axis_transition_step", new ConfigOptionFloat(2.));

    Model model;
    add_cube(model, 20., 0., 0., "base");
    ModelObject *lean_x = add_cube(model, 10., 10., 20., "lean toward x");
    lean_x->config.set("five_axis_segment_phi", 0.);
    ModelObject *lean_y = add_cube(model, 10., 20., 30., "lean toward y");
    lean_y->config.set("five_axis_segment_phi", 90.);

    const FiveAxisPrintResult r = five_axis_slice(model, config);
    INFO(r.error);
    REQUIRE(r.ok());
    CHECK(r.segments == 3);
    CHECK(r.moves_transformed > 0);

    // The first tilted segment fans in from the level table: stepped poses,
    // no park, and the full ten degrees is reached.
    CHECK(has_text(r.gcode, "five-axis: slanted transition into segment 1"));
    CHECK(has_text(r.gcode, "; five-axis: transition tilt 2 deg"));
    CHECK(has_text(r.gcode, "; five-axis: transition tilt 10 deg"));
    CHECK_FALSE(has_text(r.gcode, "five-axis: reorient for segment 1"));

    // The second starts from a tilted table, so it gets one reorientation -
    // the default pose template speaks direction angles: A tilt, B azimuth.
    CHECK(has_text(r.gcode, "five-axis: reorient for segment 2"));
    CHECK_FALSE(has_text(r.gcode, "five-axis: slanted transition into segment 2"));
    CHECK(has_text(r.gcode, "A20 B90"));

    // The job ends on a level table.
    CHECK(has_text(r.gcode, "five-axis: level the table"));
}
