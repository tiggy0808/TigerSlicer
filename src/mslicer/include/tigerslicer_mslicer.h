#ifndef TIGERSLICER_MSLICER_H
#define TIGERSLICER_MSLICER_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define TS_MSLICER_ABI_VERSION 11u

typedef struct ts_mslicer_engine ts_mslicer_engine;

typedef uint32_t ts_mslicer_status;
enum {
    TS_MSLICER_OK = 0,
    TS_MSLICER_INVALID_ARGUMENT = 1,
    TS_MSLICER_INVALID_CONFIG = 2,
    TS_MSLICER_INVALID_MESH = 3,
    TS_MSLICER_CANCELLED = 4,
    TS_MSLICER_NOT_READY = 5,
    TS_MSLICER_BUFFER_TOO_SMALL = 6,
    TS_MSLICER_UNSUPPORTED = 7,
    TS_MSLICER_ENGINE_ERROR = 8,
    TS_MSLICER_PANIC = 9
};

typedef uint32_t ts_mslicer_format;
enum {
    TS_MSLICER_FORMAT_CTB = 1,
    TS_MSLICER_FORMAT_GOO = 2,
    TS_MSLICER_FORMAT_NANODLP = 3,
    /* Native vector layer sheet; diagnostic, not a printer machine file. */
    TS_MSLICER_FORMAT_SVG = 4
};

enum ts_mslicer_capability {
    TS_MSLICER_CAP_RASTER_SLICE = UINT64_C(1) << 0,
    TS_MSLICER_CAP_CTB = UINT64_C(1) << 1,
    TS_MSLICER_CAP_GOO = UINT64_C(1) << 2,
    TS_MSLICER_CAP_NANODLP = UINT64_C(1) << 3,
    TS_MSLICER_CAP_LAYER_PREVIEW = UINT64_C(1) << 4,
    TS_MSLICER_CAP_PROGRESS = UINT64_C(1) << 5,
    TS_MSLICER_CAP_CANCELLATION = UINT64_C(1) << 6,
    TS_MSLICER_CAP_METRICS = UINT64_C(1) << 7,
    TS_MSLICER_CAP_MANIFEST = UINT64_C(1) << 8,
    TS_MSLICER_CAP_STRUCTURED_DIAGNOSTICS = UINT64_C(1) << 9,
    TS_MSLICER_CAP_SVG = UINT64_C(1) << 10,
    TS_MSLICER_CAP_OUTPUT_INSPECTION = UINT64_C(1) << 11,
    TS_MSLICER_CAP_SDCP_REMOTE = UINT64_C(1) << 12,
    /* Automatic model-space supports generated before rasterization. */
    TS_MSLICER_CAP_AUTO_SUPPORTS = UINT64_C(1) << 13,
    /* Generated model-space support geometry may be copied before slicing. */
    TS_MSLICER_CAP_PREPARED_SUPPORT_MESH = UINT64_C(1) << 14
};

typedef struct ts_mslicer_exposure {
    float exposure_seconds;
    float delay_seconds;
    uint8_t pwm;
    uint8_t reserved[3];
    float lift_distance_mm;
    float lift_speed_mm_min;
    float retract_distance_mm;
    float retract_speed_mm_min;
} ts_mslicer_exposure;

typedef struct ts_mslicer_config {
    uint32_t struct_size;
    uint32_t resolution_x;
    uint32_t resolution_y;
    float platform_x_mm;
    float platform_y_mm;
    float platform_z_mm;
    float layer_height_mm;
    uint8_t supersample;
    /* 0 = landscape, 1 = rotate output 90 degrees clockwise. */
    uint8_t orientation;
    uint8_t mirror_x;
    uint8_t mirror_y;
    uint32_t first_layers;
    uint32_t transition_layers;
    ts_mslicer_exposure normal;
    ts_mslicer_exposure first;
    uint64_t deterministic_seed;
    /* ABI 11: raster compensation, rounded to whole pixels per axis.
       absolute_correction grows (positive) or shrinks (negative) every
       layer; elefant_foot shrinks the first layer only. */
    float absolute_correction_mm;
    float elefant_foot_compensation_mm;
} ts_mslicer_config;

typedef struct ts_mslicer_mesh {
    uint32_t struct_size;
    const float *vertices_xyz;
    size_t vertex_count;
    const uint32_t *triangles;
    size_t triangle_count;
    /* Column-major affine transform, matching OpenGL and nalgebra storage. */
    float transform[16];
    uint8_t relative_exposure;
    /* ABI 11: 0 = printable model, 1 = support blocker volume, 2 = support
       enforcer volume. Region volumes are never sliced; they steer automatic
       support placement. */
    uint8_t role;
    uint8_t reserved[6];
} ts_mslicer_mesh;

