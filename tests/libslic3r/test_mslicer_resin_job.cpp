#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/MSlicerResinJob.hpp"
#include "libslic3r/MSlicerResinPrint.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cstring>
#include <map>

using namespace Slic3r;

namespace {

MSlicerResinSettings settings()
{
    MSlicerResinSettings value;
    value.resolution_x = 64;
    value.resolution_y = 48;
    value.platform_x_mm = 32.f;
    value.platform_y_mm = 24.f;
    value.platform_z_mm = 20.f;
    value.layer_height_mm = 1.f;
    value.bottom_layers = 1;
    value.transition_layers = 1;
    value.deterministic_seed = 39;
    return value;
}

indexed_triangle_set cube()
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {5, 5, 0}, {15, 5, 0}, {15, 15, 0}, {5, 15, 0},
        {5, 5, 2}, {15, 5, 2}, {15, 15, 2}, {5, 15, 2}
    };
    mesh.indices = {
        {0,2,1}, {0,3,2}, {4,5,6}, {4,6,7},
        {0,1,5}, {0,5,4}, {1,2,6}, {1,6,5},
        {2,3,7}, {2,7,6}, {3,0,4}, {3,4,7}
    };
    return mesh;
}

} // namespace

TEST_CASE("MSLA supports stay inside the build volume", "[mslicer][resin][supports]")
{
    // A sphere at the plate edge with a tiny minimum height: its contacts sit
    // right at Z=0 and its columns route outward past the plate boundary.
    // Before the volume clamp this refused the whole generation.
    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 1.f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 2.f;
    support.max_overhang_angle_degrees = 30.f;
    support.face_support_spacing_mm = 2.f;
    support.edge_support_spacing_mm = 2.f;
    support.min_support_height_mm = 0.01f;
    support.base = TS_MSLICER_SUPPORT_BASE_RAFT;

    const MSlicerResinSettings resin = settings();
    indexed_triangle_set sphere = its_make_sphere(6.0, PI / 18.0);
    Transform3d at_edge = Transform3d::Identity();
    at_edge.translate(Vec3d(6.5, 6.5, 6.));

    MSlicerResinJob job(resin);
    job.add_mesh(sphere, at_edge);
    indexed_triangle_set supports;
    REQUIRE_NOTHROW(supports = job.generate_supports(support));
    REQUIRE_FALSE(supports.empty());
    for (const Vec3f &vertex : supports.vertices) {
        CHECK(vertex.z() >= -1e-3f);
        CHECK(vertex.z() <= resin.platform_z_mm + 1e-3f);
        CHECK(vertex.x() >= -1e-3f);
        CHECK(vertex.x() <= resin.platform_x_mm + 1e-3f);
        CHECK(vertex.y() >= -1e-3f);
        CHECK(vertex.y() <= resin.platform_y_mm + 1e-3f);
    }
}

TEST_CASE("MSLA supports reach a lifted sphere", "[mslicer][resin][supports]")
{
    // A sphere resting on the plate has its only overhang at plate level,
    // where no tip fits - the GUI explains that and asks for a lift. Lifted,
    // its underside is exactly what the generator must catch.
    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 1.f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 0.5f;
    support.max_overhang_angle_degrees = 30.f;
    support.face_support_spacing_mm = 2.f;
    support.edge_support_spacing_mm = 2.f;
    support.min_support_height_mm = 1.2f;
    support.base = TS_MSLICER_SUPPORT_BASE_NONE;

    indexed_triangle_set sphere = its_make_sphere(6.0, PI / 18.0);
    Transform3d lifted = Transform3d::Identity();
    lifted.translate(Vec3d(16., 12., 8.));

    MSlicerResinJob job(settings());
    job.add_mesh(sphere, lifted);
    indexed_triangle_set supports;
    REQUIRE_NOTHROW(supports = job.generate_supports(support));
    REQUIRE_FALSE(supports.empty());
    // Columns reach the plate and hold the underside, not just the south pole.
    float lowest = std::numeric_limits<float>::infinity();
    for (const Vec3f &vertex : supports.vertices)
        lowest = std::min(lowest, vertex.z());
    CHECK(lowest == Catch::Approx(0.f).margin(1e-3));
}

TEST_CASE("MSlicer resin job owns raster results and deterministic metadata", "[mslicer][resin]")
{
    MSlicerResinJob job(settings());
    Transform3d transform = Transform3d::Identity();
    transform.translate(Vec3d(5., 2., 0.));
    job.add_mesh(cube(), transform);
    job.slice();

    REQUIRE(job.layer_count() == 2);
    REQUIRE(job.input_size_mm().isApprox(Vec3f(10.f, 10.f, 2.f)));
    REQUIRE(job.input_min_mm().isApprox(Vec3f(10.f, 7.f, 0.f)));
    REQUIRE(job.input_max_mm().isApprox(Vec3f(20.f, 17.f, 2.f)));
    REQUIRE(job.input_mesh_volume_mm3() == Catch::Approx(200.).margin(0.01));
    REQUIRE(job.layer(0).size() == 64 * 48);
    REQUIRE(job.metrics().lit_voxels > 0);
    REQUIRE(job.metrics().resin_volume_mm3 == Catch::Approx(200.).margin(1.));
    REQUIRE(job.manifest_json().find("be44e0b") != std::string::npos);
    const std::string diagnostics = job.diagnostics_json();
    REQUIRE(diagnostics.front() == '[');
    REQUIRE(diagnostics.back() == ']');
    const std::map<ts_mslicer_format, std::string> formats {
        { TS_MSLICER_FORMAT_CTB, "\"version\":5" },
        { TS_MSLICER_FORMAT_GOO, "\"version\":\"V3.0\"" },
        { TS_MSLICER_FORMAT_NANODLP, "\"format\":\"nanodlp\"" },
        { TS_MSLICER_FORMAT_SVG, "diagnostic vector layer sheet" }
    };
    for (const auto &[format, marker] : formats) {
        const std::vector<uint8_t> first = job.encode(format);
        const std::vector<uint8_t> second = job.encode(format);
        REQUIRE_FALSE(first.empty());
        CHECK(first == second);
        CHECK(job.inspect_output(format, first).find(marker) != std::string::npos);

        std::vector<uint8_t> malformed(first.begin(), first.begin() + first.size() / 2);
        CHECK_THROWS_AS(job.inspect_output(format, malformed), MSlicer::Error);
    }
    REQUIRE(std::string(MSlicerResinJob::format_extension(TS_MSLICER_FORMAT_CTB)) == ".ctb");
    REQUIRE(std::string(MSlicerResinJob::format_extension(TS_MSLICER_FORMAT_SVG)) == ".svg");
    REQUIRE(MSlicerResinJob::format_extension(999) == nullptr);
}

