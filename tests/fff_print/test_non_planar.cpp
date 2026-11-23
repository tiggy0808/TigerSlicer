#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// The top of the dome block (mm above its bottom) at `r` mm from its middle: `base` mm, and a cosine dome `bump` mm high
// out to `radius` mm.
static double dome_top(double r, double bump, double radius, double base) { return base + (r < radius ? 0.5 * bump * (1. + std::cos(M_PI * r / radius)) : 0.); }

// A `size` x `size` mm block `base` mm tall with a smooth round dome on top, `bump` mm high in the middle and `radius` mm
// across (see dome_top()). Its steepest slope is atan(bump * pi / (2 * radius)): about 17 degrees with the defaults. Built
// as a height field on a 0.5 mm grid.
static TriangleMesh dome_block(double bump = 2., double radius = 10., double base = 3., double size = 30.)
{
    const int    n    = int(std::lround(size / 0.5));
    const double cell = size / n;
    auto         top  = [&](double x, double y) { return dome_top(std::hypot(x - 0.5 * size, y - 0.5 * size), bump, radius, base); };
    indexed_triangle_set its;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            its.vertices.emplace_back(float(i * cell), float(j * cell), float(top(i * cell, j * cell)));
    const int bottom = int(its.vertices.size());
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            its.vertices.emplace_back(float(i * cell), float(j * cell), 0.f);
    auto t = [n](int i, int j) { return j * (n + 1) + i; };
    auto b = [n, bottom](int i, int j) { return bottom + j * (n + 1) + i; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            its.indices.emplace_back(t(i, j), t(i + 1, j), t(i + 1, j + 1));
            its.indices.emplace_back(t(i, j), t(i + 1, j + 1), t(i, j + 1));
            its.indices.emplace_back(b(i, j), b(i + 1, j + 1), b(i + 1, j));
            its.indices.emplace_back(b(i, j), b(i, j + 1), b(i + 1, j + 1));
        }
    for (int k = 0; k < n; ++k) {
        // Front (y = 0) and back (y = size).
        its.indices.emplace_back(b(k, 0), b(k + 1, 0), t(k + 1, 0));
        its.indices.emplace_back(b(k, 0), t(k + 1, 0), t(k, 0));
        its.indices.emplace_back(b(k + 1, n), b(k, n), t(k, n));
        its.indices.emplace_back(b(k + 1, n), t(k, n), t(k + 1, n));
        // Left (x = 0) and right (x = size).
        its.indices.emplace_back(b(0, k + 1), b(0, k), t(0, k));
        its.indices.emplace_back(b(0, k + 1), t(0, k), t(0, k + 1));
        its.indices.emplace_back(b(n, k), b(n, k + 1), t(n, k + 1));
        its.indices.emplace_back(b(n, k), t(n, k + 1), t(n, k));
    }
    return TriangleMesh(std::move(its));
}

// FNV-1a over the G-code's moves only: no comments, timestamps or config dump.
static uint64_t motion_hash(const std::string &gcode)
{
    uint64_t           hash = 1469598103934665603ull;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line)) {
        if (line.rfind("G0 ", 0) != 0 && line.rfind("G1 ", 0) != 0 && line.rfind("G2 ", 0) != 0 && line.rfind("G3 ", 0) != 0)
            continue;
        line = line.substr(0, line.find(';'));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.pop_back();
        for (const unsigned char c : line + "\n") {
            hash ^= c;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

// 0.2 mm layers, three top layers and a 0.4 mm nozzle, `extra` on top.
static DynamicPrintConfig dome_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"nozzle_diameter", "0.4"}, {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
                                   {"top_shell_layers", 3}, {"top_shell_thickness", 0}});
    config.set_deserialize_strict(extra);
    return config;
}

// The block sliced with dome_config(extra).
static std::string slice_dome(std::initializer_list<ConfigBase::SetDeserializeItem> extra, Model &model, Print &print,
                              const TriangleMesh &mesh = dome_block(), bool experimental = true)
{
    print.set_experimental_features_enabled(experimental);
    init_print({mesh}, print, model, dome_config(extra));
    print.process();
    return gcode(print);
}

// The block as one object, `edit` applied to it, sliced with dome_config(extra).
static std::string slice_dome_object(std::initializer_list<ConfigBase::SetDeserializeItem> extra, Model &model, Print &print,
                                     const std::function<void(ModelObject &)> &edit)
{
    ModelObject *object = model.add_object();
    object->name        = "dome";
    object->add_volume(dome_block());
    object->add_instance();
    edit(*object);
    for (ModelObject *each : model.objects) {
        each->ensure_on_bed();
        print.auto_assign_extruders(each);
    }
    print.set_experimental_features_enabled(true);
    print.apply(model, dome_config(extra));
    print.process();
    return gcode(print);
}

