#ifndef slic3r_GLGizmoEdgeBlend_hpp_
#define slic3r_GLGizmoEdgeBlend_hpp_

// TigerSlicer: the fillet and chamfer tool. Click the model's sharp edges to pick them, then round or bevel
// them. An outside edge is cut with a negative volume and an inside corner filled with a part; the
// model's own mesh is not changed.

#include "GLGizmoBase.hpp"
#include "GLGizmoModelTools.hpp"
#include "libslic3r/Feature/ModelTools/EdgeBlend.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include <optional>
#include <vector>

namespace Slic3r::GUI {

class GLGizmoEdgeBlend : public GLGizmoBase
{
public:
    GLGizmoEdgeBlend(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

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
    struct Segment
    {
        Vec3d  a, b;
        size_t chain;
    };
    // A part's mesh in world coordinates, its sharp edges, and their segments.
    struct PartEdges
    {
        indexed_triangle_set               world;
        std::vector<ModelTools::EdgeChain> chains;
        std::vector<Segment>               segments;
    };
    struct ChainRef
    {
        size_t part  = 0;
        size_t chain = 0;
        bool   operator==(const ChainRef &other) const { return part == other.part && chain == other.chain; }
        bool   operator!=(const ChainRef &other) const { return !(*this == other); }
    };

    void                    update_edges();
    void                    update_hover(const Vec2d &mouse);
    std::optional<ChainRef> chain_under(const SurfaceHit &hit, const Vec2d &mouse) const;
    std::optional<ChainRef> chain_on_screen(const Vec2d &mouse) const;
    void                    toggle(const ChainRef &ref);
    void                    update_preview();
    void                    apply();
    void                    clear_selection();
    void                    render_chain(const ChainRef &ref, const ColorRGBA &color, double radius);

    SurfacePicker           m_picker;
    std::vector<PartEdges>  m_edges;
    std::optional<ChainRef> m_hover;
    std::vector<ChainRef>   m_selected;
    GLModel                 m_tube; // unit cylinder for drawing edge segments
    GLModel                 m_preview_cut;
    GLModel                 m_preview_fill;
    bool                    m_preview_dirty  = true;
    size_t                  m_failed         = 0; // selected chains that cannot be blended
    bool                    m_left_down_used = false;

    ModelTools::EdgeBlend m_type = ModelTools::EdgeBlend::Fillet;
    float                 m_size = 1.f;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoEdgeBlend_hpp_
