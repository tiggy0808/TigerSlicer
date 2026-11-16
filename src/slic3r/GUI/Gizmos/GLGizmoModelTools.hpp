#ifndef slic3r_GLGizmoModelTools_hpp_
#define slic3r_GLGizmoModelTools_hpp_

// TigerSlicer: what the hole tool and the fillet/chamfer tool share. Both find where the mouse is on the
// selected object's parts, and both leave the model's meshes alone: they add a negative volume or a part.

#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

#include <admesh/stl.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;
class TriangleMesh;
enum class ModelVolumeType : int;

namespace GUI {

class Camera;

// Where the mouse ray meets the selected instance's parts, in world coordinates.
struct SurfaceHit
{
    size_t part;   // index into SurfacePicker::parts()
    int    facet;
    Vec3d  point;
    Vec3d  normal; // outward
};

// Ray casting against the model parts of one instance of an object. update() rebuilds only when the
// object, its parts or their transformations changed.
class SurfacePicker
{
public:
    struct Part
    {
        std::shared_ptr<const TriangleMesh> mesh;
        Transform3d                         world = Transform3d::Identity(); // mesh to world
        std::unique_ptr<MeshRaycaster>      raycaster;
    };

    // Returns true if it rebuilt (the parts changed since the last call).
    bool update(const ModelObject *object, int instance);
    void clear();

    const std::vector<Part> &parts() const { return m_parts; }
    const ModelObject       *object() const { return m_object; }
    int                      instance() const { return m_instance; }
    const Transform3d       &instance_matrix() const { return m_instance_matrix; }

    std::optional<SurfaceHit> pick(const Vec2d &mouse, const Camera &camera) const;

    // Whether `point` (world, on or near the parts' surface) can be seen from the camera past the parts.
    bool is_visible(const Vec3d &point, const Camera &camera) const;

    // How far a line into the object from `point` along `-normal` runs before it leaves the object for
    // the last time, over all parts. 0 if it never enters.
    double depth_through(const Vec3d &point, const Vec3d &normal) const;

private:
    const ModelObject       *m_object   = nullptr;
    int                      m_instance = -1;
    Transform3d              m_instance_matrix = Transform3d::Identity();
    std::vector<Transform3d> m_volume_matrices;
    std::vector<const TriangleMesh *> m_meshes;
    std::vector<Part>        m_parts;
};

// A volume one of the tools adds: its mesh in the world coordinates of `instance_matrix`, its type and name.
struct ToolVolume
{
    indexed_triangle_set world_mesh;
    ModelVolumeType      type;
    std::string          name;
};

// Adds the volumes to object `object_idx` (whose instance has `instance_matrix`), with one undo snapshot,
// and refreshes the object list and the scene. The selection stays on the instance. Empty meshes are
// skipped; returns how many volumes were added.
size_t add_tool_volumes(int object_idx, const Transform3d &instance_matrix, std::vector<ToolVolume> volumes,
                        const std::string &snapshot_name);

// "3.4", "3", "0.25": a length in mm for a volume's name, without trailing zeros.
std::string format_mm(double value);

// Draws a model whose mesh is in world coordinates (moved by `matrix`) over the scene, lit and see-through.
// Call between begin_tool_render() and end_tool_render(), which clear the depth buffer so the model shows
// through the object it is in.
bool begin_tool_render();
void render_tool_model(GLModel &model, const ColorRGBA &color, const Transform3d &matrix = Transform3d::Identity());
void end_tool_render();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoModelTools_hpp_