TEST_CASE("MSlicer resin job rejects unsafe configuration and mesh input", "[mslicer][resin]")
{
    MSlicerResinSettings invalid = settings();
    invalid.resolution_x = 0;
    REQUIRE_THROWS_AS(MSlicerResinJob::validate(invalid), MSlicer::Error);

    MSlicerResinJob job(settings());
    REQUIRE_THROWS_AS(job.add_mesh(indexed_triangle_set{}), std::invalid_argument);
}

TEST_CASE("MSlicer resin job tolerates degenerate triangles like the FDM pipeline", "[mslicer][resin]")
{
    // Real-world STLs - the bundled Benchy among them - carry a few
    // degenerate triangles. The FDM pipeline slices them; refusing them here
    // made the same model unsliceable only in the resin workspace.
    indexed_triangle_set damaged = cube();
    // A face with a repeated vertex: no interior, no topology - dropped.
    damaged.indices.push_back({0, 0, 1});
    MSlicerResinJob job(settings());
    CHECK_NOTHROW(job.add_mesh(damaged));
    CHECK_NOTHROW(job.slice());
    CHECK(job.layer_count() == 2);
    CHECK(job.diagnostics_json().find("degenerate triangles") != std::string::npos);
}

TEST_CASE("MSLA supports survive a zero-area sliver in a manifold mesh", "[mslicer][resin][supports]")
{
    // A manifold cube with one zero-area sliver: vertex 8 lies on the edge
    // 0-1, one side face is split through it, and the sliver {0,1,8} pairs
    // every edge. Its normal is undefined (NaN); the generator must skip the
    // placements it contributes rather than poisoning the generated mesh.
    indexed_triangle_set slivered = cube();
    slivered.vertices.emplace_back(10.f, 5.f, 0.f);
    for (auto &face : slivered.indices)
        if (face(0) == 0 && face(1) == 1 && face(2) == 5)
            face = {0, 8, 5};
    slivered.indices.push_back({8, 1, 5});
    slivered.indices.push_back({0, 1, 8});

    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));
    MSlicerResinJob job(settings());
    job.add_mesh(slivered, raised);

    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 1.f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 0.5f;
    support.max_overhang_angle_degrees = 35.f;
    support.face_support_spacing_mm = 2.f;
    support.edge_support_spacing_mm = 2.f;
    support.min_support_height_mm = 1.2f;

    indexed_triangle_set supports;
    CHECK_NOTHROW(supports = job.generate_supports(support));
    CHECK_FALSE(supports.empty());
    for (const Vec3f &vertex : supports.vertices)
        CHECK(vertex.allFinite());
}

TEST_CASE("MSlicer resin job collects printable model instances without legacy processors", "[mslicer][resin]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    // Legacy option definitions keep supports/pad enabled by default for safe
    // migration of old profiles; native MSlicer resin profiles disable them.
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(false));

    Model model;
    ModelObject *object = model.add_object("cube", "", TriangleMesh(cube()));
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(3., 4., 0.));

    MSlicerResinJob::validate_model_input(model, config);
    MSlicerResinJob job(settings());
    CHECK(job.add_model(model) == 1);
    CHECK(job.input_min_mm().isApprox(Vec3f(8.f, 9.f, 0.f)));
    CHECK(job.input_max_mm().isApprox(Vec3f(18.f, 19.f, 2.f)));

    object->add_volume(TriangleMesh(cube()), ModelVolumeType::NEGATIVE_VOLUME);
    CHECK_THROWS_WITH(MSlicerResinJob::validate_model_input(model, config),
        Catch::Matchers::ContainsSubstring("negative volume"));
}

TEST_CASE("MSlicer automatic supports are generated before and sliced with the model", "[mslicer][resin][supports]")
{
    MSlicerResinJob unsupported(settings());
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));
    unsupported.add_mesh(cube(), raised);
    unsupported.slice();
    const uint64_t unsupported_voxels = unsupported.metrics().lit_voxels;

    MSlicerResinJob supported(settings());
    supported.add_mesh(cube(), raised);
    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 1.f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 0.5f;
    support.max_overhang_angle_degrees = 35.f;
    support.face_support_spacing_mm = 2.f;
    support.edge_support_spacing_mm = 2.f;
    const indexed_triangle_set prepared_supports = supported.generate_supports(support);
    CHECK_FALSE(prepared_supports.empty());
    supported.slice();

    CHECK(supported.metrics().lit_voxels > unsupported_voxels);
    CHECK(supported.diagnostics_json().find("generated automatic MSLA supports") != std::string::npos);
    CHECK_FALSE(supported.encode(TS_MSLICER_FORMAT_GOO).empty());
}

