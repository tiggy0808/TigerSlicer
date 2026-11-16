#ifndef slic3r_GLGizmoHole_hpp_
#define slic3r_GLGizmoHole_hpp_

// TigerSlicer: the hole tool. Click the model to drill a round hole for a metric screw, square to the
// surface: a pilot hole the screw threads into, or a clearance hole it slides through. Each hole is a
// negative volume, so it can be moved, resized or deleted like any other.

#include "GLGizmoBase.hpp"
#include "GLGizmoModelTools.hpp"
#include "libslic3r/Feature/ModelTools/ScrewHole.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include <optional>

namespace Slic3r::GUI {

class GLGizmoHole : public GLGizmoBase
{
public:
    GLGizmoHole(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    bool               on_is_activable() const override;
    void               on_set_state() override;
    void               on_render() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    void                 update_picker();
    void                 update_hover(const Vec2d &mouse);
    double               depth_at(const SurfaceHit &hit) const;
    indexed_triangle_set hole_at(const SurfaceHit &hit) const;
    void                 drill(SurfaceHit hit);
    void                 reset_diameter();

    SurfacePicker             m_picker;
    std::optional<SurfaceHit> m_hover;
    GLModel                   m_preview;
    bool                      m_left_down_used = false;

    int                   m_size    = 2; // into ModelTools::screw_sizes(): M3
    ModelTools::HoleFit   m_fit     = ModelTools::HoleFit::Through;
    float                 m_diameter = 3.4f;
    bool                  m_through = true;
    float                 m_depth   = 10.f;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoHole_hpp_
