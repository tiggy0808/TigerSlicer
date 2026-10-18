#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/AutoSlice.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MSlicerResinJob.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::auto_slice;
using Catch::Matchers::WithinAbs;

namespace {

std::string picked(const std::vector<ProcessCandidate> &candidates, double nozzle, Purpose purpose)
{
    const int index = pick_process(candidates, nozzle, purpose);
    return index < 0 ? std::string("<none>") : candidates[size_t(index)].name;
}

// The Custom printers' process profiles for a 0.4 mm nozzle, as on disk.
const std::vector<ProcessCandidate> klipper_04 = {
    {"0.08mm Extra Fine", 0.08}, {"0.12mm Fine", 0.12},     {"0.15mm Optimal", 0.15},
    {"0.16mm Optimal", 0.16},    {"0.20mm Standard", 0.20}, {"0.24mm Draft", 0.24},
    {"0.28mm Extra Draft", 0.28}, {"0.32mm Standard", 0.32},
};

} // namespace

TEST_CASE("Auto slice picks the process profile nearest each purpose's layer height", "[AutoSlice]")
{
    // Quality aims at 0.3 x the nozzle, Structural at 0.5 x, Fast at 0.7 x.
    CHECK(picked(klipper_04, 0.4, Purpose::Quality) == "0.12mm Fine");
    CHECK(picked(klipper_04, 0.4, Purpose::Structural) == "0.20mm Standard");
    CHECK(picked(klipper_04, 0.4, Purpose::Fast) == "0.28mm Extra Draft");

    // A printer with fewer profiles still gets the nearest one.
    const std::vector<ProcessCandidate> ender = {
        {"0.12mm Fine", 0.12}, {"0.16mm Optimal", 0.16}, {"0.20mm Standard", 0.20}, {"0.24mm Draft", 0.24}};
    CHECK(picked(ender, 0.4, Purpose::Quality) == "0.12mm Fine");
    CHECK(picked(ender, 0.4, Purpose::Structural) == "0.20mm Standard");
    CHECK(picked(ender, 0.4, Purpose::Fast) == "0.24mm Draft");

    // The targets scale with the nozzle: 0.18 / 0.30 / 0.42 mm for 0.6 mm.
    const std::vector<ProcessCandidate> nozzle_06 = {
        {"0.18mm Fine", 0.18}, {"0.24mm Optimal", 0.24}, {"0.30mm Standard", 0.30}, {"0.36mm Draft", 0.36},
        {"0.42mm Extra Draft", 0.42}};
    CHECK(picked(nozzle_06, 0.6, Purpose::Quality) == "0.18mm Fine");
    CHECK(picked(nozzle_06, 0.6, Purpose::Structural) == "0.30mm Standard");
    CHECK(picked(nozzle_06, 0.6, Purpose::Fast) == "0.42mm Extra Draft");
}

TEST_CASE("Structural prefers a printer's strength profiles", "[AutoSlice]")
{
    const std::vector<ProcessCandidate> candidates = {
        {"0.12mm Fine", 0.12}, {"0.20mm Standard", 0.20}, {"0.16mm Strength", 0.16}, {"0.28mm Extra Draft", 0.28}};
    CHECK(picked(candidates, 0.4, Purpose::Structural) == "0.16mm Strength");
    // The other purposes ignore the name.
    CHECK(picked(candidates, 0.4, Purpose::Quality) == "0.12mm Fine");
    CHECK(picked(candidates, 0.4, Purpose::Fast) == "0.28mm Extra Draft");
}

TEST_CASE("Auto slice has nothing to pick without process profiles", "[AutoSlice]")
{
    CHECK(pick_process({}, 0.4, Purpose::Quality) == -1);
    // An unknown nozzle diameter falls back to 0.4 mm rather than picking at random.
    CHECK(picked(klipper_04, 0., Purpose::Fast) == "0.28mm Extra Draft");
}

TEST_CASE("Auto slice chooses among the selected printer's system process profiles", "[AutoSlice]")
{
    const boost::filesystem::path profiles =
        (boost::filesystem::path(std::string(TEST_DATA_DIR)) / ".." / ".." / "resources" / "profiles").lexically_normal();
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles.string(), PresetBundle::ORCA_DEFAULT_BUNDLE, PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(bundle.printers.select_preset_by_name("MyKlipper 0.4 nozzle", true));
    bundle.update_compatible(PresetSelectCompatibleType::Never, PresetSelectCompatibleType::Never);

    const std::vector<ProcessCandidate> candidates = process_candidates(bundle.prints);
    REQUIRE_FALSE(candidates.empty());
    for (const ProcessCandidate &candidate : candidates) {
        INFO(candidate.name);
        // Only this printer's 0.4 mm nozzle profiles: none of the 0.6 / 0.8 mm ones.
        CHECK(candidate.layer_height < 0.4);
        CHECK(candidate.layer_height > 0.);
    }
    CHECK(picked(candidates, 0.4, Purpose::Quality) == "0.12mm Fine @MyKlipper");
    CHECK(picked(candidates, 0.4, Purpose::Structural) == "0.20mm Standard @MyKlipper");
    CHECK(picked(candidates, 0.4, Purpose::Fast) == "0.28mm Extra Draft @MyKlipper");
}