// The dome block as sliced for the regression hashes: 0.2 mm layers and a 0.4 mm nozzle, the rest the defaults.
static std::string slice_as_recorded(const std::string &mode, Print &print, bool experimental = true)
{
    Model              model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"nozzle_diameter", "0.4"}, {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
                                   {"non_planar_mode", mode}});
    print.set_experimental_features_enabled(experimental);
    init_print({dome_block()}, print, model, config);
    print.process();
    return gcode(print);
}

static void for_each_path(const ExtrusionEntity &entity, const std::function<void(const ExtrusionPath &)> &visit)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
        for (const ExtrusionEntity *child : collection->entities)
            for_each_path(*child, visit);
    else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        for (const ExtrusionPath &path : loop->paths)
            visit(path);
    else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        for (const ExtrusionPath &path : multi->paths)
            visit(path);
    else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
        visit(*path);
}

static void for_each_path(const Layer &layer, const std::function<void(const ExtrusionPath &)> &visit)
{
    for (const LayerRegion *region : layer.regions()) {
        for_each_path(region->perimeters, visit);
        for_each_path(region->thin_fills, visit);
        for_each_path(region->fills, visit);
    }
}

// The extrusions in the G-code more than 1 mm under their layer's height, which only curved layers make; the lowest of
// them; and the travels more than a layer under their layer's height, which none should be.
struct DeepMoves
{
    size_t extrusions  = 0;
    double lowest      = 1e9;
    size_t low_travels = 0;
};

static DeepMoves deep_moves(const std::string &gcode_text)
{
    DeepMoves   out;
    GCodeReader reader;
    double      layer_z = 0.;
    reader.parse_buffer(gcode_text, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string &raw = line.raw();
        if (raw.rfind(";Z:", 0) == 0)
            layer_z = std::stod(raw.substr(3));
        else if (raw.rfind("; Z_HEIGHT:", 0) == 0)
            layer_z = std::stod(raw.substr(11));
        if (!line.cmd_is("G1") || line.dist_XY(self) <= 0.)
            return;
        const double z = line.new_Z(self);
        if (!line.extruding(self)) {
            if (z < layer_z - 0.2 - 1e-3)
                ++out.low_travels;
        } else if (layer_z - z > 1.) {
            ++out.extrusions;
            out.lowest = std::min(out.lowest, z);
        }
    });
    return out;
}

// The shell of a curved path on `layer` of the block with a dome `bump` high (0 the lowest of `shells`), from how far
// under the block's top its first point is. `z_min` is where the object starts in print Z.
static int shell_of(const ExtrusionPath &path, const Layer &layer, int shells, double z_min = 0.)
{
    const Point3 &p = path.polyline.points.front();
    const double  r = unscale<double>(Point(p.x(), p.y()).cast<double>().norm());
    return shells - 1 - int(std::lround((dome_top(r, 2., 10., 3.) + z_min - (layer.print_z + unscale<double>(p.z()))) / 0.2));
}

// The layer's paths of curved top layers.
static std::vector<ExtrusionPath> curved_paths(const Layer &layer)
{
    std::vector<ExtrusionPath> out;
    for_each_path(layer, [&out](const ExtrusionPath &path) {
        if (path.non_planar)
            out.push_back(path);
    });
    return out;
}

TEST_CASE("Off prints what it printed before non-planar slicing", "[NonPlanar][Regression]")
{
    // First recorded before non_planar_mode replaced zaa_enabled. Recorded again with MSVC after the merge of
    // OrcaSlicer 2.5.2 (TigerSlicer 0.1.0), which changed the toolpaths. Other compilers give other values.
    Print print;
    CHECK(motion_hash(slice_as_recorded("off", print)) == 0x60a18b25c0b9b534ull);
    CHECK(print.objects().front()->non_planar_zones().empty());
}

TEST_CASE("Z contouring only prints what Z contouring printed before", "[NonPlanar][Regression]")
{
    // First recorded with zaa_enabled on, before non_planar_mode replaced it. Recorded again with MSVC after
    // the merge of OrcaSlicer 2.5.2 (TigerSlicer 0.1.0).
    Print print;
    CHECK(motion_hash(slice_as_recorded("z_contouring", print)) == 0xc38c218d37299d43ull);
}