/* What a support column stands on. Every column still routes to the build
   plate; this decides only what is built where it lands. */
enum ts_mslicer_support_base {
    TS_MSLICER_SUPPORT_BASE_NONE = 0,
    TS_MSLICER_SUPPORT_BASE_RAFT = 1,
    TS_MSLICER_SUPPORT_BASE_PADS = 2
};

typedef struct ts_mslicer_support_config {
    uint32_t struct_size;
    float support_radius_mm;
    float tip_radius_mm;
    float tip_length_mm;
    uint32_t precision;
    float base_height_mm;
    float base_offset_mm;
    float max_overhang_angle_degrees;
    float face_support_spacing_mm;
    float edge_support_spacing_mm;
    float edge_angle_delta_radians;
    /* ABI 7. How far the tip apex is pushed into the model past the contact
       point; zero leaves it exactly on the surface. */
    float contact_penetration_mm;
    /* A contact point closer to the plate than this gets no support. */
    float min_support_height_mm;
    /* Connected overhanging regions with less area than this are left
       unsupported. Isolated local minima are never dropped by this rule. */
    float min_overhang_area_mm2;
    /* One of ts_mslicer_support_base. */
    uint32_t base_kind;
    /* Diameter of one pad, used only by TS_MSLICER_SUPPORT_BASE_PADS. */
    float base_diameter_mm;
    /* ABI 8. Nonzero lets a column end on the model directly below its
       contact instead of routing to the plate, with a second tip there. */
    uint32_t support_on_model;
    /* Nonzero adds diagonal X braces between neighbouring plate columns. */
    uint32_t cross_bracing;
    /* Maximum distance between two braced feet; also one brace band's height.
       Must be positive when cross_bracing is set. */
    float brace_spacing_mm;
    /* Nonzero grows the pillar radius with column height: one millimetre of
       radius per 50 mm of height, capped by max_pillar_diameter_mm. */
    uint32_t adaptive_diameter;
    /* Upper bound of the adaptive pillar diameter. Must be at least the
       pillar diameter when adaptive_diameter is set. */
    float max_pillar_diameter_mm;
    /* ABI 11: nonzero restricts automatic contacts to enforcer volumes.
       Manual points are exempt. */
    uint32_t enforcers_only;
} ts_mslicer_support_config;

typedef struct ts_mslicer_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ts_mslicer_buffer;

typedef struct ts_mslicer_metrics {
    uint32_t struct_size;
    uint32_t layer_count;
    uint64_t lit_voxels;
    double resin_volume_mm3;
    double estimated_seconds;
} ts_mslicer_metrics;

typedef uint8_t (*ts_mslicer_cancel_callback)(void *user_data);
typedef void (*ts_mslicer_progress_callback)(void *user_data, uint64_t completed, uint64_t total);
typedef struct ts_mslicer_remote_probe_request {
    uint32_t struct_size;
    const uint8_t *address;
    size_t address_size;
    uint16_t port;
    uint16_t reserved;
    uint32_t resolution_x;
    uint32_t resolution_y;
    uint32_t timeout_ms;
    ts_mslicer_cancel_callback cancelled;
    void *user_data;
} ts_mslicer_remote_probe_request;

typedef struct ts_mslicer_remote_upload_request {
    uint32_t struct_size;
    const uint8_t *address;
    size_t address_size;
    const uint8_t *filename;
    size_t filename_size;
    const uint8_t *artifact;
    size_t artifact_size;
    uint16_t port;
    uint16_t reserved;
    uint32_t resolution_x;
    uint32_t resolution_y;
    uint32_t probe_timeout_ms;
    uint32_t connect_timeout_ms;
    uint32_t transfer_timeout_ms;
    ts_mslicer_cancel_callback cancelled;
    ts_mslicer_progress_callback progress;
    void *user_data;
} ts_mslicer_remote_upload_request;

uint32_t ts_mslicer_abi_version(void);
const char *ts_mslicer_engine_version(void);
uint64_t ts_mslicer_capabilities(void);
/* Accepts an integer so unknown values from untrusted callers remain defined. */
const char *ts_mslicer_status_name(uint32_t status);

ts_mslicer_status ts_mslicer_config_validate(
    const ts_mslicer_config *config,
    char *message,
    size_t message_capacity);

ts_mslicer_engine *ts_mslicer_engine_create(const ts_mslicer_config *config);
void ts_mslicer_engine_destroy(ts_mslicer_engine *engine);
ts_mslicer_status ts_mslicer_engine_last_status(const ts_mslicer_engine *engine);
size_t ts_mslicer_engine_last_message(
    const ts_mslicer_engine *engine,
    char *message,
    size_t message_capacity);

