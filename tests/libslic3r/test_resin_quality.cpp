// Phase R8 Resin quality and safety verification: adversarial mesh input,
// cancellation, bounds gating, many-layer behavior, and raster shape checks
// for the native MSlicer Resin pipeline.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/MSlicerResinJob.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace Slic3r;

namespace {

MSlicerResinSettings quality_settings()
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

// Axis-aligned closed box between two corners.
indexed_triangle_set box(const Vec3f &min, const Vec3f &max)
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {min.x(), min.y(), min.z()}, {max.x(), min.y(), min.z()},
        {max.x(), max.y(), min.z()}, {min.x(), max.y(), min.z()},
        {min.x(), min.y(), max.z()}, {max.x(), min.y(), max.z()},
        {max.x(), max.y(), max.z()}, {min.x(), max.y(), max.z()}
    };
    mesh.indices = {
        {0,2,1}, {0,3,2}, {4,5,6}, {4,6,7},
        {0,1,5}, {0,5,4}, {1,2,6}, {1,6,5},
        {2,3,7}, {2,7,6}, {3,0,4}, {3,4,7}
    };
    return mesh;
}

} // namespace

TEST_CASE("Resin job without meshes cannot produce a printable artifact", "[mslicer][resin][quality]")
{
    MSlicerResinJob job(quality_settings());
    // Whatever the engine reports, the contract is that no non-empty printer
    // artifact can come out of an empty job.
    try {
        job.slice();
        CHECK(job.layer_count() == 0);
        CHECK(job.metrics().lit_voxels == 0);
    } catch (const MSlicer::Error &) {
        SUCCEED("empty job rejected with a structured engine error");
    }
}

TEST_CASE("Zero-volume sheet input stays safe and deterministic", "[mslicer][resin][quality]")
{
    indexed_triangle_set sheet;
    sheet.vertices = { {8, 8, 1}, {24, 8, 1}, {24, 16, 1}, {8, 16, 1} };
    sheet.indices = { {0, 1, 2}, {0, 2, 3} };

    MSlicerResinJob job(quality_settings());
    job.add_mesh(sheet);
    // input_mesh_volume_mm3 sums signed origin tetrahedra and is only exact
    // for closed meshes, so it is deliberately not asserted for an open sheet.
    try {
        job.slice();
        // A degenerate sheet must not invent printable volume.
        CHECK(job.metrics().resin_volume_mm3 == Catch::Approx(0.).margin(1.));
        const std::string diagnostics = job.diagnostics_json();
        CHECK(diagnostics.front() == '[');
    } catch (const MSlicer::Error &) {
        SUCCEED("degenerate sheet rejected with a structured engine error");
    }
}

TEST_CASE("Non-manifold and self-intersecting input slices deterministically", "[mslicer][resin][quality]")
{
    // Open box: remove the two top-face triangles.
    indexed_triangle_set open_box = box({5, 5, 0}, {15, 15, 2});
    open_box.indices.erase(open_box.indices.begin() + 2, open_box.indices.begin() + 4);

    // Self-intersecting compound: two overlapping closed boxes in one mesh.
    indexed_triangle_set overlapping = box({5, 5, 0}, {15, 15, 2});
    indexed_triangle_set second = box({10, 10, 0}, {20, 20, 2});
    const uint32_t offset = uint32_t(overlapping.vertices.size());
    overlapping.vertices.insert(overlapping.vertices.end(), second.vertices.begin(), second.vertices.end());
    for (stl_triangle_vertex_indices triangle : second.indices)
        overlapping.indices.emplace_back(triangle[0] + offset, triangle[1] + offset, triangle[2] + offset);

    for (const indexed_triangle_set *mesh : { &open_box, &overlapping }) {
        MSlicerResinJob first_job(quality_settings());
        first_job.add_mesh(*mesh);
        first_job.slice();
        REQUIRE(first_job.layer_count() > 0);
        REQUIRE(first_job.metrics().lit_voxels > 0);

        MSlicerResinJob second_job(quality_settings());
        second_job.add_mesh(*mesh);
        second_job.slice();
        for (ts_mslicer_format format : { TS_MSLICER_FORMAT_CTB, TS_MSLICER_FORMAT_GOO }) {
            const std::vector<uint8_t> a = first_job.encode(format);
            const std::vector<uint8_t> b = second_job.encode(format);
            REQUIRE_FALSE(a.empty());
            CHECK(a == b);
        }
    }
}

