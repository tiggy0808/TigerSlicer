#ifndef slic3r_MSlicerResinJob_hpp_
#define slic3r_MSlicerResinJob_hpp_

#include "ObjectID.hpp"
#include "TriangleMesh.hpp"
#include "mslicer/cpp/MSlicerBridge.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <limits>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Model;
class ModelObject;

struct MSlicerExposureSettings {
    float exposure_seconds { 2.5f };
    float delay_seconds { 0.f };
    uint8_t pwm { 255 };
    float lift_distance_mm { 5.f };
    float lift_speed_mm_min { 120.f };
    float retract_distance_mm { 5.f };
    float retract_speed_mm_min { 180.f };
};

struct MSlicerResinSettings {
    uint32_t resolution_x { 0 };
    uint32_t resolution_y { 0 };
    float platform_x_mm { 0.f };
    float platform_y_mm { 0.f };
    float platform_z_mm { 0.f };
    float layer_height_mm { 0.f };
    uint8_t supersample { 1 };
    bool portrait { false };
    bool mirror_x { false };
    bool mirror_y { false };
    uint32_t bottom_layers { 0 };
    uint32_t transition_layers { 0 };
    MSlicerExposureSettings normal;
    MSlicerExposureSettings bottom;
    uint64_t deterministic_seed { 0 };
    // ABI 11: raster compensation in whole pixels per axis. absolute grows
    // (positive) or shrinks (negative) every layer, elephant foot shrinks the
    // first layer only.
    float absolute_correction_mm { 0.f };
    float elefant_foot_compensation_mm { 0.f };
};

struct MSlicerResinMetrics {
    uint32_t layer_count { 0 };
    uint64_t lit_voxels { 0 };
    double resin_volume_mm3 { 0. };
    double estimated_seconds { 0. };
};

struct MSlicerSupportSettings {
    float support_radius_mm { 0.5f };
    float tip_radius_mm { 0.2f };
    float tip_length_mm { 1.f };
    uint32_t precision { 10 };
    float base_height_mm { 1.f };
    float base_offset_mm { 1.f };
    float max_overhang_angle_degrees { 30.f };
    float face_support_spacing_mm { 50.f };
    float edge_support_spacing_mm { 20.f };
    float edge_angle_delta_radians { 0.1f };
    // How far the tip apex is pushed into the model past the contact point.
    float contact_penetration_mm { 0.f };
    // A contact point closer to the plate than this gets no support.
    float min_support_height_mm { 1.2f };
    // Connected overhanging regions smaller than this are left unsupported.
    // Isolated local minima are never dropped by this rule.
    float min_overhang_area_mm2 { 0.f };
    // What each column stands on. The ABI enum is the single definition; the
    // configuration's own MSlicerSupportBaseKind is asserted equal to it.
    ts_mslicer_support_base base { TS_MSLICER_SUPPORT_BASE_RAFT };
    float base_diameter_mm { 3.f };
    // ABI 8. A column may end on the model below its contact, with a second
    // tip there, instead of routing to the plate.
    bool support_on_model { false };
    // Diagonal X braces between neighbouring plate columns.
    bool cross_bracing { false };
    // Maximum distance between braced feet; also one brace band's height.
    float brace_spacing_mm { 10.f };
    // The pillar diameter grows with column height up to the maximum.
    bool adaptive_diameter { false };
    float max_pillar_diameter_mm { 6.f };
    // ABI 11. Automatic contacts are restricted to enforcer volumes; manual
    // points are exempt.
    bool enforcers_only { false };
};

// Native TigerSlicer owner for one in-process MSlicer Resin job. This class
// deliberately has no dependency on the legacy SLA archive/raster pipeline.
class MSlicerResinJob
{
public:
    explicit MSlicerResinJob(const MSlicerResinSettings &settings);