TEST_CASE("MSlicer support option keys cover everything generation reads", "[mslicer][resin][supports]")
{
    const std::vector<std::string> &keys = MSlicerResinJob::support_option_keys();

    // Every key generation actually depends on. A key missing here would let a
    // prepared support mesh survive a change it was generated from.
    for (const char *expected : { "support_tree_type", "support_enforcers_only",
                                  "support_pillar_diameter", "support_head_front_diameter",
                                  "support_head_width", "support_base_height",
                                  "support_base_safety_distance",
                                  "support_points_density_relative",
                                  "msla_support_contact_penetration", "msla_support_min_height",
                                  "msla_support_min_overhang_area", "msla_support_base_kind",
                                  "msla_support_base_diameter", "msla_support_overhang_angle",
                                  "msla_support_edge_angle_delta", "msla_support_precision",
                                  "msla_support_on_model", "msla_support_cross_bracing",
                                  "msla_support_brace_spacing", "msla_support_adaptive_diameter",
                                  "msla_support_max_pillar_diameter" })
        CHECK(MSlicerResinJob::is_support_option(expected));

    CHECK_FALSE(MSlicerResinJob::is_support_option("pad_enable"));
    CHECK_FALSE(MSlicerResinJob::is_support_option("layer_height"));
    CHECK_FALSE(MSlicerResinJob::is_support_option(""));

    // The list is the single source of truth, so it must not carry duplicates.
    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    // Each listed key must exist in a real SLA print configuration; a renamed
    // option would otherwise silently stop invalidating prepared supports.
    DynamicPrintConfig config;
    config.apply(Slic3r::DynamicPrintConfig::full_print_config());
    config.apply(*Slic3r::DynamicPrintConfig::new_from_defaults_keys(keys));
    for (const std::string &key : keys)
        CHECK(config.option(key) != nullptr);

    // Changing any of them changes what generate_supports() is asked to build,
    // or whether it may run at all.
    const MSlicerSupportSettings before = MSlicerResinJob::support_settings_from_config(config);
    config.set_key_value("support_points_density_relative", new ConfigOptionInt(400));
    const MSlicerSupportSettings after = MSlicerResinJob::support_settings_from_config(config);
    CHECK(after.face_support_spacing_mm < before.face_support_spacing_mm);
}

TEST_CASE("MSLA support generation settings come from the configuration", "[mslicer][resin][supports]")
{
    DynamicPrintConfig config;
    config.apply(Slic3r::DynamicPrintConfig::full_print_config());
    config.apply(*Slic3r::DynamicPrintConfig::new_from_defaults_keys(
        MSlicerResinJob::support_option_keys()));

    // Defaults must reproduce what the generator did before these settings
    // existed, or every saved profile silently changes what it generates.
    const MSlicerSupportSettings defaults = MSlicerResinJob::support_settings_from_config(config);
    CHECK(defaults.contact_penetration_mm == Catch::Approx(0.f));
    CHECK(defaults.base == TS_MSLICER_SUPPORT_BASE_RAFT);
    CHECK(defaults.min_overhang_area_mm2 == Catch::Approx(0.f));
    CHECK(defaults.max_overhang_angle_degrees == Catch::Approx(30.f));
    CHECK(defaults.precision == 10u);
    CHECK_FALSE(defaults.support_on_model);
    CHECK_FALSE(defaults.cross_bracing);
    CHECK(defaults.brace_spacing_mm == Catch::Approx(10.f));
    CHECK_FALSE(defaults.adaptive_diameter);
    CHECK(defaults.max_pillar_diameter_mm == Catch::Approx(6.f));

    config.set_key_value("msla_support_contact_penetration", new ConfigOptionFloat(0.4));
    config.set_key_value("msla_support_min_height", new ConfigOptionFloat(2.5));
    config.set_key_value("msla_support_min_overhang_area", new ConfigOptionFloat(1.75));
    config.set_key_value("msla_support_base_kind",
                         new ConfigOptionEnum<MSlicerSupportBaseKind>(msbPads));
    config.set_key_value("msla_support_base_diameter", new ConfigOptionFloat(4.5));
    config.set_key_value("msla_support_overhang_angle", new ConfigOptionFloat(45.));
    config.set_key_value("msla_support_edge_angle_delta", new ConfigOptionFloat(90.));
    config.set_key_value("msla_support_precision", new ConfigOptionInt(24));
    config.set_key_value("msla_support_on_model", new ConfigOptionBool(true));
    config.set_key_value("msla_support_cross_bracing", new ConfigOptionBool(true));
    config.set_key_value("msla_support_brace_spacing", new ConfigOptionFloat(15.));
    config.set_key_value("msla_support_adaptive_diameter", new ConfigOptionBool(true));
    config.set_key_value("msla_support_max_pillar_diameter", new ConfigOptionFloat(8.));

    const MSlicerSupportSettings edited = MSlicerResinJob::support_settings_from_config(config);
    CHECK(edited.contact_penetration_mm == Catch::Approx(0.4f));
    CHECK(edited.min_support_height_mm == Catch::Approx(2.5f));
    CHECK(edited.min_overhang_area_mm2 == Catch::Approx(1.75f));
    CHECK(edited.base == TS_MSLICER_SUPPORT_BASE_PADS);
    CHECK(edited.base_diameter_mm == Catch::Approx(4.5f));
    CHECK(edited.max_overhang_angle_degrees == Catch::Approx(45.f));
    // Degrees in the configuration, radians at the engine boundary.
    CHECK(edited.edge_angle_delta_radians == Catch::Approx(float(M_PI) / 2.f).epsilon(0.001));
    CHECK(edited.precision == 24u);
    CHECK(edited.support_on_model);
    CHECK(edited.cross_bracing);
    CHECK(edited.brace_spacing_mm == Catch::Approx(15.f));
    CHECK(edited.adaptive_diameter);
    CHECK(edited.max_pillar_diameter_mm == Catch::Approx(8.f));
}