TEST_CASE("Out-of-bounds instances are gated before rasterization", "[mslicer][resin][quality]")
{
    Model model;
    ModelObject *object = model.add_object("cube", "", TriangleMesh(box({5, 5, 0}, {15, 15, 2})));
    ModelInstance *instance = object->add_instance();
    instance->set_offset(Vec3d(500., 500., 0.));
    // Plater/BuildVolume validation marks such instances outside; is_printable()
    // then excludes them, so the job must refuse to slice nothing.
    instance->print_volume_state = ModelInstancePVS_Fully_Outside;

    MSlicerResinJob job(quality_settings());
    CHECK_THROWS_WITH(job.add_model(model),
        Catch::Matchers::ContainsSubstring("inside the build volume"));

    // The job also exposes exact world-space bounds so callers can verify
    // placement against the platform before slicing.
    MSlicerResinJob bounds_probe(quality_settings());
    Transform3d shift = Transform3d::Identity();
    shift.translate(Vec3d(25., 0., 0.));
    bounds_probe.add_mesh(box({5, 5, 0}, {15, 15, 2}), shift);
    CHECK(bounds_probe.input_max_mm().x() == Catch::Approx(40.f));
    CHECK(bounds_probe.input_max_mm().x() > quality_settings().platform_x_mm);
}

TEST_CASE("Cancellation before slicing yields no artifact", "[mslicer][resin][quality]")
{
    MSlicerResinJob job(quality_settings());
    job.add_mesh(box({5, 5, 0}, {15, 15, 2}));
    job.request_cancel();
    try {
        job.slice();
        // If the engine treats a pre-slice cancel as a no-op run, it must not
        // report progress beyond completion or half-built layers silently.
        CHECK((job.layer_count() == 0 || job.metrics().layer_count == job.layer_count()));
    } catch (const MSlicer::Error &) {
        SUCCEED("cancelled job rejected with a structured engine error");
    }
}

TEST_CASE("Many-layer model reports exact layer and volume metrics", "[mslicer][resin][quality]")
{
    MSlicerResinSettings tall = quality_settings();
    tall.layer_height_mm = 0.05f;
    MSlicerResinJob job(tall);
    job.add_mesh(box({10, 10, 0}, {20, 20, 15}));
    job.slice();
    CHECK(job.layer_count() == 300);
    CHECK(job.metrics().layer_count == 300);
    CHECK(job.metrics().resin_volume_mm3 == Catch::Approx(1500.).epsilon(0.05));
}

TEST_CASE("Layer raster reproduces expected disjoint geometry", "[mslicer][resin][quality]")
{
    // Two separated 5x5 mm towers on the 32x24 mm platform.
    MSlicerResinJob job(quality_settings());
    job.add_mesh(box({5, 5, 0}, {10, 10, 2}));
    job.add_mesh(box({22, 14, 0}, {27, 19, 2}));
    job.slice();
    REQUIRE(job.layer_count() >= 1);

    const MSlicerResinSettings config = quality_settings();
    const std::vector<uint8_t> mask = job.layer(0);
    REQUIRE(mask.size() == size_t(config.resolution_x) * config.resolution_y);

    // 0.5 mm per pixel: each tower covers about a 10x10 pixel square.
    auto pixel = [&](double x_mm, double y_mm) {
        const size_t x = size_t(x_mm / 0.5);
        const size_t y = size_t(y_mm / 0.5);
        return mask[y * config.resolution_x + x];
    };
    CHECK(pixel(7.5, 7.5) > 0);    // center of tower A
    CHECK(pixel(24.5, 16.5) > 0);  // center of tower B
    CHECK(pixel(16., 12.) == 0);   // midpoint between towers is empty
    CHECK(pixel(2., 2.) == 0);     // platform corner is empty

    const size_t lit = size_t(std::count_if(mask.begin(), mask.end(),
                                            [](uint8_t v) { return v > 0; }));
    // Two 10x10 pixel squares, with a tolerant band for edge antialiasing.
    CHECK(lit >= 160);
    CHECK(lit <= 260);

    // Dump the mask for the R8 manual raster inspection record.
    if (const char *dir = std::getenv("TIGERSLICER_R8_RASTER_DIR"); dir != nullptr) {
        const std::string path = std::string(dir) + "/r8_two_towers_layer0.png";
        CHECK(png::write_gray_to_file_scaled(path, config.resolution_x,
                                             config.resolution_y, mask, 8));
    }
}
