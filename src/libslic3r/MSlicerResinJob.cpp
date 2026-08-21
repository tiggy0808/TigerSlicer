#include "MSlicerResinJob.hpp"
#include "MeshBoolean.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "SLA/Hollowing.hpp"

#include <boost/algorithm/string/case_conv.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>

namespace Slic3r {

namespace {

constexpr const char *PREPARED_SUPPORT_INPUT_FILE = "tigerslicer://prepared-msla-supports";

ts_mslicer_exposure exposure_to_abi(const MSlicerExposureSettings &source)
{
    ts_mslicer_exposure target {};
    target.exposure_seconds = source.exposure_seconds;
    target.delay_seconds = source.delay_seconds;
    target.pwm = source.pwm;
    target.lift_distance_mm = source.lift_distance_mm;
    target.lift_speed_mm_min = source.lift_speed_mm_min;
    target.retract_distance_mm = source.retract_distance_mm;
    target.retract_speed_mm_min = source.retract_speed_mm_min;
    return target;
}

} // namespace

bool MSlicerResinJob::is_prepared_support_object(const ModelObject &object)
{
    return object.input_file == PREPARED_SUPPORT_INPUT_FILE;
}

bool MSlicerResinJob::has_prepared_supports(const Model &model)
{
    return std::any_of(model.objects.begin(), model.objects.end(),
        [](const ModelObject *object) { return is_prepared_support_object(*object); });
}

const char *MSlicerResinJob::prepared_support_input_file() noexcept
{
    return PREPARED_SUPPORT_INPUT_FILE;
}

MSlicerResinSettings MSlicerResinJob::settings_from_config(const DynamicPrintConfig &config)
{
    MSlicerResinSettings settings;
    settings.resolution_x = uint32_t(std::max(0, config.opt_int("display_pixels_x")));
    settings.resolution_y = uint32_t(std::max(0, config.opt_int("display_pixels_y")));
    settings.platform_x_mm = float(config.opt_float("display_width"));
    settings.platform_y_mm = float(config.opt_float("display_height"));
    settings.platform_z_mm = float(config.opt_float("max_print_height"));
    settings.layer_height_mm = float(config.opt_float("layer_height"));
    settings.supersample = uint8_t(std::clamp(config.opt_int("mslicer_supersample"), 1, 4));
    settings.portrait = config.opt_enum<SLADisplayOrientation>("display_orientation") == sladoPortrait;
    settings.mirror_x = config.opt_bool("display_mirror_x");
    settings.mirror_y = config.opt_bool("display_mirror_y");
    settings.bottom_layers = uint32_t(std::max(0, config.opt_int("mslicer_bottom_layers")));
    settings.transition_layers = uint32_t(std::max(0, config.opt_int("mslicer_transition_layers")));
    settings.normal.exposure_seconds = float(config.opt_float("exposure_time"));
    settings.bottom.exposure_seconds = float(config.opt_float("initial_exposure_time"));
    settings.normal.delay_seconds = float(config.opt_float("mslicer_normal_wait_time"));
    settings.bottom.delay_seconds = float(config.opt_float("mslicer_bottom_wait_time"));
    settings.normal.pwm = uint8_t(std::clamp(config.opt_int("mslicer_normal_pwm"), 0, 255));
    settings.bottom.pwm = uint8_t(std::clamp(config.opt_int("mslicer_bottom_pwm"), 0, 255));
    settings.normal.lift_distance_mm = float(config.opt_float("mslicer_normal_lift_distance"));
    settings.normal.lift_speed_mm_min = float(config.opt_float("mslicer_normal_lift_speed"));
    settings.normal.retract_distance_mm = float(config.opt_float("mslicer_normal_retract_distance"));
    settings.normal.retract_speed_mm_min = float(config.opt_float("mslicer_normal_retract_speed"));
    settings.bottom.lift_distance_mm = float(config.opt_float("mslicer_bottom_lift_distance"));
    settings.bottom.lift_speed_mm_min = float(config.opt_float("mslicer_bottom_lift_speed"));
    settings.bottom.retract_distance_mm = float(config.opt_float("mslicer_bottom_retract_distance"));
    settings.bottom.retract_speed_mm_min = float(config.opt_float("mslicer_bottom_retract_speed"));
    settings.deterministic_seed = 0x5449474552ULL;
    settings.absolute_correction_mm = float(config.opt_float("absolute_correction"));
    settings.elefant_foot_compensation_mm = float(config.opt_float("elefant_foot_compensation"));
    return settings;
}

MSlicerSupportSettings MSlicerResinJob::support_settings_from_config(const DynamicPrintConfig &config)
{
    // The configuration declares the base kind independently of the engine
    // header; if the two ever drift, this stops compiling rather than
    // generating the wrong base.
    static_assert(int(msbNone) == int(TS_MSLICER_SUPPORT_BASE_NONE));
    static_assert(int(msbRaft) == int(TS_MSLICER_SUPPORT_BASE_RAFT));
    static_assert(int(msbPads) == int(TS_MSLICER_SUPPORT_BASE_PADS));

    MSlicerSupportSettings settings;
    settings.support_radius_mm = float(config.opt_float("support_pillar_diameter") * 0.5);
    settings.tip_radius_mm = float(config.opt_float("support_head_front_diameter") * 0.5);
    settings.tip_length_mm = float(config.opt_float("support_head_width"));
    settings.base_height_mm = float(config.opt_float("support_base_height"));
    settings.base_offset_mm = float(config.opt_float("support_base_safety_distance"));
    const double density = std::max(1., double(config.opt_int("support_points_density_relative")));
    const float spacing_scale = float(std::sqrt(100. / density));
    settings.face_support_spacing_mm = 50.f * spacing_scale;
    settings.edge_support_spacing_mm = 20.f * spacing_scale;

    settings.contact_penetration_mm = float(config.opt_float("msla_support_contact_penetration"));
    settings.min_support_height_mm = float(config.opt_float("msla_support_min_height"));
    settings.min_overhang_area_mm2 = float(config.opt_float("msla_support_min_overhang_area"));
    settings.base = static_cast<ts_mslicer_support_base>(
        config.opt_enum<MSlicerSupportBaseKind>("msla_support_base_kind"));
    settings.base_diameter_mm = float(config.opt_float("msla_support_base_diameter"));
    settings.max_overhang_angle_degrees = float(config.opt_float("msla_support_overhang_angle"));
    // The generator compares raw radians; the setting is in degrees because
    // that is the unit the rest of the angle controls use.
    settings.edge_angle_delta_radians =
        float(config.opt_float("msla_support_edge_angle_delta") * M_PI / 180.);
    settings.precision = uint32_t(config.opt_int("msla_support_precision"));

    settings.support_on_model = config.opt_bool("msla_support_on_model");
    settings.cross_bracing = config.opt_bool("msla_support_cross_bracing");
    settings.brace_spacing_mm = float(config.opt_float("msla_support_brace_spacing"));
    settings.adaptive_diameter = config.opt_bool("msla_support_adaptive_diameter");
    settings.max_pillar_diameter_mm =
        float(config.opt_float("msla_support_max_pillar_diameter"));
    settings.enforcers_only = config.opt_bool("support_enforcers_only");
    return settings;
}

const std::vector<std::string> &MSlicerResinJob::support_option_keys()
{
    // The first two gate generation (see validate_model_input); the rest are
    // read by support_settings_from_config. Keep both in step with this list.
    // "supports_enable" is deliberately absent: MSLA supports exist when they
    // have been generated, not when a switch says so.
    static const std::vector<std::string> keys {
        "support_tree_type",
        "support_enforcers_only",
        "support_pillar_diameter",
        "support_head_front_diameter",
        "support_head_width",
        "support_base_height",
        "support_base_safety_distance",
        "support_points_density_relative",
        "msla_support_contact_penetration",
        "msla_support_min_height",
        "msla_support_min_overhang_area",
        "msla_support_base_kind",
        "msla_support_base_diameter",
        "msla_support_overhang_angle",
        "msla_support_edge_angle_delta",
        "msla_support_precision",
        "msla_support_on_model",
        "msla_support_cross_bracing",
        "msla_support_brace_spacing",
        "msla_support_adaptive_diameter",
        "msla_support_max_pillar_diameter",
        "msla_support_manual_only",
    };
    return keys;
}

bool MSlicerResinJob::is_support_option(const std::string &opt_key)
{
    const std::vector<std::string> &keys = support_option_keys();
    return std::find(keys.begin(), keys.end(), opt_key) != keys.end();
}

ts_mslicer_format MSlicerResinJob::format_from_config(const DynamicPrintConfig &config)
{
    const std::string name = boost::algorithm::to_lower_copy(config.opt_string("sla_archive_format"));
    if (name == "ctb") return TS_MSLICER_FORMAT_CTB;
    if (name == "goo") return TS_MSLICER_FORMAT_GOO;
    if (name == "nanodlp") return TS_MSLICER_FORMAT_NANODLP;
    if (name == "svg") return TS_MSLICER_FORMAT_SVG;
    throw std::invalid_argument(
        "This Resin profile selects unsupported archive format '" + name +
        "'. Choose CTB, GOO, NanoDLP, or explicitly enabled diagnostic SVG.");
}

void MSlicerResinJob::validate_model_input(const Model &model, const DynamicPrintConfig &config)
{
    // Orca offers no SLA tree-type selection - the default tree is the only
    // one - so the guard PrusaSlicer needed here has nothing to refuse.
    if (config.opt_int("support_points_density_relative") <= 0)
        throw std::invalid_argument("Automatic MSLA support-point density must be greater than zero.");
    if (config.opt_bool("pad_enable"))
        throw std::invalid_argument("This print profile enables a pad under the model, which TigerSlicer does not generate - supports carry their own base pads. Switch to a TigerSlicer resin print profile before slicing.");
    // absolute_correction, elefant_foot_compensation and the printer/material
    // scaling corrections are applied now - as raster morphology and exact
    // geometry scaling respectively. Z correction remains unimplemented.
    if (config.opt_int("zcorrection_layers") != 0)
        throw std::invalid_argument("This Resin profile enables Z correction that MSlicer cannot apply. Set Z correction layers to zero.");
    if (config.opt_float("elefant_foot_compensation") < 0.)
        throw std::invalid_argument("Elephant-foot compensation must not be negative for Resin printing.");

    for (const ModelObject *object : model.objects) {
        if (!object->printable)
            continue;
        // hollowing_enable overrides and sla_drain_holes are honoured by
        // prepared_object_mesh, not refused.
        for (const char *key : { "supports_enable", "pad_enable" }) {
            if (object->config.has(key) && object->config.get().opt_bool(key))
                throw std::invalid_argument("A Resin object carries a per-object support or pad override that MSlicer does not read. Remove the object override before slicing.");
        }
        // sla_support_points are no longer refused: they are the manual
        // support point store, honoured by generation as manual additions.
        for (const ModelVolume *volume : object->volumes) {
            if (volume->type() == ModelVolumeType::NEGATIVE_VOLUME)
                throw std::invalid_argument("This Resin object contains an unbaked negative volume. Apply the boolean operation to the mesh before slicing with MSlicer.");
            // Support blockers and enforcers are consumed by generation now.
        }
    }
}

indexed_triangle_set MSlicerResinJob::generate_supports(const MSlicerSupportSettings &settings)
{
    ts_mslicer_support_config config {};
    config.struct_size = sizeof(config);
    config.support_radius_mm = settings.support_radius_mm;
    config.tip_radius_mm = settings.tip_radius_mm;
    config.tip_length_mm = settings.tip_length_mm;
    config.precision = settings.precision;
    config.base_height_mm = settings.base_height_mm;
    config.base_offset_mm = settings.base_offset_mm;
    config.max_overhang_angle_degrees = settings.max_overhang_angle_degrees;
    config.face_support_spacing_mm = settings.face_support_spacing_mm;
    config.edge_support_spacing_mm = settings.edge_support_spacing_mm;
    config.edge_angle_delta_radians = settings.edge_angle_delta_radians;
    config.contact_penetration_mm = settings.contact_penetration_mm;
    config.min_support_height_mm = settings.min_support_height_mm;
    config.min_overhang_area_mm2 = settings.min_overhang_area_mm2;
    config.base_kind = static_cast<uint32_t>(settings.base);
    config.base_diameter_mm = settings.base_diameter_mm;
    config.support_on_model = settings.support_on_model ? 1u : 0u;
    config.cross_bracing = settings.cross_bracing ? 1u : 0u;
    config.brace_spacing_mm = settings.brace_spacing_mm;
    config.adaptive_diameter = settings.adaptive_diameter ? 1u : 0u;
    config.max_pillar_diameter_mm = settings.max_pillar_diameter_mm;
    config.enforcers_only = settings.enforcers_only ? 1 : 0;
    m_engine.generate_supports(config);
    const MSlicer::PreparedMesh prepared = m_engine.generated_support_mesh();
    indexed_triangle_set output;
    output.vertices.reserve(prepared.vertices.size());
    output.indices.reserve(prepared.triangles.size());
    for (const std::array<float, 3> &vertex : prepared.vertices)
        output.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const std::array<uint32_t, 3> &triangle : prepared.triangles)
        output.indices.emplace_back(triangle[0], triangle[1], triangle[2]);
    return output;
}

