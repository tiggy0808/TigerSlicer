#include "GLGizmoHole.hpp"

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include <imgui/imgui_internal.h>

#include <algorithm>

namespace Slic3r::GUI {

using namespace ModelTools;

namespace {

constexpr float kMinDiameter = 0.5f;
constexpr float kMaxDiameter = 30.f;
constexpr float kMinDepth    = 0.5f;
constexpr float kMaxDepth    = 300.f;
// A hole starts this far over the surface, so that it opens cleanly.
constexpr double kAbove = 0.5;
// A through hole runs this far past where it leaves the object.
constexpr double kExitMargin = 1.;

// Blue, to stand out on the usual filament colors (and on TigerSlicer pink).
const ColorRGBA kPreviewColor{0.12f, 0.55f, 1.f, 0.7f};

} // namespace

GLGizmoHole::GLGizmoHole(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
    reset_diameter();
}

bool GLGizmoHole::on_init() { return true; }

std::string GLGizmoHole::on_get_name() const { return _u8L("Screw hole"); }

bool GLGizmoHole::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    return selection.is_single_full_instance() && !selection.is_wipe_tower();
}

CommonGizmosDataID GLGizmoHole::on_get_requirements() const { return CommonGizmosDataID::SelectionInfo; }

void GLGizmoHole::on_set_state()
{
    if (m_state == On)
        update_picker();
    else {
        m_picker.clear();
        m_hover.reset();
        m_preview.reset();
    }
}

void GLGizmoHole::data_changed(bool is_serializing)
{
    if (m_state == On)
        update_picker();
}

void GLGizmoHole::update_picker()
{
    const CommonGizmosDataObjects::SelectionInfo *info = m_c->selection_info();
    const ModelObject *object = info != nullptr ? info->model_object() : nullptr;
    if (m_picker.update(object, object != nullptr ? info->get_active_instance() : -1)) {
        m_hover.reset();
        m_preview.reset();
    }
}

void GLGizmoHole::reset_diameter()
{
    const std::vector<ScrewSize> &sizes = screw_sizes();
    m_size     = std::clamp(m_size, 0, int(sizes.size()) - 1);
    m_diameter = float(hole_diameter(sizes[m_size], m_fit));
}

double GLGizmoHole::depth_at(const SurfaceHit &hit) const
{
    if (!m_through)
        return m_depth;
    const double through = m_picker.depth_through(hit.point, hit.normal);
    return through > 0. ? through + kExitMargin : double(m_depth);
}

indexed_triangle_set GLGizmoHole::hole_at(const SurfaceHit &hit) const
{
    return hole_mesh(hit.point, hit.normal, m_diameter, depth_at(hit), kAbove);
}

void GLGizmoHole::update_hover(const Vec2d &mouse)
{
    update_picker();
    std::optional<SurfaceHit> hover = m_picker.pick(mouse, wxGetApp().plater()->get_camera());
    const bool was = m_hover.has_value();
    m_hover        = hover;
    // GLModel::init_from() ignores a model that is already set up.
    m_preview.reset();
    if (m_hover)
        m_preview.init_from(hole_at(*m_hover));
    if (was || m_hover)
        m_parent.set_as_dirty();
}

// By value: adding the volume reloads the scene, which resets m_hover.
void GLGizmoHole::drill(SurfaceHit hit)
{
    const std::vector<ScrewSize> &sizes = screw_sizes();
    // A typed diameter that differs from the table's is in the name, so the list shows what was drilled.
    std::string name = hole_name(sizes[m_size], m_fit);
    if (std::abs(m_diameter - hole_diameter(sizes[m_size], m_fit)) > 1e-3)
        name += " " + format_mm(m_diameter) + " mm";
    std::vector<ToolVolume> volumes;
    volumes.push_back({hole_at(hit), ModelVolumeType::NEGATIVE_VOLUME, name});
    const Transform3d instance = m_picker.instance_matrix();
    add_tool_volumes(m_parent.get_selection().get_object_idx(), instance, std::move(volumes), _u8L("Add screw hole"));
    m_hover.reset();
    m_preview.reset();
}

bool GLGizmoHole::on_mouse(const wxMouseEvent &mouse_event)
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
            drill(*m_hover);
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

void GLGizmoHole::on_render()
{
    if (!m_hover || !m_preview.is_initialized())
        return;
    if (begin_tool_render()) {
        render_tool_model(m_preview, kPreviewColor);
        end_tool_render();
    }
}

void GLGizmoHole::on_render_input_window(float x, float y, float bottom_limit)
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

    std::vector<wxString> captions = {_L("Screw"), _L("Diameter"), _L("Depth")};
    const float caption_width = m_imgui->find_widest_text(captions) + m_imgui->scaled(1.f);
    const float field_width   = m_imgui->scaled(8.f);

    const std::vector<ScrewSize> &sizes = screw_sizes();
    std::vector<std::string> size_names;
    for (const ScrewSize &size : sizes)
        size_names.push_back(size.name);
    ImGui::AlignTextToFramePadding();
    ImGuiWrapper::push_combo_style(scale);
    if (m_imgui->combo(into_u8(_L("Screw")), size_names, m_size, 0, caption_width, field_width))
        reset_diameter();
    ImGui::PopItemWidth();
    ImGuiWrapper::pop_combo_style();

    if (m_imgui->radio_button(_L("Screw threads into it"), m_fit == HoleFit::ThreadIn) && m_fit != HoleFit::ThreadIn) {
        m_fit = HoleFit::ThreadIn;
        reset_diameter();
    }
    if (m_imgui->radio_button(_L("Screw passes through"), m_fit == HoleFit::Through) && m_fit != HoleFit::Through) {
        m_fit = HoleFit::Through;
        reset_diameter();
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Diameter"));
    ImGui::SameLine(caption_width);
    ImGui::PushItemWidth(field_width);
    ImGui::BBLDragFloat("##hole_diameter", &m_diameter, 0.05f, kMinDiameter, kMaxDiameter, "%.2f mm");
    ImGui::PopItemWidth();
    m_diameter = std::clamp(m_diameter, kMinDiameter, kMaxDiameter);

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Depth"));
    ImGui::SameLine(caption_width);
    m_imgui->bbl_checkbox(_L("All the way through"), m_through);
    if (!m_through) {
        ImGui::AlignTextToFramePadding();
        ImGui::Dummy(ImVec2(0.f, 0.f));
        ImGui::SameLine(caption_width);
        ImGui::PushItemWidth(field_width);
        ImGui::BBLDragFloat("##hole_depth", &m_depth, 0.1f, kMinDepth, kMaxDepth, "%.1f mm");
        ImGui::PopItemWidth();
        m_depth = std::clamp(m_depth, kMinDepth, kMaxDepth);
    }

    ImGui::Separator();
    const float wrap = caption_width + field_width + m_imgui->scaled(4.f);
    m_imgui->text_wrapped(_L("Click the model to drill a hole square to its surface. Each hole is a negative part: "
                             "move, resize or delete it in the object list."),
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