TEST_CASE("MSLA support base kind changes what the columns stand on", "[mslicer][resin][supports]")
{
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));

    auto generate = [&raised](ts_mslicer_support_base base, float penetration) {
        MSlicerResinJob job(settings());
        job.add_mesh(cube(), raised);
        MSlicerSupportSettings support;
        support.support_radius_mm = 0.6f;
        support.tip_radius_mm = 0.2f;
        support.tip_length_mm = 1.f;
        support.precision = 8;
        support.base_height_mm = 0.5f;
        support.base_offset_mm = 0.5f;
        support.base_diameter_mm = 2.f;
        support.max_overhang_angle_degrees = 35.f;
        support.face_support_spacing_mm = 2.f;
        support.edge_support_spacing_mm = 2.f;
        support.min_support_height_mm = 1.2f;
        support.base = base;
        support.contact_penetration_mm = penetration;
        return job.generate_supports(support);
    };

    const indexed_triangle_set raft = generate(TS_MSLICER_SUPPORT_BASE_RAFT, 0.f);
    const indexed_triangle_set pads = generate(TS_MSLICER_SUPPORT_BASE_PADS, 0.f);
    const indexed_triangle_set bare = generate(TS_MSLICER_SUPPORT_BASE_NONE, 0.f);
    CHECK_FALSE(raft.empty());
    CHECK_FALSE(pads.empty());
    CHECK_FALSE(bare.empty());

    // A column standing on nothing must reach the plate itself. The generator
    // used to end every column at a fixed Z of one millimetre - exactly the
    // default base height - which left them floating whenever the base was
    // None or its height was not 1.
    auto lowest = [](const indexed_triangle_set &mesh) {
        float bottom = std::numeric_limits<float>::infinity();
        for (const Vec3f &vertex : mesh.vertices)
            bottom = std::min(bottom, vertex.z());
        return bottom;
    };
    CHECK(lowest(bare) == Catch::Approx(0.f).margin(1e-3));
    CHECK(lowest(raft) == Catch::Approx(0.f).margin(1e-3));
    CHECK(lowest(pads) == Catch::Approx(0.f).margin(1e-3));

    // Each base kind is a different mesh: no base is the smallest, and asking
    // for one adds geometry rather than being quietly ignored.
    CHECK(raft.indices.size() > bare.indices.size());
    CHECK(pads.indices.size() > bare.indices.size());
    CHECK(raft.indices.size() != pads.indices.size());

    // Penetration moves the tip apex into the model, so the support mesh
    // reaches higher than one that only touches the surface.
    auto highest = [](const indexed_triangle_set &mesh) {
        float top = -std::numeric_limits<float>::infinity();
        for (const Vec3f &vertex : mesh.vertices)
            top = std::max(top, vertex.z());
        return top;
    };
    const indexed_triangle_set bitten = generate(TS_MSLICER_SUPPORT_BASE_NONE, 0.5f);
    CHECK(highest(bitten) > highest(bare));
}

TEST_CASE("MSLA minimum overhang area leaves small regions unsupported", "[mslicer][resin][supports]")
{
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));

    auto generate = [&raised](float min_area) {
        MSlicerResinJob job(settings());
        job.add_mesh(cube(), raised);
        MSlicerSupportSettings support;
        support.support_radius_mm = 0.6f;
        support.tip_radius_mm = 0.2f;
        support.tip_length_mm = 1.f;
        support.precision = 8;
        support.base_height_mm = 0.5f;
        support.base_offset_mm = 0.5f;
        support.max_overhang_angle_degrees = 35.f;
        support.face_support_spacing_mm = 2.f;
        support.edge_support_spacing_mm = 2.f;
        support.min_support_height_mm = 1.2f;
        support.min_overhang_area_mm2 = min_area;
        return job.generate_supports(support);
    };

    const indexed_triangle_set every = generate(0.f);
    CHECK_FALSE(every.empty());

    // An area larger than the whole cube's underside leaves that region out.
    // The isolated-point rule still applies, so this asserts "fewer", not
    // "none": dropping the points that cannot print at all would be wrong.
    const indexed_triangle_set sparse = generate(1.0e6f);
    CHECK(sparse.indices.size() < every.indices.size());
}

TEST_CASE("MSLA supports can rest on the model instead of the plate", "[mslicer][resin][supports]")
{
    // A table: one cube on the plate, a smaller cube floating directly above
    // its middle. The upper cube is shrunk so that even the rim supports -
    // whose tips lean diagonally outward - still start above the lower cube
    // and have clear model below.
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(4., 4., 8.));
    raised.scale(0.6);

    auto generate = [&raised](bool on_model) {
        MSlicerResinJob job(settings());
        job.add_mesh(cube());
        job.add_mesh(cube(), raised);
        MSlicerSupportSettings support;
        support.support_radius_mm = 0.6f;
        support.tip_radius_mm = 0.2f;
        support.tip_length_mm = 1.f;
        support.precision = 8;
        support.base_height_mm = 0.5f;
        support.base_offset_mm = 0.5f;
        support.max_overhang_angle_degrees = 35.f;
        support.face_support_spacing_mm = 2.f;
        support.edge_support_spacing_mm = 2.f;
        support.min_support_height_mm = 1.2f;
        support.base = TS_MSLICER_SUPPORT_BASE_NONE;
        support.support_on_model = on_model;
        return job.generate_supports(support);
    };

    auto lowest = [](const indexed_triangle_set &mesh) {
        float bottom = std::numeric_limits<float>::infinity();
        for (const Vec3f &vertex : mesh.vertices)
            bottom = std::min(bottom, vertex.z());
        return bottom;
    };

    // Routed to the plate, the columns walk out from under the lower cube and
    // reach Z=0. Resting on the model, every column ends on the lower cube's
    // top face at Z=2 and nothing reaches the plate.
    const indexed_triangle_set routed = generate(false);
    const indexed_triangle_set rested = generate(true);
    CHECK_FALSE(routed.empty());
    CHECK_FALSE(rested.empty());
    CHECK(lowest(routed) == Catch::Approx(0.f).margin(1e-3));
    CHECK(lowest(rested) > 1.5f);
    // No volume comparison here: the two routes support different column
    // counts, because an escape walk that fails drops its candidate entirely
    // while resting on the model keeps it.
}