std::vector<Vec3f> MSlicerResinJob::gather_manual_support_points(const Model &model)
{
    std::vector<Vec3f> points;
    for (const ModelObject *object : model.objects) {
        if (!object->printable || is_prepared_support_object(*object))
            continue;
        if (object->sla_support_points.empty())
            continue;
        for (const ModelInstance *instance : object->instances) {
            if (!instance->is_printable())
                continue;
            const Transform3d trafo = instance->get_matrix();
            for (const sla::SupportPoint &point : object->sla_support_points)
                points.emplace_back((trafo * point.pos.cast<double>()).cast<float>());
        }
    }
    return points;
}

void MSlicerResinJob::set_manual_support_points(const std::vector<Vec3f> &points, bool manual_only)
{
    static_assert(sizeof(Vec3f) == 3 * sizeof(float));
    m_engine.set_manual_support_points(
        points.empty() ? nullptr : points.front().data(), points.size(), manual_only);
}

Vec3d MSlicerResinJob::suggest_orientation(double max_overhang_angle_degrees)
{
    const std::array<float, 3> rotation =
        m_engine.suggest_orientation(float(max_overhang_angle_degrees));
    return Vec3d(rotation[0], rotation[1], rotation[2]);
}

namespace {

// Subtract the drain-hole cylinders from the mesh. Deterministic: the jitter
// that keeps the boolean away from degenerate coincidences uses a fixed seed,
// so the same project always produces the same layers.
void drill_drain_holes(indexed_triangle_set &mesh, const sla::DrainHoles &holes)
{
    if (holes.empty())
        return;

    std::mt19937 rng{0x54494745u};
    std::uniform_real_distribution<float> dist(0.f, float(EPSILON));

    auto holes_cgal = MeshBoolean::cgal::triangle_mesh_to_cgal(indexed_triangle_set{});
    for (sla::DrainHole hole : holes) {
        hole.normal += Vec3f{dist(rng), dist(rng), dist(rng)};
        hole.normal.normalize();
        hole.pos += Vec3f{dist(rng), dist(rng), dist(rng)};
        auto hole_cgal = MeshBoolean::cgal::triangle_mesh_to_cgal(hole.to_mesh());
        MeshBoolean::cgal::plus(*holes_cgal, *hole_cgal);
    }

    if (MeshBoolean::cgal::does_self_intersect(*holes_cgal))
        throw std::invalid_argument(
            "Two drain holes intersect in a way that cannot be drilled. Move the holes apart.");

    auto mesh_cgal = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    if (!MeshBoolean::cgal::does_bound_a_volume(*mesh_cgal))
        throw std::invalid_argument(
            "The model mesh does not enclose a volume, so its drain holes cannot be drilled. Repair the mesh first.");

    try {
        MeshBoolean::cgal::minus(*mesh_cgal, *holes_cgal);
        mesh = MeshBoolean::cgal::cgal_to_indexed_triangle_set(*mesh_cgal);
    } catch (const std::exception &) {
        throw std::invalid_argument(
            "Drilling the drain holes failed. Reposition the holes or repair the mesh.");
    }

    if (mesh.empty())
        throw std::invalid_argument(
            "Drilling the drain holes removed the whole model. Reposition the holes.");
}

} // namespace

