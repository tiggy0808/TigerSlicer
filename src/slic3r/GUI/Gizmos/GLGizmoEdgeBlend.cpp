#include "GLGizmoEdgeBlend.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include <imgui/imgui_internal.h>
#include <wx/utils.h>

#include <algorithm>
#include <limits>

namespace Slic3r::GUI {

using namespace ModelTools;

namespace {

constexpr float kMinSize = 0.1f;
constexpr float kMaxSize = 20.f;
// How close (pixels) the mouse must be to an edge to pick it.
constexpr double kPickPixels = 12.;
// Drawn edge thickness (pixels).
constexpr double kEdgeRadiusPixels = 2.5;

// Colors that stand out on the usual filament colors (and on TigerSlicer pink).
const ColorRGBA kHoverColor{1.f, 0.8f, 0.1f, 1.f};
const ColorRGBA kSelectedColor{0.12f, 0.55f, 1.f, 1.f};
const ColorRGBA kCutColor{0.12f, 0.55f, 1.f, 0.5f};
const ColorRGBA kFillColor{0.2f, 0.8f, 0.35f, 0.55f};

Vec3d closest_on_segment(const Vec3d &a, const Vec3d &b, const Vec3d &p)
{
    const Vec3d  ab     = b - a;
    const double length = ab.squaredNorm();
    if (length <= 0.)
        return a;
    return a + std::clamp((p - a).dot(ab) / length, 0., 1.) * ab;
}

template<typename Fn> void for_each_segment(const indexed_triangle_set &its, const EdgeChain &chain, Fn &&fn)
{
    for (size_t k = 0; k < chain.segments(); ++k)
        fn(its.vertices[chain.vertices[k]].cast<double>(), its.vertices[chain.vertices[(k + 1) % chain.vertices.size()]].cast<double>());
}

} // namespace

GLGizmoEdgeBlend::GLGizmoEdgeBlend(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoEdgeBlend::on_init()
{
    m_tube.init_from(smooth_cylinder(12, 1.f, 1.f));
    return true;
}

std::string GLGizmoEdgeBlend::on_get_name() const { return _u8L("Fillet and chamfer"); }

bool GLGizmoEdgeBlend::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    return selection.is_single_full_instance() && !selection.is_wipe_tower();
}

CommonGizmosDataID GLGizmoEdgeBlend::on_get_requirements() const { return CommonGizmosDataID::SelectionInfo; }

void GLGizmoEdgeBlend::on_set_state()
{
    if (m_state == On)
        update_edges();
    else {
        m_picker.clear();
        m_edges.clear();
        m_hover.reset();
        clear_selection();
    }
}

void GLGizmoEdgeBlend::data_changed(bool is_serializing)
{
    if (m_state == On)
        update_edges();
}

void GLGizmoEdgeBlend::clear_selection()
{
    m_selected.clear();
    m_preview_cut.reset();
    m_preview_fill.reset();
    m_preview_dirty = true;
    m_failed        = 0;
}

void GLGizmoEdgeBlend::update_edges()
{
    const CommonGizmosDataObjects::SelectionInfo *info = m_c->selection_info();
    const ModelObject *object = info != nullptr ? info->model_object() : nullptr;
    if (!m_picker.update(object, object != nullptr ? info->get_active_instance() : -1))
        return;
    wxBusyCursor wait;
    m_edges.clear();
    for (const SurfacePicker::Part &part : m_picker.parts()) {
        PartEdges edges;
        edges.world = part.mesh->its;
        // Keep the faces facing out when the part or instance is mirrored.
        its_transform(edges.world, part.world, true);
        edges.chains = sharp_edges(edges.world);
        for (size_t c = 0; c < edges.chains.size(); ++c)
            for_each_segment(edges.world, edges.chains[c], [&](const Vec3d &a, const Vec3d &b) { edges.segments.push_back({a, b, c}); });
        m_edges.push_back(std::move(edges));
    }
    m_hover.reset();
    clear_selection();
}

std::optional<GLGizmoEdgeBlend::ChainRef> GLGizmoEdgeBlend::chain_under(const SurfaceHit &hit, const Vec2d &mouse) const
{
    if (hit.part >= m_edges.size())
        return std::nullopt;
    std::optional<ChainRef> best;
    Vec3d  best_point    = Vec3d::Zero();
    double best_distance = std::numeric_limits<double>::max();
    for (const Segment &segment : m_edges[hit.part].segments) {
        const Vec3d  q        = closest_on_segment(segment.a, segment.b, hit.point);
        const double distance = (q - hit.point).squaredNorm();
        if (distance < best_distance) {
            best_distance = distance;
            best_point    = q;
            best          = ChainRef{hit.part, segment.chain};
        }
    }
    if (!best)
        return std::nullopt;
    const Point on_screen = CameraUtils::project(wxGetApp().plater()->get_camera(), best_point);
    if ((Vec2d(on_screen.x(), on_screen.y()) - mouse).norm() > kPickPixels * m_parent.get_scale())
        return std::nullopt;
    return best;
}

// Off the model, the nearest edge on screen, if nothing hides it: an edge on the outline is easiest to
// point at from just outside.
std::optional<GLGizmoEdgeBlend::ChainRef> GLGizmoEdgeBlend::chain_on_screen(const Vec2d &mouse) const
{
    const Camera           &camera = wxGetApp().plater()->get_camera();
    std::optional<ChainRef> best;
    Vec3d                   best_point    = Vec3d::Zero();
    double                  best_distance = kPickPixels * m_parent.get_scale();
    for (size_t p = 0; p < m_edges.size(); ++p) {
        const std::vector<Segment> &segments = m_edges[p].segments;
        std::vector<Vec3d>          ends;
        ends.reserve(2 * segments.size());
        for (const Segment &segment : segments) {
            ends.push_back(segment.a);
            ends.push_back(segment.b);
        }
        const Points screen = CameraUtils::project(camera, ends);
        for (size_t i = 0; i < segments.size(); ++i) {
            const Vec2d  a(screen[2 * i].x(), screen[2 * i].y()), b(screen[2 * i + 1].x(), screen[2 * i + 1].y());
            const Vec2d  ab     = b - a;
            const double length = ab.squaredNorm();
            const double t      = length > 0. ? std::clamp((mouse - a).dot(ab) / length, 0., 1.) : 0.;
            const double d      = (a + t * ab - mouse).norm();
            if (d < best_distance) {
                best_distance = d;
                best_point    = segments[i].a + t * (segments[i].b - segments[i].a);
                best          = ChainRef{p, segments[i].chain};
            }
        }
    }
    if (best && !m_picker.is_visible(best_point, camera))
        return std::nullopt;
    return best;
}

void GLGizmoEdgeBlend::update_hover(const Vec2d &mouse)
{
    update_edges();
    std::optional<ChainRef>   hover;
    std::optional<SurfaceHit> hit = m_picker.pick(mouse, wxGetApp().plater()->get_camera());
    hover                         = hit ? chain_under(*hit, mouse) : chain_on_screen(mouse);
    if (hover != m_hover) {
        m_hover = hover;
        m_parent.set_as_dirty();
    }
}

void GLGizmoEdgeBlend::toggle(const ChainRef &ref)
{
    auto it = std::find(m_selected.begin(), m_selected.end(), ref);
    if (it == m_selected.end())
        m_selected.push_back(ref);
    else
        m_selected.erase(it);
    m_preview_dirty = true;
    m_parent.set_as_dirty();
}

void GLGizmoEdgeBlend::update_preview()
{
    m_preview_dirty = false;
    m_failed        = 0;
    indexed_triangle_set cut, fill;
    for (const ChainRef &ref : m_selected) {
        const PartEdges     &edges = m_edges[ref.part];
        const EdgeChain     &chain = edges.chains[ref.chain];
        indexed_triangle_set blend = edge_blend_mesh(edges.world, chain, m_type, m_size);
        if (blend.indices.empty())
            ++m_failed;
        else
            its_merge(chain.convex ? cut : fill, blend);
    }
    // GLModel::init_from() ignores a model that is already set up.
    m_preview_cut.reset();
    m_preview_fill.reset();
    if (!cut.indices.empty())
        m_preview_cut.init_from(cut);
    if (!fill.indices.empty())
        m_preview_fill.init_from(fill);
}

void GLGizmoEdgeBlend::apply()
{
    const std::string name = (m_type == EdgeBlend::Fillet ? _u8L("Fillet") : _u8L("Chamfer")) + " " + format_mm(m_size) + " mm";
    std::vector<ToolVolume> volumes;
    for (const ChainRef &ref : m_selected) {
        const PartEdges &edges = m_edges[ref.part];
        const EdgeChain &chain = edges.chains[ref.chain];
        volumes.push_back({edge_blend_mesh(edges.world, chain, m_type, m_size),
                           chain.convex ? ModelVolumeType::NEGATIVE_VOLUME : ModelVolumeType::MODEL_PART, name});
    }
    const std::string snapshot = m_type == EdgeBlend::Fillet ? _u8L("Fillet edges") : _u8L("Chamfer edges");
    const Transform3d instance = m_picker.instance_matrix();
    clear_selection();
    add_tool_volumes(m_parent.get_selection().get_object_idx(), instance, std::move(volumes), snapshot);
}

bool GLGizmoEdgeBlend::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse(mouse_event.GetX(), mouse_event.GetY());
    if (mouse_event.Moving()) {
        update_hover(mouse);
        return false;
    }
    if (mouse_event.LeftDown()) {
        // Ctrl+click turns the view, as elsewhere.
        if (mouse_event.CmdDown())
            return false;
        update_hover(mouse);
        if (m_hover) {
            toggle(*m_hover);
            m_left_down_used = true;
            return true;
        }
        return false;
    }
    if (mouse_event.LeftUp() && m_left_down_used) {
        m_left_down_used = false;
        return true;
    }
    if (mouse_event.Dragging() && m_left_down_used)
        return true;
    return false;
}

