#include "MSlicerBridge.hpp"

#include <cstring>
#include <utility>

namespace Slic3r::MSlicer {

Error::Error(ts_mslicer_status status, std::string message)
    : std::runtime_error(std::move(message)), m_status(status)
{
}

Buffer::~Buffer()
{
    ts_mslicer_buffer_release(&m_buffer);
}

Buffer::Buffer(Buffer &&other) noexcept : m_buffer(other.m_buffer)
{
    other.m_buffer = { nullptr, 0, 0 };
}

Buffer &Buffer::operator=(Buffer &&other) noexcept
{
    if (this != &other) {
        ts_mslicer_buffer_release(&m_buffer);
        m_buffer = other.m_buffer;
        other.m_buffer = { nullptr, 0, 0 };
    }
    return *this;
}

Engine::Engine(const ts_mslicer_config &config)
{
    validate(config);
    m_engine = ts_mslicer_engine_create(&config);
    if (m_engine == nullptr)
        throw Error(TS_MSLICER_ENGINE_ERROR, "MSlicer engine creation failed");
}

Engine::~Engine()
{
    ts_mslicer_engine_destroy(m_engine);
}

Engine::Engine(Engine &&other) noexcept : m_engine(other.m_engine)
{
    other.m_engine = nullptr;
}

Engine &Engine::operator=(Engine &&other) noexcept
{
    if (this != &other) {
        ts_mslicer_engine_destroy(m_engine);
        m_engine = other.m_engine;
        other.m_engine = nullptr;
    }
    return *this;
}

uint32_t Engine::abi_version() noexcept
{
    return ts_mslicer_abi_version();
}

uint64_t Engine::capabilities() noexcept
{
    return ts_mslicer_capabilities();
}

std::string Engine::version()
{
    const char *value = ts_mslicer_engine_version();
    return value == nullptr ? std::string() : std::string(value);
}

void Engine::validate(const ts_mslicer_config &config)
{
    std::array<char, 512> message {};
    const ts_mslicer_status status = ts_mslicer_config_validate(&config, message.data(), message.size());
    if (status != TS_MSLICER_OK)
        throw Error(status, message.data());
}

void Engine::check(ts_mslicer_status status) const
{
    if (status == TS_MSLICER_OK)
        return;
    // Only adopt the engine's stored message when it actually belongs to this
    // status. Several non-OK returns (e.g. NotReady, BufferTooSmall) never call
    // set_error, so last_message() can still hold an unrelated earlier failure;
    // attaching it here would blame the wrong cause. When it does not match,
    // fall back to the generic status name so provenance stays correct.
    std::string message;
    if (ts_mslicer_engine_last_status(m_engine) == status)
        message = last_message();
    if (message.empty()) {
        const char *name = ts_mslicer_status_name(static_cast<uint32_t>(status));
        message = name == nullptr ? "MSlicer engine error" : name;
    }
    throw Error(status, std::move(message));
}

std::string Engine::last_message() const
{
    const size_t required = ts_mslicer_engine_last_message(m_engine, nullptr, 0);
    if (required <= 1)
        return {};
    std::vector<char> text(required);
    ts_mslicer_engine_last_message(m_engine, text.data(), text.size());
    return text.data();
}

void Engine::add_mesh(const ts_mslicer_mesh &mesh)
{
    check(ts_mslicer_engine_add_mesh(m_engine, &mesh));
}

void Engine::generate_supports(const ts_mslicer_support_config &config)
{
    check(ts_mslicer_engine_generate_supports(m_engine, &config));
}

void Engine::set_manual_support_points(const float *points_xyz, size_t point_count, bool manual_only)
{
    check(ts_mslicer_engine_set_manual_support_points(
        m_engine, points_xyz, point_count, manual_only ? 1u : 0u));
}

std::array<float, 3> Engine::suggest_orientation(float max_overhang_angle_degrees)
{
    std::array<float, 3> rotation { 0.f, 0.f, 0.f };
    check(ts_mslicer_engine_suggest_orientation(
        m_engine, max_overhang_angle_degrees, rotation.data()));
    return rotation;
}

PreparedMesh Engine::generated_support_mesh() const
{
    size_t vertex_count = 0;
    size_t triangle_count = 0;
    check(ts_mslicer_engine_generated_support_mesh_info(
        m_engine, &vertex_count, &triangle_count));
    PreparedMesh output;
    output.vertices.resize(vertex_count);
    output.triangles.resize(triangle_count);
    check(ts_mslicer_engine_copy_generated_support_mesh(
        m_engine,
        output.vertices.empty() ? nullptr : output.vertices.front().data(),
        output.vertices.size() * 3,
        output.triangles.empty() ? nullptr : output.triangles.front().data(),
        output.triangles.size() * 3));
    return output;
}

void Engine::clear_meshes()
{
    check(ts_mslicer_engine_clear_meshes(m_engine));
}

void Engine::slice()
{
    check(ts_mslicer_engine_slice(m_engine));
}

void Engine::request_cancel() noexcept
{
    ts_mslicer_engine_request_cancel(m_engine);
}

float Engine::progress() const noexcept
{
    return ts_mslicer_engine_progress(m_engine);
}

uint32_t Engine::layer_count() const noexcept
{
    return ts_mslicer_engine_layer_count(m_engine);
}

std::vector<uint8_t> Engine::layer(uint32_t index) const
{
    size_t required = 0;
    const ts_mslicer_status probe = ts_mslicer_engine_copy_layer(m_engine, index, nullptr, 0, &required);
    if (probe != TS_MSLICER_BUFFER_TOO_SMALL)
        check(probe);
    std::vector<uint8_t> pixels(required);
    check(ts_mslicer_engine_copy_layer(m_engine, index, pixels.data(), pixels.size(), &required));
    return pixels;
}

ts_mslicer_metrics Engine::metrics() const
{
    ts_mslicer_metrics value {};
    value.struct_size = sizeof(value);
    check(ts_mslicer_engine_metrics(m_engine, &value));
    return value;
}

Buffer Engine::manifest() const
{
    Buffer output;
    check(ts_mslicer_engine_manifest_json(m_engine, output.out()));
    return output;
}

Buffer Engine::diagnostics() const
{
    Buffer output;
    check(ts_mslicer_engine_diagnostics_json(m_engine, output.out()));
    return output;
}

Buffer Engine::encode(ts_mslicer_format format,
                      const uint8_t *preview_rgba,
                      uint32_t preview_width,
                      uint32_t preview_height) const
{
    Buffer output;
    check(ts_mslicer_engine_encode(m_engine, static_cast<uint32_t>(format), preview_rgba,
                                   preview_width, preview_height, output.out()));
    return output;
}

Buffer Engine::inspect_output(ts_mslicer_format format, const uint8_t *data, size_t size) const
{
    Buffer output;
    check(ts_mslicer_engine_inspect_output(m_engine, static_cast<uint32_t>(format), data, size, output.out()));
    return output;
}

Buffer Remote::probe(const ts_mslicer_remote_probe_request &request)
{
    Buffer output;
    const ts_mslicer_status status = ts_mslicer_remote_probe(&request, output.out());
    if (status != TS_MSLICER_OK) {
        const std::string message = output.empty()
            ? std::string()
            : std::string(output.data(), output.data() + output.size());
        throw Error(status, message.empty() ? "SDCP printer probe failed" : message);
    }
    return output;
}

Buffer Remote::upload(const ts_mslicer_remote_upload_request &request)
{
    Buffer output;
    const ts_mslicer_status status = ts_mslicer_remote_upload(&request, output.out());
    if (status != TS_MSLICER_OK) {
        const std::string message = output.empty()
            ? std::string()
            : std::string(output.data(), output.data() + output.size());
        throw Error(status, message.empty() ? "SDCP artifact upload failed" : message);
    }
    return output;
}

} // namespace Slic3r::MSlicer