TEST_CASE("Settings saved with the Z contouring checkbox load as the non-planar setting", "[NonPlanar]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("zaa_enabled", "1");
    CHECK(config.opt_enum<NonPlanarMode>("non_planar_mode") == npmZContouring);
    config.set_deserialize_strict("zaa_enabled", "0");
    CHECK(config.opt_enum<NonPlanarMode>("non_planar_mode") == npmOff);
}

TEST_CASE("On prints a gently sloped top in curved layers that follow it", "[NonPlanar]")
{
    Model       model;
    Print       print;
    const auto  gcode_text = slice_dome({{"non_planar_mode", "on"}}, model, print);
    const auto &object     = *print.objects().front();

    // The block's whole top, its flat border included, is one zone from the border (3 mm) to the dome's top (5 mm).
    REQUIRE(object.non_planar_zones().size() == 1);
    const NonPlanar::Zone &zone = object.non_planar_zones().front();
    CHECK_THAT(zone.bottom, Catch::Matchers::WithinAbs(3., 0.01));
    CHECK_THAT(zone.top, Catch::Matchers::WithinAbs(5., 0.01));
    CHECK(zone.shells == 3);

    // Three curved layers, each a layer apart under the top, the top one on it, printed bottom up.
    const Layer &layer = *object.layers()[zone.layer];
    const auto   paths = curved_paths(layer);
    REQUIRE(!paths.empty());
    std::vector<double> per_shell(3, 0.);
    int                 last_shell = 0;
    for (const ExtrusionPath &path : paths) {
        auto below = [&layer](const Point3 &p) {
            return dome_top(unscale<double>(Point(p.x(), p.y()).cast<double>().norm()), 2., 10., 3.) - (layer.print_z + unscale<double>(p.z()));
        };
        const int shell = shell_of(path, layer, 3);
        CAPTURE(shell, below(path.polyline.points.front()));
        REQUIRE(shell >= 0);
        REQUIRE(shell <= 2);
        CHECK(shell >= last_shell);
        last_shell = shell;
        per_shell[shell] += unscale<double>(path.polyline.length());
        for (const Point3 &p : path.polyline.points)
            CHECK_THAT(below(p), Catch::Matchers::WithinAbs((2 - shell) * 0.2, 0.02));
    }
    // Each covers the block's 30 x 30 mm top with lines about 0.4 mm apart.
    for (double length : per_shell)
        CHECK(length > 1500.);

    // The flat layers under them stop three layers under the top, and leave it no flat top surfaces.
    for (const Layer *flat : object.layers()) {
        CAPTURE(flat->print_z);
        for_each_path(*flat, [&flat](const ExtrusionPath &path) {
            if (path.non_planar)
                return;
            CHECK(path.role() != erTopSolidInfill);
            for (const Point3 &p : path.polyline.points) {
                const double r = unscale<double>(Point(p.x(), p.y()).cast<double>().norm());
                CHECK(flat->print_z <= dome_top(r, 2., 10., 3.) - 0.6 + 0.05);
            }
        });
    }

    // In the G-code, the curved layers reach below the layer's height, and the nozzle travels no lower than a layer under
    // it (Z contouring's own shallow paths): straight down into a curved layer, straight up out of it.
    const DeepMoves deep = deep_moves(gcode_text);
    CHECK(deep.extrusions > 100);
    CHECK(deep.low_travels == 0);
    // The lowest curved layer at the flat border, two layers under it.
    CHECK_THAT(deep.lowest, Catch::Matchers::WithinAbs(3. - 0.4, 0.02));
    // The layers emptied under the curved ones are not an error.
    for (const auto &warning : print.step_state_with_warnings(psGCodeExport).warnings)
        CHECK(warning.level != PrintStateBase::WarningLevel::CRITICAL);
}

TEST_CASE("With experimental features off, On prints as Z contouring only", "[NonPlanar]")
{
    Print print;
    CHECK(motion_hash(slice_as_recorded("on", print, false)) == 0xc38c218d37299d43ull);
    CHECK(print.objects().front()->non_planar_zones().empty());
    REQUIRE(print.experimental_values_ignored().size() == 1);
    CHECK(print.experimental_values_ignored().front() == "non_planar_mode");
}