TEST_CASE("MSLA cross bracing and adaptive diameter add real geometry", "[mslicer][resin][supports]")
{
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 12.));

    auto generate = [&raised](bool bracing, bool adaptive) {
        MSlicerResinJob job(settings());
        job.add_mesh(cube(), raised);
        MSlicerSupportSettings support;
        support.support_radius_mm = 0.6f;
        support.tip_radius_mm = 0.2f;
        support.tip_length_mm = 1.f;
        support.precision = 8;
        support.base_height_mm = 0.5f;
        support.base_offset_mm = 0.5f;
        support.max_overhang_angle_degrees = 35.f;
        support.face_support_spacing_mm = 2.f;
        support.edge_support_spacing_mm = 2.f;
        support.min_support_height_mm = 1.2f;
        support.base = TS_MSLICER_SUPPORT_BASE_NONE;
        support.cross_bracing = bracing;
        support.brace_spacing_mm = 6.f;
        support.adaptive_diameter = adaptive;
        support.max_pillar_diameter_mm = 6.f;
        return job.generate_supports(support);
    };

    const indexed_triangle_set plain = generate(false, false);
    CHECK_FALSE(plain.empty());

    // Bracing adds the X struts as triangles of their own.
    const indexed_triangle_set braced = generate(true, false);
    CHECK(braced.indices.size() > plain.indices.size());

    // Adaptive thickness keeps the topology but thickens tall shafts, so the
    // same columns enclose more volume.
    const indexed_triangle_set thick = generate(false, true);
    CHECK(thick.indices.size() == plain.indices.size());
    CHECK(its_volume(thick) > its_volume(plain));
}

TEST_CASE("MSLA manual support points are honoured, alone or with detection", "[mslicer][resin][supports]")
{
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));
    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 1.f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 0.5f;
    support.max_overhang_angle_degrees = 35.f;
    support.face_support_spacing_mm = 4.f;
    support.edge_support_spacing_mm = 4.f;
    support.min_support_height_mm = 1.2f;
    support.base = TS_MSLICER_SUPPORT_BASE_NONE;

    auto generate = [&](const std::vector<Vec3f> &points, bool manual_only) {
        MSlicerResinJob job(settings());
        job.add_mesh(cube(), raised);
        job.set_manual_support_points(points, manual_only);
        return job.generate_supports(support);
    };

    const indexed_triangle_set automatic = generate({}, false);
    CHECK_FALSE(automatic.empty());

    // A manual point near the underside adds a column on top of detection.
    const std::vector<Vec3f> one_point { Vec3f(10.f, 10.f, 4.8f) };
    const indexed_triangle_set added = generate(one_point, false);
    CHECK(added.indices.size() > automatic.indices.size());

    // Manual-only supports exactly the requested point: far fewer columns
    // than detection places, but never nothing.
    const indexed_triangle_set only = generate(one_point, true);
    CHECK_FALSE(only.empty());
    CHECK(only.indices.size() < automatic.indices.size());

    // Manual-only with nothing placed generates nothing; the GUI's prepare
    // path turns that empty result into an explicit refusal dialog rather
    // than an empty prepared object.
    CHECK(generate({}, true).empty());
}

TEST_CASE("MSLA orientation suggestion prefers less overhang and leaves good scenes alone", "[mslicer][resin][supports]")
{
    auto box = [](float x0, float x1, float y0, float y1, float z0, float z1) {
        indexed_triangle_set mesh;
        mesh.vertices = {
            {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
            {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}
        };
        mesh.indices = {
            {0,2,1}, {0,3,2}, {4,5,6}, {4,6,7},
            {0,1,5}, {0,5,4}, {1,2,6}, {1,6,5},
            {2,3,7}, {2,7,6}, {3,0,4}, {3,4,7}
        };
        return mesh;
    };

    // A cube flat on the plate has nothing overhanging: the identity always
    // competes, so the suggestion must be to leave it alone.
    {
        MSlicerResinJob job(settings());
        job.add_mesh(box(5.f, 15.f, 5.f, 15.f, 0.f, 5.f));
        const Vec3d suggestion = job.suggest_orientation(30.);
        CHECK(suggestion.isApprox(Vec3d::Zero(), 1e-6));
    }

    // A shelf: a wall on the plate carrying a large horizontal plate whose
    // underside is pure overhang. Some rotation must do better than leaving
    // that underside facing down.
    {
        MSlicerResinJob job(settings());
        job.add_mesh(box(5.f, 7.f, 5.f, 15.f, 0.f, 12.f));
        job.add_mesh(box(7.f, 25.f, 5.f, 15.f, 10.f, 12.f));
        const Vec3d suggestion = job.suggest_orientation(30.);
        CHECK_FALSE(suggestion.isApprox(Vec3d::Zero(), 1e-6));
    }
}

TEST_CASE("MSLA supports skip overhangs the model already holds up", "[mslicer][resin][supports]")
{
    auto support = []() {
        MSlicerSupportSettings s;
        s.support_radius_mm = 0.6f;
        s.tip_radius_mm = 0.2f;
        s.tip_length_mm = 1.f;
        s.precision = 8;
        s.base_height_mm = 0.5f;
        s.base_offset_mm = 0.5f;
        s.max_overhang_angle_degrees = 35.f;
        s.face_support_spacing_mm = 2.f;
        s.edge_support_spacing_mm = 2.f;
        s.min_support_height_mm = 1.2f;
        s.base = TS_MSLICER_SUPPORT_BASE_NONE;
        return s;
    };
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));

    // The same floating slab twice; the second time half its underside sits
    // on a pedestal. The pedestal's edge anchors the nearby underside - the
    // resin can grow outward from it - so that half needs far less, while the
    // free half keeps its columns.
    indexed_triangle_set slab = cube(); // 10 x 10, raised to z 5..7 (scaled)
    MSlicerResinJob floating(settings());
    floating.add_mesh(slab, raised);
    const indexed_triangle_set unpruned = floating.generate_supports(support());
    REQUIRE_FALSE(unpruned.empty());

    indexed_triangle_set pedestal;
    pedestal.vertices = {
        {5.f, 5.f, 0.f}, {10.f, 5.f, 0.f}, {10.f, 15.f, 0.f}, {5.f, 15.f, 0.f},
        {5.f, 5.f, 5.f}, {10.f, 5.f, 5.f}, {10.f, 15.f, 5.f}, {5.f, 15.f, 5.f}
    };
    pedestal.indices = {
        {0,2,1}, {0,3,2}, {4,5,6}, {4,6,7},
        {0,1,5}, {0,5,4}, {1,2,6}, {1,6,5},
        {2,3,7}, {2,7,6}, {3,0,4}, {3,4,7}
    };
    MSlicerResinJob propped(settings());
    propped.add_mesh(slab, raised);
    propped.add_mesh(pedestal);
    const indexed_triangle_set pruned = propped.generate_supports(support());
    REQUIRE_FALSE(pruned.empty());
    CHECK(its_volume(pruned) < its_volume(unpruned));
}

