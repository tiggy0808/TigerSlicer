#include "GLGizmoModelTools.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace Slic3r::GUI {

void SurfacePicker::clear()
{
    m_object   = nullptr;
    m_instance = -1;
    m_volume_matrices.clear();
    m_meshes.clear();
    m_parts.clear();
}

bool SurfacePicker::update(const ModelObject *object, int instance)
{
    if (object == nullptr || instance < 0 || size_t(instance) >= object->instances.size()) {
        const bool had_parts = !m_parts.empty();
        clear();
        return had_parts;
    }
    const Transform3d                 instance_matrix = object->instances[instance]->get_matrix();
    std::vector<Transform3d>          matrices;
    std::vector<const TriangleMesh *> meshes;
    for (const ModelVolume *volume : object->volumes)
        if (volume->is_model_part()) {
            matrices.push_back(volume->get_matrix());
            meshes.push_back(&volume->mesh());
        }
    bool same = object == m_object && instance == m_instance && instance_matrix.isApprox(m_instance_matrix) && meshes == m_meshes;
    for (size_t i = 0; same && i < matrices.size(); ++i)
        same = matrices[i].isApprox(m_volume_matrices[i]);
    if (same)
        return false;

    m_object          = object;
    m_instance        = instance;
    m_instance_matrix = instance_matrix;
    m_volume_matrices = std::move(matrices);
    m_meshes          = std::move(meshes);
    m_parts.clear();
    for (const ModelVolume *volume : object->volumes)
        if (volume->is_model_part()) {
            Part part;
            part.mesh      = volume->mesh_ptr();
            part.world     = instance_matrix * volume->get_matrix();
            part.raycaster = std::make_unique<MeshRaycaster>(part.mesh);
            m_parts.push_back(std::move(part));
        }
    return true;
}

std::optional<SurfaceHit> SurfacePicker::pick(const Vec2d &mouse, const Camera &camera) const
{
    std::optional<SurfaceHit> best;
    double                    best_distance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < m_parts.size(); ++i) {
        const Part &part = m_parts[i];
        Vec3f       hit, normal;
        size_t      facet = 0;
        if (!part.raycaster->unproject_on_mesh(mouse, part.world, camera, hit, normal, nullptr, &facet))
            continue;
        const Vec3d  world_hit = part.world * hit.cast<double>();
        const double distance  = (camera.get_position() - world_hit).squaredNorm();
        if (distance < best_distance) {
            best_distance = distance;
            const Vec3d world_normal = (part.world.linear().inverse().transpose() * normal.cast<double>()).normalized();
            best                     = SurfaceHit{i, int(facet), world_hit, world_normal};
        }
    }
    return best;
}

bool SurfacePicker::is_visible(const Vec3d &point, const Camera &camera) const
{
    Vec3d origin, dir;
    if (camera.get_type() == Camera::EType::Perspective) {
        origin = camera.get_position();
        dir    = point - origin;
    } else {
        dir    = camera.get_dir_forward();
        origin = point - 1e4 * dir;
        dir *= 1e4;
    }
    const double length = dir.norm();
    if (!(length > 0.))
        return true;
    dir /= length;
    // The point is on an edge of the surface: hits that close to it are the point itself.
    const double tolerance = 0.05 + 1e-3 * length;
    for (const Part &part : m_parts) {
        const Transform3d             to_mesh = part.world.inverse();
        const AABBMesh::hit_result hit     = part.raycaster->get_aabb_mesh().query_ray_hit(to_mesh * origin, to_mesh.linear() * dir);
        if (hit.is_hit() && (part.world * hit.position() - origin).dot(dir) < length - tolerance)
            return false;
    }
    return true;
}

double SurfacePicker::depth_through(const Vec3d &point, const Vec3d &normal) const
{
    double deepest = 0.;
    for (const Part &part : m_parts) {
        const Transform3d to_mesh = part.world.inverse();
        const Vec3d       source  = to_mesh * point;
        const Vec3d       dir     = to_mesh.linear() * (-normal);
        for (const AABBMesh::hit_result &hit : part.raycaster->get_aabb_mesh().query_ray_hits(source, dir))
            if (hit.is_hit())
                deepest = std::max(deepest, (part.world * hit.position() - point).dot(-normal));
    }
    return deepest;
}

size_t add_tool_volumes(int object_idx, const Transform3d &instance_matrix, std::vector<ToolVolume> volumes,
                        const std::string &snapshot_name)
{
    Plater *plater = wxGetApp().plater();
    Model  &model  = plater->model();
    if (object_idx < 0 || size_t(object_idx) >= model.objects.size())
        return 0;
    volumes.erase(std::remove_if(volumes.begin(), volumes.end(), [](const ToolVolume &v) { return v.world_mesh.indices.empty(); }),
                  volumes.end());
    if (volumes.empty())
        return 0;

    ModelObject &object = *model.objects[object_idx];
    // The volumes go in the object's coordinates, which the instance transformation maps to the world.
    const Transform3d to_object = instance_matrix.inverse();
    bool              any_part  = false;
    {
        Plater::TakeSnapshot snapshot(plater, snapshot_name);
        for (ToolVolume &tool : volumes) {
            its_transform(tool.world_mesh, to_object, true);
            ModelVolume *volume = object.add_volume(TriangleMesh(std::move(tool.world_mesh)), tool.type);
            volume->name        = tool.name;
            int extruder        = 0;
            if (tool.type == ModelVolumeType::MODEL_PART) {
                any_part = true;
                if (object.config.has("extruder"))
                    extruder = object.config.opt_int("extruder");
            }
            volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        }
        Slic3r::save_object_mesh(object);
    }
    // Lists the new volumes and reloads the scene; an instance selection takes the new volumes in.
    ObjectList *list = wxGetApp().obj_list();
    list->reorder_volumes_and_get_selection(object_idx);
    if (any_part)
        plater->get_view3D_canvas3D()->update_instance_printable_state_for_object(size_t(object_idx));
    // The plates keep their own view of each instance (as when a part is added from the menu).
    list->notify_instance_updated(object_idx);
    return volumes.size();
}

std::string format_mm(double value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    std::string s(buf);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

bool begin_tool_render()
{
    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return false;
    shader->start_using();
    shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());
    shader->set_uniform("emission_factor", 0.25f);
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    return true;
}

void render_tool_model(GLModel &model, const ColorRGBA &color, const Transform3d &matrix)
{
    GLShaderProgram *shader = wxGetApp().get_current_shader();
    if (shader == nullptr || !model.is_initialized())
        return;
    const Transform3d &view = wxGetApp().plater()->get_camera().get_view_matrix();
    shader->set_uniform("view_model_matrix", view * matrix);
    const Matrix3d view_normal = view.matrix().block(0, 0, 3, 3) * matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal);
    model.set_color(color);
    model.render();
}

void end_tool_render()
{
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    if (GLShaderProgram *shader = wxGetApp().get_current_shader())
        shader->stop_using();
}

} // namespace Slic3r::GUI