TEST_CASE("Tops steeper than the maximum slope stay flat", "[NonPlanar]")
{
    // A dome 4 mm high and 3 mm in radius: up to 64 degrees. Only its very top and its foot are gentle.
    Model model;
    Print print;
    slice_dome({{"non_planar_mode", "on"}}, model, print, dome_block(4., 3.));
    for (const NonPlanar::Zone &zone : print.objects().front()->non_planar_zones())
        for (int i = 0; i < 8; ++i) {
            // 1.5 mm from the middle, the dome is at its steepest.
            const double angle = i * M_PI / 4.;
            const Point  steep(scaled<coord_t>(1.5 * std::cos(angle)), scaled<coord_t>(1.5 * std::sin(angle)));
            CAPTURE(zone.bottom, zone.top, angle);
            for (const ExPolygon &piece : zone.footprint)
                CHECK_FALSE(piece.contains(steep));
        }
}

TEST_CASE("The lowest curved layer fills the step the flat layers leave under it", "[NonPlanar]")
{
    Model       model;
    Print       print;
    slice_dome({{"non_planar_mode", "on"}}, model, print);
    const auto &object = *print.objects().front();
    REQUIRE(object.non_planar_zones().size() == 1);
    const Layer &layer = *object.layers()[object.non_planar_zones().front().layer];

    size_t fuller = 0;
    for (const ExtrusionPath &path : curved_paths(layer)) {
        if (shell_of(path, layer, 3) != 0) {
            CHECK(path.point_flow.empty());
            continue;
        }
        REQUIRE(path.point_flow.size() == path.polyline.points.size());
        for (size_t i = 0; i < path.point_flow.size(); ++i) {
            // The flat layers end at the highest 0.2 mm step under the curved layer's bottom, three layers under the
            // block's top. The point is two layers under the top: the top of the mesh, a little off the ideal dome.
            const Point3 &p      = path.polyline.points[i];
            const double  bottom = layer.print_z + unscale<double>(p.z()) + 0.4 - 0.6;
            const double  step   = bottom - 0.2 * std::floor((bottom + 1e-3) / 0.2);
            CAPTURE(bottom, step);
            CHECK_THAT(path.point_flow[i], Catch::Matchers::WithinAbs((0.2 + step) / 0.2, 0.02));
            if (path.point_flow[i] > 1.25f)
                ++fuller;
        }
    }
    CHECK(fuller > 100);
}

TEST_CASE("A mirrored object's top is curved like the object's", "[NonPlanar]")
{
    Model model;
    Print print;
    slice_dome_object({{"non_planar_mode", "on"}}, model, print,
                      [](ModelObject &object) { object.instances.front()->set_mirror(Axis::X, -1.); });
    const auto &zones = print.objects().front()->non_planar_zones();
    REQUIRE(zones.size() == 1);
    CHECK_THAT(zones.front().bottom, Catch::Matchers::WithinAbs(3., 0.01));
    CHECK_THAT(zones.front().top, Catch::Matchers::WithinAbs(5., 0.01));
}

TEST_CASE("A curved top on a raft follows the object's top", "[NonPlanar]")
{
    Model       model;
    Print       print;
    const auto  gcode_text = slice_dome({{"non_planar_mode", "on"}, {"raft_layers", 2}}, model, print);
    const auto &object     = *print.objects().front();
    const double z_min      = object.slicing_parameters().object_print_z_min;
    REQUIRE(z_min > 0.3);
    REQUIRE(object.non_planar_zones().size() == 1);
    const NonPlanar::Zone &zone = object.non_planar_zones().front();
    CHECK_THAT(zone.bottom, Catch::Matchers::WithinAbs(3., 0.01));
    CHECK_THAT(zone.top, Catch::Matchers::WithinAbs(5., 0.01));

    const Layer &layer = *object.layers()[zone.layer];
    const auto   paths = curved_paths(layer);
    REQUIRE(!paths.empty());
    for (const ExtrusionPath &path : paths) {
        const int shell = shell_of(path, layer, 3, z_min);
        REQUIRE(shell >= 0);
        REQUIRE(shell <= 2);
        for (const Point3 &p : path.polyline.points) {
            const double r = unscale<double>(Point(p.x(), p.y()).cast<double>().norm());
            CHECK_THAT(layer.print_z + unscale<double>(p.z()), Catch::Matchers::WithinAbs(dome_top(r, 2., 10., 3.) + z_min - (2 - shell) * 0.2, 0.02));
        }
    }
    CHECK_THAT(deep_moves(gcode_text).lowest, Catch::Matchers::WithinAbs(z_min + 3. - 0.4, 0.02));
}