const indexed_triangle_set *MSlicerResinJob::prepared_mesh_for(const ObjectID &object_id) const
{
    for (const auto &[id, mesh] : m_prepared_meshes)
        if (id == object_id)
            return &mesh;
    return nullptr;
}

indexed_triangle_set MSlicerResinJob::prepared_object_mesh(const ModelObject &object,
                                                           const DynamicPrintConfig &config)
{
    indexed_triangle_set mesh = object.raw_indexed_triangle_set();
    if (mesh.empty())
        return mesh;

    DynamicPrintConfig merged = config;
    merged.apply(object.config.get(), true);

    const bool hollow = merged.has("hollowing_enable") && merged.opt_bool("hollowing_enable");
    if (hollow) {
        sla::HollowingConfig hollow_cfg;
        hollow_cfg.min_thickness    = merged.opt_float("hollowing_min_thickness");
        hollow_cfg.quality          = merged.opt_float("hollowing_quality");
        hollow_cfg.closing_distance = merged.opt_float("hollowing_closing_distance");

        // Orca's hollowing speaks TriangleMesh rather than the raw triangle
        // set; wrap, hollow, and unwrap.
        TriangleMesh hollow_mesh_wrap(mesh);
        sla::InteriorPtr interior = sla::generate_interior(hollow_mesh_wrap, hollow_cfg);
        if (!interior || get_mesh(*interior).empty())
            throw std::invalid_argument(
                "Hollowing produced no interior for \"" + object.name +
                "\". The walls may be thinner than the configured shell; reduce the wall thickness or disable hollowing.");
        sla::hollow_mesh(hollow_mesh_wrap, *interior, sla::hfRemoveInsideTriangles);
        mesh = std::move(hollow_mesh_wrap.its);
    }

    drill_drain_holes(mesh, object.sla_drain_holes);
    return mesh;
}