TEST_CASE("MSlicer automatic supports ignore model faces already on the build plate", "[mslicer][resin][supports]")
{
    MSlicerResinJob job(settings());
    job.add_mesh(cube());
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));
    job.add_mesh(cube(), raised);

    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 2.5f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 0.5f;
    support.max_overhang_angle_degrees = 35.f;
    support.face_support_spacing_mm = 2.f;
    support.edge_support_spacing_mm = 2.f;

    indexed_triangle_set prepared_supports;
    CHECK_NOTHROW(prepared_supports = job.generate_supports(support));
    CHECK_FALSE(prepared_supports.empty());
    CHECK_NOTHROW(job.slice());
    CHECK(job.diagnostics_json().find("generated automatic MSLA supports") != std::string::npos);
}

TEST_CASE("MSlicer resin job applies printer image transforms before preview and export", "[mslicer][resin]")
{
    MSlicerResinSettings transformed = settings();
    transformed.portrait = true;
    transformed.mirror_x = true;
    MSlicerResinJob job(transformed);
    job.add_mesh(cube());
    job.slice();

    REQUIRE(job.layer(0).size() == 48 * 64);
    REQUIRE(job.manifest_json().find("\"resolution\":[48,64]") != std::string::npos);
    REQUIRE(job.manifest_json().find("\"mirror_x\":true") != std::string::npos);
    REQUIRE_FALSE(job.encode(TS_MSLICER_FORMAT_GOO).empty());
    const std::vector<uint8_t> svg = job.encode(TS_MSLICER_FORMAT_SVG);
    const std::string svg_text(svg.begin(), svg.end());
    REQUIRE(svg_text.find("width=\"48mm\"") != std::string::npos);
    REQUIRE(svg_text.find("height=\"64mm\"") != std::string::npos);
}

TEST_CASE("native MSlicer Resin print owns apply process and output naming", "[mslicer][resin]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("display_pixels_x", new ConfigOptionInt(64));
    config.set_key_value("display_pixels_y", new ConfigOptionInt(48));
    config.set_key_value("display_width", new ConfigOptionFloat(32.));
    config.set_key_value("display_height", new ConfigOptionFloat(24.));
    config.set_key_value("printable_height", new ConfigOptionFloat(20.));
    config.set_key_value("layer_height", new ConfigOptionFloat(1.));
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(false));
    config.set_key_value("sla_archive_format", new ConfigOptionString("goo"));
    config.set_key_value("output_filename_format", new ConfigOptionString("[input_filename_base]"));

    Model model;
    ModelObject *object = model.add_object("cube", "cube.stl", TriangleMesh(cube()));
    object->add_instance();

    MSlicerResinPrint print;
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_UNCHANGED);
    model.objects.front()->instances.front()->set_offset(X, 1.);
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK(print.validate().string.empty());
    CHECK_FALSE(print.empty());
    CHECK(print.output_filename() == "cube.goo");
    print.process();
    CHECK(print.finished());
    REQUIRE(print.job() != nullptr);
    CHECK(print.job()->layer_count() == 2);
    CHECK_FALSE(print.encode().empty());

    // A plate with no prepared supports slices as it is: there is no enable
    // flag left that could claim supports were wanted but missing.
    config.set_key_value("support_points_density_relative", new ConfigOptionInt(10000));
    model.objects.front()->instances.front()->set_offset(Z, 5.);
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK(print.validate().string.empty());

    MSlicerResinJob preparation(MSlicerResinJob::settings_from_config(config));
    preparation.add_model(model, false);
    indexed_triangle_set prepared_supports =
        preparation.generate_supports(MSlicerResinJob::support_settings_from_config(config));
    REQUIRE_FALSE(prepared_supports.empty());
    ModelObject *support_object = model.add_object(
        "Generated MSLA Supports",
        MSlicerResinJob::prepared_support_input_file(),
        TriangleMesh(std::move(prepared_supports)));
    support_object->add_instance();

    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK(print.validate().string.empty());
    print.process();
    REQUIRE(print.job() != nullptr);
    CHECK(MSlicerResinJob::has_prepared_supports(model));
    CHECK_FALSE(print.encode().empty());
}