TEST_CASE("Copies of an object print its curved top too", "[NonPlanar]")
{
    Model model, two_model;
    Print print, two;
    const DeepMoves one = deep_moves(slice_dome({{"non_planar_mode", "on"}}, model, print));
    // A copy, as the GUI makes one, shares the original's mesh, and so its layers and zones. (Model::add_object gives
    // the copy an extruder; the GUI's objects all have one.)
    const DeepMoves both = deep_moves(slice_dome_object({{"non_planar_mode", "on"}}, two_model, two, [&two_model](ModelObject &object) {
        object.config.set_key_value("extruder", new ConfigOptionInt(1));
        two_model.add_object(object)->instances.front()->set_offset(Vec3d(40., 0., 0.));
    }));
    REQUIRE(two.objects().size() == 2);
    CHECK(two.objects()[1]->get_shared_object() == two.objects()[0]);
    CHECK(two.objects()[1]->non_planar_zones().size() == 1);
    CHECK(both.extrusions == 2 * one.extrusions);
    CHECK(both.low_travels == 0);
    for (const auto &warning : two.step_state_with_warnings(psGCodeExport).warnings)
        CHECK(warning.level != PrintStateBase::WarningLevel::CRITICAL);
}

TEST_CASE("A top shell thickness adds curved layers as it adds flat ones", "[NonPlanar]")
{
    // Three top layers, but at least 1 mm of them: five 0.2 mm layers.
    Model       model;
    Print       print;
    const auto  gcode_text = slice_dome({{"non_planar_mode", "on"}, {"top_shell_thickness", 1.}}, model, print);
    const auto &zones      = print.objects().front()->non_planar_zones();
    REQUIRE(zones.size() == 1);
    CHECK(zones.front().shells == 5);
    CHECK_THAT(zones.front().bottom, Catch::Matchers::WithinAbs(3., 0.01));
    // The lowest curved layer at the flat border, four layers under it.
    CHECK_THAT(deep_moves(gcode_text).lowest, Catch::Matchers::WithinAbs(3. - 0.8, 0.02));
}

TEST_CASE("Modifiers turn curved tops on and off for their part of an object", "[NonPlanar]")
{
    SECTION("off over the whole object")
    {
        Model model;
        Print print;
        slice_dome_object({{"non_planar_mode", "on"}}, model, print, [](ModelObject &object) {
            TriangleMesh box = make_cube(32., 32., 10.);
            box.translate(-1.f, -1.f, -1.f);
            object.add_volume(std::move(box), ModelVolumeType::PARAMETER_MODIFIER)->config.set_key_value("non_planar_mode", new ConfigOptionEnum<NonPlanarMode>(npmOff));
        });
        CHECK(print.objects().front()->non_planar_zones().empty());
    }
    SECTION("on around the dome")
    {
        // A cylinder 12 mm in radius around the dome, the rest of the block off.
        Model       model;
        Print       print;
        const auto  gcode_text = slice_dome_object({{"non_planar_mode", "off"}}, model, print, [](ModelObject &object) {
            TriangleMesh cylinder = make_cylinder(12., 10.);
            cylinder.translate(15.f, 15.f, -1.f);
            object.add_volume(std::move(cylinder), ModelVolumeType::PARAMETER_MODIFIER)->config.set_key_value("non_planar_mode", new ConfigOptionEnum<NonPlanarMode>(npmOn));
        });
        const auto &zones = print.objects().front()->non_planar_zones();
        REQUIRE(zones.size() == 1);
        // Only within the cylinder.
        const BoundingBox box = get_extents(zones.front().footprint);
        CHECK_THAT(unscale<double>(box.size().x()), Catch::Matchers::WithinAbs(24., 0.5));
        CHECK_THAT(unscale<double>(box.size().y()), Catch::Matchers::WithinAbs(24., 0.5));
        CHECK(deep_moves(gcode_text).low_travels == 0);
    }
}

TEST_CASE("A curved top taller than the nozzle can reach is curved at its top only", "[NonPlanar]")
{
    // A dome 4 mm high and 20 mm across (17 degrees), with a 3 mm reach: three 0.2 mm layers leave 2.4 mm of curve.
    Model       model;
    Print       print;
    slice_dome({{"non_planar_mode", "on"}, {"non_planar_max_height", 3.}}, model, print, dome_block(4., 20., 3., 46.));
    const auto &zones = print.objects().front()->non_planar_zones();
    REQUIRE(zones.size() == 1);
    CHECK_THAT(zones.front().top, Catch::Matchers::WithinAbs(7., 0.01));
    CHECK_THAT(zones.front().bottom, Catch::Matchers::WithinAbs(7. - 2.4, 0.01));
    // Only the dome's top: a disc well inside the block.
    const BoundingBox box = get_extents(zones.front().footprint);
    CHECK(unscale<double>(box.size().x()) < 30.);
    CHECK(unscale<double>(box.size().x()) > 10.);
}