void GLGizmoEdgeBlend::render_chain(const ChainRef &ref, const ColorRGBA &color, double radius)
{
    if (ref.part >= m_edges.size() || ref.chain >= m_edges[ref.part].chains.size())
        return;
    const PartEdges &edges = m_edges[ref.part];
    for_each_segment(edges.world, edges.chains[ref.chain], [&](const Vec3d &a, const Vec3d &b) {
        const Vec3d       d      = b - a;
        const Transform3d matrix = Geometry::translation_transform(a) * Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), d) *
                                   Geometry::scale_transform({radius, radius, d.norm()});
        render_tool_model(m_tube, color, matrix);
    });
}

void GLGizmoEdgeBlend::on_render()
{
    if (m_preview_dirty)
        update_preview();
    if (!m_hover && m_selected.empty())
        return;
    if (!begin_tool_render())
        return;
    const double radius = kEdgeRadiusPixels * wxGetApp().plater()->get_camera().get_inv_zoom();
    render_tool_model(m_preview_cut, kCutColor);
    render_tool_model(m_preview_fill, kFillColor);
    for (const ChainRef &ref : m_selected)
        render_chain(ref, kSelectedColor, radius);
    if (m_hover)
        render_chain(*m_hover, kHoverColor, 1.4 * radius);
    end_tool_render();
}