TEST_CASE("Resin support levels grow from low to high and only touch support options", "[AutoSlice][resin]")
{
    const ResinSupportSettings &low    = resin_support_settings(SupportLevel::Low);
    const ResinSupportSettings &medium = resin_support_settings(SupportLevel::Medium);
    const ResinSupportSettings &high   = resin_support_settings(SupportLevel::High);
    CHECK(low.pillar_diameter < medium.pillar_diameter);
    CHECK(medium.pillar_diameter < high.pillar_diameter);
    CHECK(low.head_front_diameter < medium.head_front_diameter);
    CHECK(medium.head_front_diameter < high.head_front_diameter);
    CHECK(low.points_density < medium.points_density);
    CHECK(medium.points_density < high.points_density);
    CHECK(low.contact_penetration < high.contact_penetration);

    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    const DynamicPrintConfig before = config;
    apply_resin_support_settings(config, SupportLevel::High);
    CHECK_THAT(config.opt_float("support_pillar_diameter"), WithinAbs(high.pillar_diameter, 1e-9));
    CHECK_THAT(config.opt_float("support_head_front_diameter"), WithinAbs(high.head_front_diameter, 1e-9));
    CHECK_THAT(config.opt_float("support_head_width"), WithinAbs(high.head_width, 1e-9));
    CHECK(config.opt_int("support_points_density_relative") == high.points_density);
    CHECK_THAT(config.opt_float("msla_support_contact_penetration"), WithinAbs(high.contact_penetration, 1e-9));

    // Every key it changed invalidates prepared supports, so the next generation uses the level.
    for (const std::string &key : before.diff(config)) {
        INFO(key);
        CHECK(MSlicerResinJob::is_support_option(key));
    }
}

TEST_CASE("Only models thick enough for the wall are hollowed", "[AutoSlice][resin][hollowing]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("hollowing_min_thickness", new ConfigOptionFloat(3.));

    CHECK(can_hollow(its_make_cube(30., 30., 30.), config));
    // 4 mm across leaves no room inside 3 mm walls.
    CHECK_FALSE(can_hollow(its_make_cube(4., 4., 4.), config));
}

TEST_CASE("Drain holes go into the lowest surface, apart from each other", "[AutoSlice][resin][hollowing]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const float radius = 2.f, depth = 6.f;
    const sla::DrainHoles holes = place_drain_holes(cube, Transform3d::Identity(), radius, depth, 2);
    REQUIRE(holes.size() == 2);
    for (const sla::DrainHole &hole : holes) {
        // Straight up into the cube from its bottom face, starting just outside it.
        CHECK_THAT(hole.normal.z(), WithinAbs(1., 1e-5));
        CHECK_THAT(hole.pos.z(), WithinAbs(-sla::HoleStickOutLength, 1e-5));
        CHECK_THAT(hole.height, WithinAbs(depth + sla::HoleStickOutLength, 1e-5));
        CHECK_THAT(hole.radius, WithinAbs(radius, 1e-6));
        // Inside the face, not on its edge.
        CHECK(hole.pos.x() > radius);
        CHECK(hole.pos.x() < 20. - radius);
        CHECK(hole.pos.y() > radius);
        CHECK(hole.pos.y() < 20. - radius);
    }
    const Vec3f gap = holes[0].pos - holes[1].pos;
    CHECK(gap.norm() >= 2.f * radius + 2.f);
}

TEST_CASE("Drain holes follow the instance onto the bed", "[AutoSlice][resin][hollowing]")
{
    // Tipped over: after -90 degrees about X the cube's object-space +Y face is the bottom.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const Transform3d instance = Geometry::assemble_transform(Vec3d::Zero(), Vec3d(-0.5 * PI, 0., 0.));
    const sla::DrainHoles holes = place_drain_holes(cube, instance, 2.f, 6.f, 2);
    REQUIRE_FALSE(holes.empty());
    for (const sla::DrainHole &hole : holes) {
        // Stored in the object's own coordinates, but pointing up on the bed.
        const Vec3d world_normal = (instance.linear() * hole.normal.cast<double>()).normalized();
        CHECK_THAT(world_normal.z(), WithinAbs(1., 1e-5));
        const Vec3d world_pos = instance * hole.pos.cast<double>();
        double bottom = std::numeric_limits<double>::max();
        for (const stl_vertex &v : cube.vertices)
            bottom = std::min(bottom, (instance * v.cast<double>()).z());
        CHECK_THAT(world_pos.z(), WithinAbs(bottom - sla::HoleStickOutLength, 1e-4));
    }
}

TEST_CASE("No drain holes without a downward-facing surface", "[AutoSlice][resin][hollowing]")
{
    CHECK(place_drain_holes(indexed_triangle_set{}, Transform3d::Identity(), 2.f, 6.f, 2).empty());
    CHECK(place_drain_holes(its_make_cube(20., 20., 20.), Transform3d::Identity(), 2.f, 6.f, 0).empty());
}