size_t MSlicerResinJob::add_model(const Model &model, bool include_prepared_supports,
                                  const DynamicPrintConfig *prep_config)
{
    // Printer and material shrinkage corrections are exact geometry scaling,
    // applied per instance about its own bed footprint so the part keeps its
    // plate position and stays on the plate.
    Vec3d scale_correction = Vec3d::Ones();
    if (prep_config != nullptr) {
        auto correction = [prep_config](const char *relative_key, const char *material_key) {
            return prep_config->opt_float(relative_key) * prep_config->opt_float(material_key);
        };
        scale_correction = Vec3d(correction("relative_correction_x", "material_correction_x"),
                                 correction("relative_correction_y", "material_correction_y"),
                                 correction("relative_correction_z", "material_correction_z"));
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(scale_correction[axis]) || scale_correction[axis] < 0.1 ||
                scale_correction[axis] > 10.)
                throw std::invalid_argument(
                    "The printer or material scaling correction is outside the sane 0.1 to 10 range.");
    }
    const bool scaled = !scale_correction.isApprox(Vec3d::Ones());

    size_t added = 0;
    for (const ModelObject *object : model.objects) {
        const bool prepared_support = is_prepared_support_object(*object);
        if (!include_prepared_supports && prepared_support)
            continue;
        if (!object->printable)
            continue;
        const bool prepare = prep_config != nullptr && !prepared_support;
        const indexed_triangle_set mesh = prepare ?
            prepared_object_mesh(*object, *prep_config) :
            object->raw_indexed_triangle_set();
        if (mesh.empty())
            continue;
        if (prepare)
            // Retained for the preview's cut faces: this is the geometry that
            // is actually sliced, cavities and drain holes included.
            m_prepared_meshes.emplace_back(object->id(), mesh);
        for (const ModelInstance *instance : object->instances) {
            if (!instance->is_printable())
                continue;
            Transform3d trafo = instance->get_matrix();
            if (scaled) {
                const BoundingBoxf3 world_box = bounding_box(mesh).transformed(trafo);
                const Vec3d anchor(world_box.center().x(), world_box.center().y(), 0.);
                Transform3d correction = Transform3d::Identity();
                correction.translate(anchor);
                correction.scale(scale_correction);
                correction.translate(-anchor);
                trafo = correction * trafo;
            }
            add_mesh(mesh, trafo);
            // Painted support-steering volumes ride along with each instance.
            for (const ModelVolume *volume : object->volumes)
                if (volume->is_support_blocker() || volume->is_support_enforcer())
                    add_support_region(volume->mesh().its, trafo * volume->get_matrix(),
                                       volume->is_support_blocker());
            ++added;
        }
    }
    if (added == 0)
        throw std::invalid_argument("The Resin project has no printable model instance fully inside the build volume.");
    return added;
}