ts_mslicer_status ts_mslicer_engine_add_mesh(
    ts_mslicer_engine *engine,
    const ts_mslicer_mesh *mesh);
/* Generates one support mesh against the complete scene accumulated through
 * add_mesh and appends it to the same immutable slice input. Call once after
 * all printable meshes have been added and before slice. */
ts_mslicer_status ts_mslicer_engine_generate_supports(
    ts_mslicer_engine *engine,
    const ts_mslicer_support_config *config);
/* Queries and copies the most recently generated support mesh in build-volume
 * millimetres. Capacities are element counts: XYZ floats and triangle indices. */
ts_mslicer_status ts_mslicer_engine_generated_support_mesh_info(
    const ts_mslicer_engine *engine,
    size_t *vertex_count,
    size_t *triangle_count);
ts_mslicer_status ts_mslicer_engine_copy_generated_support_mesh(
    const ts_mslicer_engine *engine,
    float *vertices_xyz,
    size_t vertex_float_capacity,
    uint32_t *triangles,
    size_t triangle_index_capacity);
/* ABI 10. Replaces the engine's manually placed support points: point_count
 * XYZ triples in build-volume millimetres, each snapped to the nearest model
 * surface and supported there by the next generate_supports call. Nonzero
 * manual_only suppresses automatic detection so only these points are
 * supported. Passing zero points clears the set. The set also clears with
 * clear_meshes. */
ts_mslicer_status ts_mslicer_engine_set_manual_support_points(
    ts_mslicer_engine *engine,
    const float *points_xyz,
    size_t point_count,
    uint32_t manual_only);
/* ABI 9. Computes a rotation of the complete scene accumulated through
 * add_mesh that minimises the overhanging area the automatic generator would
 * have to support: the summed area of downward faces within
 * max_overhang_angle_degrees of straight down, excluding faces that would
 * rest on the plate. The identity orientation always competes, so the result
 * never loses to leaving the scene alone. Writes euler XYZ radians. */
ts_mslicer_status ts_mslicer_engine_suggest_orientation(
    ts_mslicer_engine *engine,
    float max_overhang_angle_degrees,
    float rotation_xyz_radians[3]);
ts_mslicer_status ts_mslicer_engine_clear_meshes(ts_mslicer_engine *engine);
ts_mslicer_status ts_mslicer_engine_slice(ts_mslicer_engine *engine);
void ts_mslicer_engine_request_cancel(ts_mslicer_engine *engine);
float ts_mslicer_engine_progress(const ts_mslicer_engine *engine);

uint32_t ts_mslicer_engine_layer_count(const ts_mslicer_engine *engine);
ts_mslicer_status ts_mslicer_engine_copy_layer(
    const ts_mslicer_engine *engine,
    uint32_t layer,
    uint8_t *pixels,
    size_t pixel_capacity,
    size_t *required_size);
ts_mslicer_status ts_mslicer_engine_metrics(
    const ts_mslicer_engine *engine,
    ts_mslicer_metrics *metrics);
ts_mslicer_status ts_mslicer_engine_manifest_json(
    const ts_mslicer_engine *engine,
    ts_mslicer_buffer *json);
ts_mslicer_status ts_mslicer_engine_diagnostics_json(
    const ts_mslicer_engine *engine,
    ts_mslicer_buffer *json);
ts_mslicer_status ts_mslicer_engine_encode(
    const ts_mslicer_engine *engine,
    uint32_t format,
    const uint8_t *preview_rgba,
    uint32_t preview_width,
    uint32_t preview_height,
    ts_mslicer_buffer *output);
/* Decode and verify an encoded result against the active job. On success,
 * details_json contains a stable compatibility/metadata report. */
ts_mslicer_status ts_mslicer_engine_inspect_output(
    const ts_mslicer_engine *engine,
    uint32_t format,
    const uint8_t *data,
    size_t size,
    ts_mslicer_buffer *details_json);
void ts_mslicer_buffer_release(ts_mslicer_buffer *buffer);
/* Explicit direct probe only. Public, broadcast, multicast, unspecified, and
 * loopback addresses are rejected. Result contains identity JSON on success or
 * a UTF-8 diagnostic on failure and must be released with buffer_release. */
ts_mslicer_status ts_mslicer_remote_probe(
    const ts_mslicer_remote_probe_request *request,
    ts_mslicer_buffer *result);
/* Upload only: this function never starts a print. The exact artifact bytes are
 * copied into immutable session ownership before networking begins. */
ts_mslicer_status ts_mslicer_remote_upload(
    const ts_mslicer_remote_upload_request *request,
    ts_mslicer_buffer *result);

#if defined(__cplusplus)
}
#endif

#endif
