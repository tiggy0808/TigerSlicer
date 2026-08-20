#ifndef slic3r_MSlicerBridge_hpp_
#define slic3r_MSlicerBridge_hpp_

#include "../include/tigerslicer_mslicer.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::MSlicer {

struct PreparedMesh {
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
};

class Error : public std::runtime_error
{
public:
    Error(ts_mslicer_status status, std::string message);
    ts_mslicer_status status() const noexcept { return m_status; }

private:
    ts_mslicer_status m_status;
};

class Buffer
{
public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;

    const uint8_t *data() const noexcept { return m_buffer.data; }
    size_t size() const noexcept { return m_buffer.size; }
    bool empty() const noexcept { return m_buffer.size == 0; }

private:
    friend class Engine;
    friend class Remote;
    ts_mslicer_buffer *out() noexcept { return &m_buffer; }
    ts_mslicer_buffer m_buffer { nullptr, 0, 0 };
};

class Remote
{
public:
    static Buffer probe(const ts_mslicer_remote_probe_request &request);
    static Buffer upload(const ts_mslicer_remote_upload_request &request);
};

class Engine
{
public:
    explicit Engine(const ts_mslicer_config &config);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&other) noexcept;
    Engine &operator=(Engine &&other) noexcept;

    static uint32_t abi_version() noexcept;
    static uint64_t capabilities() noexcept;
    static std::string version();
    static void validate(const ts_mslicer_config &config);

    void add_mesh(const ts_mslicer_mesh &mesh);
    void generate_supports(const ts_mslicer_support_config &config);
    // Replaces the manually placed support points (XYZ triples, build-volume
    // millimetres) honoured by the next generate_supports call.
    void set_manual_support_points(const float *points_xyz, size_t point_count, bool manual_only);
    // Euler XYZ radians that minimise the loaded scene's supported overhang
    // area; identity when the scene is already best left alone.
    std::array<float, 3> suggest_orientation(float max_overhang_angle_degrees);
    PreparedMesh generated_support_mesh() const;
    void clear_meshes();
    void slice();
    void request_cancel() noexcept;
    float progress() const noexcept;
    uint32_t layer_count() const noexcept;
    std::vector<uint8_t> layer(uint32_t index) const;
    ts_mslicer_metrics metrics() const;
    Buffer manifest() const;
    Buffer diagnostics() const;
    Buffer encode(ts_mslicer_format format,
                  const uint8_t *preview_rgba = nullptr,
                  uint32_t preview_width = 0,
                  uint32_t preview_height = 0) const;
    Buffer inspect_output(ts_mslicer_format format, const uint8_t *data, size_t size) const;

private:
    void check(ts_mslicer_status status) const;
    std::string last_message() const;
    ts_mslicer_engine *m_engine { nullptr };
};

} // namespace Slic3r::MSlicer

#endif