ts_mslicer_config MSlicerResinJob::to_abi(const MSlicerResinSettings &settings)
{
    ts_mslicer_config config {};
    config.struct_size = sizeof(config);
    config.resolution_x = settings.resolution_x;
    config.resolution_y = settings.resolution_y;
    config.platform_x_mm = settings.platform_x_mm;
    config.platform_y_mm = settings.platform_y_mm;
    config.platform_z_mm = settings.platform_z_mm;
    config.layer_height_mm = settings.layer_height_mm;
    config.supersample = settings.supersample;
    config.orientation = settings.portrait ? 1 : 0;
    config.mirror_x = settings.mirror_x ? 1 : 0;
    config.mirror_y = settings.mirror_y ? 1 : 0;
    config.first_layers = settings.bottom_layers;
    config.transition_layers = settings.transition_layers;
    config.normal = exposure_to_abi(settings.normal);
    config.first = exposure_to_abi(settings.bottom);
    config.deterministic_seed = settings.deterministic_seed;
    config.absolute_correction_mm = settings.absolute_correction_mm;
    config.elefant_foot_compensation_mm = settings.elefant_foot_compensation_mm;
    return config;
}

MSlicerResinJob::MSlicerResinJob(const MSlicerResinSettings &settings)
    : m_settings(settings), m_engine(to_abi(settings))
{
}