    static MSlicerResinSettings settings_from_config(const DynamicPrintConfig &config);
    static MSlicerSupportSettings support_settings_from_config(const DynamicPrintConfig &config);
    // Every configuration key the prepared support mesh is generated from,
    // including the keys that gate generation at all. A prepared mesh whose
    // configuration has since changed no longer describes the current settings.
    static const std::vector<std::string> &support_option_keys();
    static bool is_support_option(const std::string &opt_key);
    static ts_mslicer_format format_from_config(const DynamicPrintConfig &config);
    static void validate_model_input(const Model &model, const DynamicPrintConfig &config);
    static bool is_prepared_support_object(const ModelObject &object);
    static bool has_prepared_supports(const Model &model);
    static const char *prepared_support_input_file() noexcept;
    static void validate(const MSlicerResinSettings &settings);
    static const char *format_extension(ts_mslicer_format format) noexcept;

    void add_mesh(const indexed_triangle_set &mesh,
                  const Transform3d &transform = Transform3d::Identity(),
                  uint8_t relative_exposure = 255);
    // A painted support-steering volume: a blocker keeps automatic contacts
    // out, an enforcer (with 'Support only in enforced regions') is the only
    // place they may land. Never sliced, never part of the input bounds.
    void add_support_region(const indexed_triangle_set &mesh,
                            const Transform3d &transform,
                            bool blocker);
    // When prep_config is given, each object's mesh is prepared for printing
    // before it is handed to the engine: hollowed to the configured wall
    // thickness and drilled with its drain holes, honouring per-object
    // overrides. Support generation and orientation must pass nullptr - they
    // work on the solid exterior, which hollowing does not change.
    size_t add_model(const Model &model, bool include_prepared_supports = true,
                     const DynamicPrintConfig *prep_config = nullptr);
    // The printable mesh of one object in object space: hollowed and drilled
    // according to config plus the object's own overrides. Throws when a
    // requested preparation cannot be produced.
    static indexed_triangle_set prepared_object_mesh(const ModelObject &object,
                                                     const DynamicPrintConfig &config);
    // The prepared mesh retained from add_model for the given source object,
    // or nullptr when that object was added unprepared. The preview uses this
    // so its cut faces show the geometry that was actually sliced - cavities,
    // drain holes and all. Valid as long as this job lives.
    const indexed_triangle_set *prepared_mesh_for(const ObjectID &object_id) const;
    // World-space positions of every model object's manually placed support
    // points (each object's sla_support_points through its instance
    // transforms), the store the painting UI and 3MF projects share.
    static std::vector<Vec3f> gather_manual_support_points(const Model &model);
    // Replaces the manual points honoured by the next generate_supports call.
    void set_manual_support_points(const std::vector<Vec3f> &points, bool manual_only);
    indexed_triangle_set generate_supports(const MSlicerSupportSettings &settings);
    // Euler XYZ radians that minimise the loaded scene's supported overhang
    // area at the given detection angle; zero when it is already best.
    Vec3d suggest_orientation(double max_overhang_angle_degrees);
    void slice();
    void request_cancel() noexcept;
    float progress() const noexcept;

    const MSlicerResinSettings &settings() const noexcept { return m_settings; }
    Vec3f input_min_mm() const noexcept;
    Vec3f input_max_mm() const noexcept;
    Vec3f input_size_mm() const noexcept;
    double input_mesh_volume_mm3() const noexcept { return m_input_mesh_volume_mm3; }
    uint32_t layer_count() const noexcept;
    std::vector<uint8_t> layer(uint32_t index) const;
    MSlicerResinMetrics metrics() const;
    std::string manifest_json() const;
    std::string diagnostics_json() const;
    std::vector<uint8_t> encode(ts_mslicer_format format) const;
    std::string inspect_output(ts_mslicer_format format, const std::vector<uint8_t> &bytes) const;

private:
    static ts_mslicer_config to_abi(const MSlicerResinSettings &settings);
    static std::string buffer_string(const MSlicer::Buffer &buffer);

    MSlicerResinSettings m_settings;
    MSlicer::Engine m_engine;
    Vec3f m_input_min { Vec3f::Constant(std::numeric_limits<float>::infinity()) };
    Vec3f m_input_max { Vec3f::Constant(-std::numeric_limits<float>::infinity()) };
    double m_input_mesh_volume_mm3 { 0.0 };
    // Object-space prepared meshes retained by add_model, see prepared_mesh_for.
    std::vector<std::pair<ObjectID, indexed_triangle_set>> m_prepared_meshes;
};

} // namespace Slic3r

#endif