TEST_CASE("MSlicer settings are first-class SLA preset options with safe defaults", "[mslicer][resin]")
{
    const SLAFullPrintConfig defaults = SLAFullPrintConfig::defaults();
    REQUIRE(defaults.mslicer_bottom_layers.value == 3);
    REQUIRE(defaults.mslicer_transition_layers.value == 10);
    REQUIRE(defaults.mslicer_supersample.value == 1);
    REQUIRE(defaults.mslicer_normal_pwm.value == 255);
    REQUIRE(defaults.mslicer_bottom_pwm.value == 255);
    REQUIRE(defaults.mslicer_normal_lift_speed.value > 0.);
    REQUIRE(defaults.mslicer_bottom_retract_speed.value > 0.);

    const auto &print_options = Preset::sla_print_options();
    const auto &material_options = Preset::sla_material_options();
    REQUIRE(std::find(print_options.begin(), print_options.end(), "mslicer_supersample") != print_options.end());
    REQUIRE(std::find(material_options.begin(), material_options.end(), "mslicer_normal_wait_time") != material_options.end());
    REQUIRE(std::find(material_options.begin(), material_options.end(), "mslicer_bottom_retract_speed") != material_options.end());

    DynamicPrintConfig config;
    config.apply(defaults);
    const MSlicerResinSettings mapped = MSlicerResinJob::settings_from_config(config);
    CHECK(mapped.resolution_x == uint32_t(defaults.display_pixels_x.value));
    CHECK(mapped.resolution_y == uint32_t(defaults.display_pixels_y.value));
    CHECK(mapped.layer_height_mm == Catch::Approx(defaults.layer_height.value));
    CHECK(mapped.normal.exposure_seconds == Catch::Approx(defaults.exposure_time.value));
    CHECK(mapped.bottom.exposure_seconds == Catch::Approx(defaults.initial_exposure_time.value));
    CHECK(mapped.deterministic_seed == 0x5449474552ULL);

    DynamicPrintConfig format_config;
    format_config.apply(defaults);
    format_config.set_key_value("sla_archive_format", new ConfigOptionString("GOO"));
    CHECK(MSlicerResinJob::format_from_config(format_config) == TS_MSLICER_FORMAT_GOO);
    format_config.set_key_value("sla_archive_format", new ConfigOptionString("sl1"));
    CHECK_THROWS_AS(MSlicerResinJob::format_from_config(format_config), std::invalid_argument);
}

TEST_CASE("MSLA hollowing empties the interior and drain holes pierce it", "[mslicer][resin][hollowing]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(true));
    config.set_key_value("hollowing_min_thickness", new ConfigOptionFloat(2.));

    Model model;
    ModelObject *object = model.add_object("cube", "", TriangleMesh(its_make_cube(20., 20., 20.)));
    object->add_instance();

    // Hollowing must be accepted by validation now, not refused.
    MSlicerResinJob::validate_model_input(model, config);

    const double solid_volume = its_volume(object->raw_indexed_triangle_set());
    const indexed_triangle_set hollowed = MSlicerResinJob::prepared_object_mesh(*object, config);
    const double hollowed_volume = its_volume(hollowed);
    // A 20 mm cube with 2 mm walls keeps well under half its material; it must
    // also keep clearly more than nothing.
    CHECK(hollowed_volume < 0.55 * solid_volume);
    CHECK(hollowed_volume > 0.05 * solid_volume);

    // A drain hole through the top wall removes further material.
    object->sla_drain_holes.emplace_back(Vec3f(10.f, 10.f, 20.f), Vec3f(0.f, 0.f, -1.f), 2.f, 6.f);
    const double drilled_volume = its_volume(MSlicerResinJob::prepared_object_mesh(*object, config));
    CHECK(drilled_volume < hollowed_volume);

    // The same two holes drilled into the solid cube: no hollowing, still a cut.
    config.set_key_value("hollowing_enable", new ConfigOptionBool(false));
    const double solid_drilled = its_volume(MSlicerResinJob::prepared_object_mesh(*object, config));
    CHECK(solid_drilled < solid_volume);
    CHECK(solid_drilled > 0.9 * solid_volume);
}

TEST_CASE("MSLA hollowing honours the per-object override", "[mslicer][resin][hollowing]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_min_thickness", new ConfigOptionFloat(2.));

    Model model;
    ModelObject *object = model.add_object("cube", "", TriangleMesh(its_make_cube(20., 20., 20.)));
    object->add_instance();
    object->config.set_key_value("hollowing_enable", new ConfigOptionBool(true));

    MSlicerResinJob::validate_model_input(model, config);
    const double solid_volume = its_volume(object->raw_indexed_triangle_set());
    const double prepared_volume = its_volume(MSlicerResinJob::prepared_object_mesh(*object, config));
    CHECK(prepared_volume < 0.55 * solid_volume);
}

TEST_CASE("MSLA hollowed plate slices end to end", "[mslicer][resin][hollowing]")
{
    // The whole pipeline: a hollowed, drilled cube through add_model with the
    // preparation config, sliced by the engine.
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(true));
    config.set_key_value("hollowing_min_thickness", new ConfigOptionFloat(2.));

    Model model;
    ModelObject *object = model.add_object("cube", "", TriangleMesh(its_make_cube(16., 16., 16.)));
    object->add_instance();
    object->sla_drain_holes.emplace_back(Vec3f(8.f, 8.f, 16.f), Vec3f(0.f, 0.f, -1.f), 2.f, 6.f);

    MSlicerResinJob job(settings());
    CHECK(job.add_model(model, true, &config) == 1);
    job.slice();
    CHECK(job.layer_count() > 0);
    // A mid-height layer of a hollowed cube is a ring: it must contain lit
    // wall pixels and a dark cavity between them.
    const std::vector<uint8_t> layer = job.layer(job.layer_count() / 2);
    REQUIRE_FALSE(layer.empty());
    const size_t lit = std::count_if(layer.begin(), layer.end(), [](uint8_t v) { return v > 0; });
    CHECK(lit > 0);
    CHECK(lit < layer.size());
}

TEST_CASE("MSLA raster compensation values flow from the configuration", "[mslicer][resin][compensation]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("absolute_correction", new ConfigOptionFloat(-0.1));
    config.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(0.2));
    const MSlicerResinSettings mapped = MSlicerResinJob::settings_from_config(config);
    CHECK(mapped.absolute_correction_mm == Catch::Approx(-0.1f));
    CHECK(mapped.elefant_foot_compensation_mm == Catch::Approx(0.2f));

    // Negative elephant foot and Z correction stay refused.
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(false));
    Model model;
    model.add_object("cube", "", TriangleMesh(its_make_cube(10., 10., 10.)))->add_instance();
    MSlicerResinJob::validate_model_input(model, config);
    config.set_key_value("zcorrection_layers", new ConfigOptionInt(2));
    CHECK_THROWS_WITH(MSlicerResinJob::validate_model_input(model, config),
        Catch::Matchers::ContainsSubstring("Z correction"));
    config.set_key_value("zcorrection_layers", new ConfigOptionInt(0));
    config.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(-0.2));
    CHECK_THROWS_WITH(MSlicerResinJob::validate_model_input(model, config),
        Catch::Matchers::ContainsSubstring("Elephant-foot"));
}