void MSlicerResinJob::validate(const MSlicerResinSettings &settings)
{
    MSlicer::Engine::validate(to_abi(settings));
}

const char *MSlicerResinJob::format_extension(ts_mslicer_format format) noexcept
{
    switch (format) {
    case TS_MSLICER_FORMAT_CTB: return ".ctb";
    case TS_MSLICER_FORMAT_GOO: return ".goo";
    case TS_MSLICER_FORMAT_NANODLP: return ".nanodlp";
    case TS_MSLICER_FORMAT_SVG: return ".svg";
    default: return nullptr;
    }
}

std::string MSlicerResinJob::inspect_output(ts_mslicer_format format, const std::vector<uint8_t> &bytes) const
{
    const MSlicer::Buffer report = m_engine.inspect_output(format, bytes.data(), bytes.size());
    return buffer_string(report);
}

void MSlicerResinJob::add_mesh(const indexed_triangle_set &mesh,
                               const Transform3d &transform,
                               uint8_t relative_exposure)
{
    if (mesh.vertices.empty() || mesh.indices.empty())
        throw std::invalid_argument("MSlicer mesh is empty");
    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("MSlicer mesh has too many vertices");

    static_assert(sizeof(stl_vertex) == sizeof(float) * 3,
                  "MSlicer bridge requires tightly packed XYZ vertices");
    static_assert(sizeof(stl_triangle_vertex_indices) == sizeof(uint32_t) * 3,
                  "MSlicer bridge requires tightly packed triangle indices");

    const Matrix4f matrix = transform.matrix().cast<float>();
    std::vector<stl_vertex> transformed_vertices;
    transformed_vertices.reserve(mesh.vertices.size());
    double signed_volume = 0.0;
    for (const stl_vertex &vertex : mesh.vertices) {
        const Vec3f transformed = (matrix * vertex.homogeneous()).hnormalized();
        transformed_vertices.emplace_back(transformed);
        m_input_min = m_input_min.cwiseMin(transformed);
        m_input_max = m_input_max.cwiseMax(transformed);
    }
    for (const stl_triangle_vertex_indices &triangle : mesh.indices) {
        const Vec3d a = transformed_vertices[triangle[0]].cast<double>();
        const Vec3d b = transformed_vertices[triangle[1]].cast<double>();
        const Vec3d c = transformed_vertices[triangle[2]].cast<double>();
        signed_volume += a.dot(b.cross(c)) / 6.0;
    }
    m_input_mesh_volume_mm3 += std::abs(signed_volume);
    ts_mslicer_mesh input {};
    input.struct_size = sizeof(input);
    input.vertices_xyz = reinterpret_cast<const float *>(transformed_vertices.data());
    input.vertex_count = transformed_vertices.size();
    input.triangles = reinterpret_cast<const uint32_t *>(mesh.indices.data());
    input.triangle_count = mesh.indices.size();
    // The Rust boundary copies the mesh synchronously. Flatten the affine
    // transform into this temporary vertex buffer so every downstream path
    // (validation, rasterization, preview, and export) consumes identical
    // world-space geometry without relying on cross-language matrix layout.
    const Matrix4f identity = Matrix4f::Identity();
    std::memcpy(input.transform, identity.data(), sizeof(input.transform));
    input.relative_exposure = relative_exposure;
    m_engine.add_mesh(input);
}

