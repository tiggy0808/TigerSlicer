#include "MSlicerBridge.hpp"

#include <cassert>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

using Slic3r::MSlicer::Engine;
using Slic3r::MSlicer::Remote;

static uint8_t cancelled(void *) { return 1; }

static uint64_t fnv1a(const uint8_t *data, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static ts_mslicer_exposure exposure()
{
    ts_mslicer_exposure value {};
    value.exposure_seconds = 2.5f;
    value.delay_seconds = 0.5f;
    value.pwm = 255;
    value.lift_distance_mm = 5.f;
    value.lift_speed_mm_min = 120.f;
    value.retract_distance_mm = 5.f;
    value.retract_speed_mm_min = 180.f;
    return value;
}

int main()
{
    try {
        assert(Engine::abi_version() == TS_MSLICER_ABI_VERSION);
        assert((Engine::capabilities() & TS_MSLICER_CAP_RASTER_SLICE) != 0);
        assert((Engine::capabilities() & TS_MSLICER_CAP_SDCP_REMOTE) != 0);
        assert((Engine::capabilities() & TS_MSLICER_CAP_AUTO_SUPPORTS) != 0);
        assert((Engine::capabilities() & TS_MSLICER_CAP_PREPARED_SUPPORT_MESH) != 0);
        assert(Engine::version().find("be44e0b") != std::string::npos);

        const std::string private_address = "192.168.1.10";
        ts_mslicer_remote_probe_request probe {};
        probe.struct_size = sizeof(probe);
        probe.address = reinterpret_cast<const uint8_t *>(private_address.data());
        probe.address_size = private_address.size();
        probe.port = 3000;
        probe.resolution_x = 3840;
        probe.resolution_y = 2400;
        probe.timeout_ms = 1000;
        probe.cancelled = cancelled;
        bool cancelled_probe = false;
        try {
            (void)Remote::probe(probe);
        } catch (const Slic3r::MSlicer::Error &error) {
            cancelled_probe = error.status() == TS_MSLICER_CANCELLED;
        }
        assert(cancelled_probe);
        const std::string public_address = "8.8.8.8";
        probe.address = reinterpret_cast<const uint8_t *>(public_address.data());
        probe.address_size = public_address.size();
        probe.cancelled = nullptr;
        bool public_rejected = false;
        try {
            (void)Remote::probe(probe);
        } catch (const Slic3r::MSlicer::Error &error) {
            public_rejected = error.status() == TS_MSLICER_INVALID_ARGUMENT;
        }
        assert(public_rejected);

        const std::string filename = "part.goo";
        const std::array<uint8_t, 4> artifact { 1, 2, 3, 4 };
        ts_mslicer_remote_upload_request upload {};
        upload.struct_size = sizeof(upload);
        upload.address = reinterpret_cast<const uint8_t *>(private_address.data());
        upload.address_size = private_address.size();
        upload.filename = reinterpret_cast<const uint8_t *>(filename.data());
        upload.filename_size = filename.size();
        upload.artifact = artifact.data();
        upload.artifact_size = artifact.size();
        upload.port = 3000;
        upload.resolution_x = 3840;
        upload.resolution_y = 2400;
        upload.probe_timeout_ms = 1000;
        upload.connect_timeout_ms = 1000;
        upload.transfer_timeout_ms = 1000;
        upload.cancelled = cancelled;
        bool cancelled_upload = false;
        try {
            (void)Remote::upload(upload);
        } catch (const Slic3r::MSlicer::Error &error) {
            cancelled_upload = error.status() == TS_MSLICER_CANCELLED;
        }
        assert(cancelled_upload);

        ts_mslicer_config config {};
        config.struct_size = sizeof(config);
        config.resolution_x = 64;
        config.resolution_y = 64;
        config.platform_x_mm = 20.f;
        config.platform_y_mm = 20.f;
        config.platform_z_mm = 20.f;
        config.layer_height_mm = 1.f;
        config.supersample = 1;
        config.first_layers = 1;
        config.normal = exposure();
        config.first = exposure();
        config.deterministic_seed = 42;

        Engine engine(config);
        const float vertices[] = {
            5,5,0, 15,5,0, 15,15,0, 5,15,0,
            5,5,2, 15,5,2, 15,15,2, 5,15,2
        };
        const uint32_t triangles[] = {
            0,2,1, 0,3,2, 4,5,6, 4,6,7,
            0,1,5, 0,5,4, 1,2,6, 1,6,5,
            2,3,7, 2,7,6, 3,0,4, 3,4,7
        };
        ts_mslicer_mesh mesh {};
        mesh.struct_size = sizeof(mesh);
        mesh.vertices_xyz = vertices;
        mesh.vertex_count = 8;
        mesh.triangles = triangles;
        mesh.triangle_count = 12;
        const float identity[] = {
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
        };
        std::memcpy(mesh.transform, identity, sizeof(identity));
        mesh.relative_exposure = 255;

        engine.add_mesh(mesh);
        assert(!engine.diagnostics().empty());
        engine.slice();
        assert(engine.layer_count() == 2);
        assert(engine.layer(0).size() == 64 * 64);
        assert(engine.metrics().lit_voxels > 0);
        assert(!engine.manifest().empty());
        const std::array<size_t, 4> golden_sizes { 1417, 196032, 4460, 663 };
        const std::array<uint64_t, 4> golden_hashes {
            UINT64_C(520540019157371930), UINT64_C(5138111534194683992),
            UINT64_C(5861227171552896705), UINT64_C(4216704901519362810)
        };
        size_t golden = 0;
        for (const ts_mslicer_format format : { TS_MSLICER_FORMAT_CTB, TS_MSLICER_FORMAT_GOO,
                                                TS_MSLICER_FORMAT_NANODLP, TS_MSLICER_FORMAT_SVG }) {
            auto first = engine.encode(format);
            auto second = engine.encode(format);
            assert(!first.empty());
            assert(first.size() == golden_sizes[golden]);
            assert(fnv1a(first.data(), first.size()) == golden_hashes[golden]);
            assert(first.size() == second.size());
            assert(std::memcmp(first.data(), second.data(), first.size()) == 0);
            assert(!engine.inspect_output(format, first.data(), first.size()).empty());
            std::vector<uint8_t> truncated(first.data(), first.data() + first.size() / 2);
            bool rejected = false;
            try {
                (void)engine.inspect_output(format, truncated.data(), truncated.size());
            } catch (const Slic3r::MSlicer::Error &) {
                rejected = true;
            }
            assert(rejected);

            std::vector<uint8_t> wrong_version_or_integrity(first.data(), first.data() + first.size());
            if (format == TS_MSLICER_FORMAT_CTB)
                wrong_version_or_integrity[16] = 4;
            else if (format == TS_MSLICER_FORMAT_GOO)
                wrong_version_or_integrity[0] = 'X';
            else if (format == TS_MSLICER_FORMAT_NANODLP)
                wrong_version_or_integrity[wrong_version_or_integrity.size() / 2] ^= 0x5a;
            else
                wrong_version_or_integrity[0] = 'X';
            rejected = false;
            try {
                (void)engine.inspect_output(format, wrong_version_or_integrity.data(), wrong_version_or_integrity.size());
            } catch (const Slic3r::MSlicer::Error &) {
                rejected = true;
            }
            assert(rejected);
            ++golden;
        }
        std::cout << Engine::version() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