TEST_CASE("MSLA scaling corrections scale the placed geometry in place", "[mslicer][resin][compensation]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(false));
    config.set_key_value("relative_correction_x", new ConfigOptionFloat(1.5));
    config.set_key_value("material_correction_y", new ConfigOptionFloat(2.0));

    Model model;
    ModelObject *object = model.add_object("cube", "", TriangleMesh(its_make_cube(10., 10., 10.)));
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(11., 7., 0.));

    MSlicerResinJob job(settings());
    CHECK(job.add_model(model, true, &config) == 1);
    const Vec3f size = job.input_size_mm();
    CHECK(size.x() == Catch::Approx(15.f));
    CHECK(size.y() == Catch::Approx(20.f));
    CHECK(size.z() == Catch::Approx(10.f));
    // Scaled about its own footprint: the XY centre stays where the instance
    // put it, and the part still rests on the plate.
    const Vec3f center = 0.5f * (job.input_min_mm() + job.input_max_mm());
    CHECK(center.x() == Catch::Approx(16.f));
    CHECK(center.y() == Catch::Approx(12.f));
    CHECK(job.input_min_mm().z() == Catch::Approx(0.f));
}


TEST_CASE("MSLA support blockers and enforcers steer automatic placement", "[mslicer][resin][supports]")
{
    // The raised cube from the generation test: it needs supports everywhere
    // under its bottom face.
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));
    MSlicerSupportSettings support;
    support.support_radius_mm = 0.6f;
    support.tip_radius_mm = 0.2f;
    support.tip_length_mm = 1.f;
    support.precision = 8;
    support.base_height_mm = 0.5f;
    support.base_offset_mm = 0.5f;
    support.max_overhang_angle_degrees = 35.f;
    support.face_support_spacing_mm = 2.f;
    support.edge_support_spacing_mm = 2.f;

    // Baseline: supports are generated.
    MSlicerResinJob plain(settings());
    plain.add_mesh(cube(), raised);
    CHECK_FALSE(plain.generate_supports(support).empty());

    // A blocker enclosing the whole scene: nothing may be placed.
    MSlicerResinJob blocked(settings());
    blocked.add_mesh(cube(), raised);
    blocked.add_support_region(its_make_cube(40., 40., 20.),
                               Transform3d(Eigen::Translation3d(-4., -8., 0.)), true);
    CHECK(blocked.generate_supports(support).empty());

    // Enforcer-only with an enforcer covering the scene: supports return.
    support.enforcers_only = true;
    MSlicerResinJob enforced(settings());
    enforced.add_mesh(cube(), raised);
    enforced.add_support_region(its_make_cube(40., 40., 20.),
                                Transform3d(Eigen::Translation3d(-4., -8., 0.)), false);
    CHECK_FALSE(enforced.generate_supports(support).empty());

    // Enforcer-only with no enforcers: nothing is placed.
    MSlicerResinJob starved(settings());
    starved.add_mesh(cube(), raised);
    CHECK(starved.generate_supports(support).empty());
}

TEST_CASE("MSLA pre-flight reports islands that print in mid-air", "[mslicer][resin][islands]")
{
    // A raised cube with no supports: every first pixel of it prints onto
    // nothing but film.
    MSlicerResinJob floating(settings());
    Transform3d raised = Transform3d::Identity();
    raised.translate(Vec3d(0., 0., 5.));
    floating.add_mesh(cube(), raised);
    floating.slice();
    CHECK(floating.diagnostics_json().find("unsupported island") != std::string::npos);

    // The same cube on the plate: nothing to report.
    MSlicerResinJob grounded(settings());
    grounded.add_mesh(cube());
    grounded.slice();
    CHECK(grounded.diagnostics_json().find("unsupported island") == std::string::npos);
}

TEST_CASE("MSLA pre-flight reports trapped air pockets", "[mslicer][resin][voids]")
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("supports_enable", new ConfigOptionBool(false));
    config.set_key_value("pad_enable", new ConfigOptionBool(false));
    config.set_key_value("hollowing_enable", new ConfigOptionBool(true));
    config.set_key_value("hollowing_min_thickness", new ConfigOptionFloat(2.));

    // A hollowed cube with no drain holes: the cavity is enclosed on every
    // side and traps uncured resin.
    Model sealed_model;
    sealed_model.add_object("cube", "", TriangleMesh(its_make_cube(16., 16., 16.)))->add_instance();
    MSlicerResinJob sealed_job(settings());
    CHECK(sealed_job.add_model(sealed_model, true, &config) == 1);
    sealed_job.slice();
    CHECK(sealed_job.diagnostics_json().find("sealed void") != std::string::npos);

    // The same cube with a drain hole through a side wall: the cavity
    // vents through the hole channel and nothing is reported.
    Model drilled_model;
    ModelObject *object =
        drilled_model.add_object("cube", "", TriangleMesh(its_make_cube(16., 16., 16.)));
    object->add_instance();
    object->sla_drain_holes.emplace_back(Vec3f(0.f, 8.f, 8.f), Vec3f(1.f, 0.f, 0.f), 2.f, 6.f);
    MSlicerResinJob drilled(settings());
    CHECK(drilled.add_model(drilled_model, true, &config) == 1);
    drilled.slice();
    CHECK(drilled.diagnostics_json().find("sealed void") == std::string::npos);
    CHECK(drilled.diagnostics_json().find("suction cup") == std::string::npos);
}