void GLGizmoEdgeBlend::on_render_input_window(float x, float y, float bottom_limit)
{
    static float last_y = 0.f;
    static float last_h = 0.f;

    const float win_h = ImGui::GetWindowHeight();
    y                 = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    const float scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(scale);
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    std::vector<wxString> captions = {_L("Radius"), _L("Distance")};
    const float caption_width = m_imgui->find_widest_text(captions) + m_imgui->scaled(1.f);
    const float field_width   = m_imgui->scaled(8.f);

    if (m_imgui->radio_button(_L("Fillet"), m_type == EdgeBlend::Fillet) && m_type != EdgeBlend::Fillet) {
        m_type          = EdgeBlend::Fillet;
        m_preview_dirty = true;
    }
    ImGui::SameLine();
    if (m_imgui->radio_button(_L("Chamfer"), m_type == EdgeBlend::Chamfer) && m_type != EdgeBlend::Chamfer) {
        m_type          = EdgeBlend::Chamfer;
        m_preview_dirty = true;
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_type == EdgeBlend::Fillet ? _L("Radius") : _L("Distance"));
    ImGui::SameLine(caption_width);
    ImGui::PushItemWidth(field_width);
    const float before = m_size;
    ImGui::BBLDragFloat("##blend_size", &m_size, 0.05f, kMinSize, kMaxSize, "%.2f mm");
    ImGui::PopItemWidth();
    m_size = std::clamp(m_size, kMinSize, kMaxSize);
    if (m_size != before)
        m_preview_dirty = true;

    ImGui::Separator();
    const float wrap = caption_width + field_width + m_imgui->scaled(6.f);
    if (m_selected.empty())
        m_imgui->text_wrapped(_L("Click the model's sharp edges to pick them. Click an edge again to drop it."), wrap);
    else
        m_imgui->text(wxString::Format(_L("%d edges picked"), int(m_selected.size())));
    if (m_failed > 0)
        m_imgui->warning_text(wxString::Format(_L("%d of them can't be rounded: their faces meet too sharply or too flat."), int(m_failed)));

    const bool can_apply = !m_selected.empty() && m_failed < m_selected.size();
    if (m_imgui->button(m_type == EdgeBlend::Fillet ? _L("Round edges") : _L("Bevel edges"), ImVec2(0.f, 0.f), can_apply) && can_apply)
        apply();
    ImGui::SameLine();
    if (m_imgui->button(_L("Clear"), ImVec2(0.f, 0.f), !m_selected.empty()) && !m_selected.empty()) {
        clear_selection();
        m_parent.set_as_dirty();
    }

    ImGui::Separator();
    m_imgui->text_wrapped(_L("Outside edges are cut with a negative part, inside corners filled with a part, so the "
                             "model itself is unchanged. Cuts show once sliced. Where picked edges meet at a corner, "
                             "the corner itself stays sharp."),
                          wrap);

    const float h = ImGui::GetWindowHeight();
    if (last_h != h || last_y != y) {
        m_imgui->set_requires_extra_frame();
        last_h = h;
        last_y = y;
    }
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