void MSlicerResinJob::add_support_region(const indexed_triangle_set &mesh,
                                         const Transform3d &transform,
                                         bool blocker)
{
    // An empty painted volume steers nothing.
    if (mesh.vertices.empty() || mesh.indices.empty())
        return;
    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("MSlicer support region has too many vertices");

    const Matrix4f matrix = transform.matrix().cast<float>();
    std::vector<stl_vertex> transformed_vertices;
    transformed_vertices.reserve(mesh.vertices.size());
    // Deliberately not folded into the input bounds: a region is not printed
    // geometry and may legitimately poke outside the build volume.
    for (const stl_vertex &vertex : mesh.vertices)
        transformed_vertices.emplace_back((matrix * vertex.homogeneous()).hnormalized());

    ts_mslicer_mesh input {};
    input.struct_size = sizeof(input);
    input.vertices_xyz = reinterpret_cast<const float *>(transformed_vertices.data());
    input.vertex_count = transformed_vertices.size();
    input.triangles = reinterpret_cast<const uint32_t *>(mesh.indices.data());
    input.triangle_count = mesh.indices.size();
    const Matrix4f identity = Matrix4f::Identity();
    std::memcpy(input.transform, identity.data(), sizeof(input.transform));
    input.relative_exposure = 255;
    input.role = blocker ? 1 : 2;
    m_engine.add_mesh(input);
}

Vec3f MSlicerResinJob::input_size_mm() const noexcept
{
    if (!m_input_min.allFinite() || !m_input_max.allFinite())
        return Vec3f::Zero();
    return (m_input_max - m_input_min).eval();
}

Vec3f MSlicerResinJob::input_min_mm() const noexcept
{
    if (!m_input_min.allFinite())
        return Vec3f::Zero();
    return m_input_min;
}

Vec3f MSlicerResinJob::input_max_mm() const noexcept
{
    if (!m_input_max.allFinite())
        return Vec3f::Zero();
    return m_input_max;
}

void MSlicerResinJob::slice()
{
    m_engine.slice();
}

void MSlicerResinJob::request_cancel() noexcept
{
    m_engine.request_cancel();
}

float MSlicerResinJob::progress() const noexcept
{
    return m_engine.progress();
}

uint32_t MSlicerResinJob::layer_count() const noexcept
{
    return m_engine.layer_count();
}

std::vector<uint8_t> MSlicerResinJob::layer(uint32_t index) const
{
    return m_engine.layer(index);
}

MSlicerResinMetrics MSlicerResinJob::metrics() const
{
    const ts_mslicer_metrics source = m_engine.metrics();
    return { source.layer_count, source.lit_voxels, source.resin_volume_mm3,
             source.estimated_seconds };
}

std::string MSlicerResinJob::buffer_string(const MSlicer::Buffer &buffer)
{
    if (buffer.empty())
        return {};
    return std::string(reinterpret_cast<const char *>(buffer.data()), buffer.size());
}

std::string MSlicerResinJob::manifest_json() const
{
    return buffer_string(m_engine.manifest());
}

std::string MSlicerResinJob::diagnostics_json() const
{
    return buffer_string(m_engine.diagnostics());
}

std::vector<uint8_t> MSlicerResinJob::encode(ts_mslicer_format format) const
{
    MSlicer::Buffer buffer;
    if (format == TS_MSLICER_FORMAT_SVG || layer_count() == 0) {
        buffer = m_engine.encode(format);
    } else {
        const uint32_t source_width = m_settings.portrait ? m_settings.resolution_y : m_settings.resolution_x;
        const uint32_t source_height = m_settings.portrait ? m_settings.resolution_x : m_settings.resolution_y;
        const uint32_t preview_width = std::min<uint32_t>(290, source_width);
        const uint32_t preview_height = std::max<uint32_t>(1,
            uint32_t(std::llround(double(source_height) * preview_width / source_width)));
        const std::vector<uint8_t> mask = layer((layer_count() - 1) / 2);
        std::vector<uint8_t> rgba(size_t(preview_width) * preview_height * 4, 255);
        for (uint32_t y = 0; y < preview_height; ++y) {
            const uint32_t source_y = std::min(source_height - 1,
                uint32_t(uint64_t(y) * source_height / preview_height));
            for (uint32_t x = 0; x < preview_width; ++x) {
                const uint32_t source_x = std::min(source_width - 1,
                    uint32_t(uint64_t(x) * source_width / preview_width));
                const uint8_t value = mask[size_t(source_y) * source_width + source_x];
                const size_t target = (size_t(y) * preview_width + x) * 4;
                rgba[target] = rgba[target + 1] = rgba[target + 2] = value;
            }
        }
        buffer = m_engine.encode(format, rgba.data(), preview_width, preview_height);
    }
    if (buffer.empty())
        return {};
    return { buffer.data(), buffer.data() + buffer.size() };
}

} // namespace Slic3r
