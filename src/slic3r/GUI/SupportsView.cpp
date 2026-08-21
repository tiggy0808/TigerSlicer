///|/ TigerSlicer MSLA Supports workspace.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SupportsView.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r/MSlicerResinJob.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Init.hpp"
#include "I18N.hpp"
#include "MeshUtils.hpp"
#include "OpenGLManager.hpp"
#include "OptionsGroup.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

#include <wx/button.h>
#include <wx/glcanvas.h> // IWYU pragma: keep
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

namespace Slic3r {
namespace GUI {

namespace {

const wxColour kPink(246, 165, 193);
const wxColour kBlack(24, 24, 27);
const wxColour kDimmed(176, 168, 173);

// One named starting point. A preset only writes these values into the preset;
// every one of them stays visible and editable afterwards, so choosing a
// preset never hides what it did.
struct SupportPreset
{
    const char *name;
    const char *description;
    double pillar_diameter;
    double head_front_diameter;
    double head_width;
    int    density;
    double penetration;
};

const SupportPreset kPresets[] = {
    { "Light",  "Thin columns and fewer contacts. Easiest to remove and leaves the least "
                "marking; least able to hold a heavy or wide overhang.",
      0.8, 0.30, 2.0,  70, 0.10 },
    { "Medium", "The balanced starting point: what these settings default to.",
      1.0, 0.40, 3.0, 100, 0.20 },
    { "Heavy",  "Thick columns and more contacts, biting deeper. Holds large flat "
                "overhangs; takes more force to remove and marks the surface more.",
      1.4, 0.60, 3.5, 150, 0.30 },
};

} // namespace

SupportsView::SupportsView(wxWindow *parent, Bed3D &bed, Model *model, DynamicPrintConfig *config,
                           BackgroundSlicingProcess *process)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return;

    // The canvas is built exactly as View3D builds its own, including the
    // shared GL context. Gizmos and the main toolbar stay off: this workspace
    // is for support settings, and a second set of editing tools acting on the
    // same model from a different view is a state-synchronisation hazard with
    // nothing to gain.
    const GUI_InitParams *const init_params = wxGetApp().init_params;
    (void)init_params;
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this);
    if (m_canvas_widget == nullptr)
        return;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));
    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_config(config);
    // A viewer, configured like Preview's canvas rather than the editor's:
    // picking, selection, moving, gizmos and the editing toolbars all stay off.
    // Editing the model from a second view while the 3D editor holds the same
    // selection is a state-synchronisation hazard with nothing to gain here -
    // this workspace acts on the whole plate, not on a selected object.
    m_canvas->enable_dynamic_background(true);
    // Manual support points render as markers here, and only here: this is
    // the workspace where they are placed and removed, and without markers a
    // placed point gives no visible feedback at all.
    m_canvas->enable_msla_manual_point_markers(true);

    // Manual point placement: while the toggle below is on, the canvas turns
    // plain clicks into these events instead of camera drags. Drain holes
    // are placed from the workbench ribbon in the 3D editor.
    m_canvas_widget->Bind(EVT_GLCANVAS_MSLA_PLACE_POINT,
                          [this](Vec2dEvent &evt) { on_place_point(evt.data); });
    m_canvas_widget->Bind(EVT_GLCANVAS_MSLA_DELETE_POINT,
                          [this](Vec2dEvent &evt) { on_delete_point(evt.data); });

    auto *root = new wxBoxSizer(wxHORIZONTAL);
    // The settings column is parented to the plater, not this view: it takes
    // the sidebar's slot at full height while this view is active, so the
    // workbench ribbon keeps its place over the canvas instead of expanding
    // across the whole window.
    build_settings_panel(parent);
    m_settings_panel->Hide();
    root->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);
    SetSizer(root);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);
}

SupportsView::~SupportsView()
{
    // The option groups hold fields parented to m_settings_panel. Release
    // them first so no field outlives the window it is parented to. The
    // panel itself is the plater's child and the plater destroys children in
    // creation order: this view first (created before the panel), then the
    // panel - so the groups are always gone before their windows are.
    m_groups.clear();
    delete m_canvas;
    delete m_canvas_widget;
}

namespace {

// The wheel scrolls the settings column from anywhere inside it. Without the
// explicit binding, Windows routes wheel input to whichever child sits under
// the cursor and the panel itself rarely sees it; with it, the wheel always
// pages the column and never spins a value in a field by accident.
//
// The rotation accumulator is shared by every binding on the panel: precision
// wheels and touchpads deliver rotations smaller than one notch per event,
// and truncating each event separately would drop them all.
void bind_wheel_scroll(wxWindow *window, wxScrolledWindow *panel, const std::shared_ptr<int> &accumulated)
{
    window->Bind(wxEVT_MOUSEWHEEL, [panel, accumulated](wxMouseEvent &evt) {
        if (evt.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) {
            evt.Skip();
            return;
        }
        *accumulated += evt.GetWheelRotation();
        const int notches = *accumulated / evt.GetWheelDelta();
        if (notches == 0)
            return;
        *accumulated -= notches * evt.GetWheelDelta();
        int x = 0, y = 0;
        panel->GetViewStart(&x, &y);
        // Three scroll units per notch; the unit is one em. Scroll() reads a
        // -1 coordinate as "leave unchanged", so an up-scroll near the top
        // must clamp at 0 or the panel sticks just short of it.
        panel->Scroll(x, std::max(0, y - notches * 3));
    });
    for (wxWindow *child : window->GetChildren())
        bind_wheel_scroll(child, panel, accumulated);
}

} // namespace

void SupportsView::build_settings_panel(wxWindow *parent)
{
    const int em = wxGetApp().em_unit();

    m_settings_panel = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                            wxVSCROLL | wxBORDER_NONE);
    // One scroll unit per em: a wheel notch (three units) moves about three
    // text lines whatever the DPI. A fixed 5 px unit was near-stationary on
    // this panel's ~1500 px of content at 200% scaling.
    m_settings_panel->SetScrollRate(0, em);
    m_settings_panel->SetBackgroundColour(kBlack);
    m_settings_panel->SetMinSize(wxSize(34 * em, -1));

    auto *column = new wxBoxSizer(wxVERTICAL);

    m_heading = new wxStaticText(m_settings_panel, wxID_ANY, _L("Resin settings"));
    m_heading->SetFont(wxGetApp().bold_font());
    m_heading->SetForegroundColour(kPink);
    column->Add(m_heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    DynamicPrintConfig *config = &wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    auto add_group = [this, config, column, em](const wxString &title,
                                                const std::vector<std::string> &keys) {
        auto group = std::make_shared<ConfigOptionsGroup>(m_settings_panel, title, config);
        group->set_config_category_and_type(title, Preset::TYPE_SLA_PRINT);
        group->m_on_change = [this](const t_config_option_key &opt_key, const boost::any &value) {
            on_option_changed(opt_key, value);
        };
        for (const std::string &key : keys)
            group->append_single_option_line(key);
        if (group->activate() && group->sizer != nullptr) {
            column->Add(group->sizer, 0, wxEXPAND | wxALL, em);
            m_groups.push_back(std::move(group));
        }
    };

    // Layers live on the Print Settings (material) tab, hollowing and drain
    // holes on the workbench ribbon; this panel holds supports and the
    // print-level details. No Pad group: the engine refuses pad_enable
    // (supports carry their own base pads), so the checkbox could only
    // make slicing fail.

    // Section label for the support half of the panel.
    auto *supports_label = new wxStaticText(m_settings_panel, wxID_ANY, _L("Supports"));
    supports_label->SetFont(wxGetApp().bold_font());
    supports_label->SetForegroundColour(kPink);
    column->Add(supports_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    auto *presets = new wxBoxSizer(wxHORIZONTAL);
    for (const SupportPreset &preset : kPresets) {
        auto *button = new wxButton(m_settings_panel, wxID_ANY, _(preset.name),
                                    wxDefaultPosition, wxSize(7 * em, 2 * em), wxBU_EXACTFIT);
        button->SetToolTip(_(preset.description));
        wxGetApp().UpdateDarkUI(button);
        const char *name = preset.name;
        button->Bind(wxEVT_BUTTON, [this, name](wxCommandEvent &) { apply_preset(name); });
        presets->Add(button, 1, wxRIGHT, int(0.3 * em));
        m_preset_buttons.push_back(button);
    }
    column->Add(presets, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_preset_note = new wxStaticText(
        m_settings_panel, wxID_ANY,
        _L("A preset only fills in the values below. Every one of them stays editable, and "
           "nothing is applied to the model until supports are generated."));
    m_preset_note->SetForegroundColour(kDimmed);
    m_preset_note->SetFont(wxGetApp().small_font());
    m_preset_note->Wrap(30 * em);
    column->Add(m_preset_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // Grouped the way a resin slicer's support panel groups them: what is
    // detected, what touches the model, what carries the load, what it stands
    // on, and how finely the result is built.
    const struct { const char *title; std::vector<std::string> keys; } sections[] = {
        { L("Detection"), { "msla_support_overhang_angle",
                            "msla_support_edge_angle_delta",
                            "support_points_density_relative",
                            "msla_support_min_overhang_area",
                            "msla_support_min_height" } },
        { L("Contact"),   { "support_head_front_diameter",
                            "support_head_width",
                            "msla_support_contact_penetration" } },
        { L("Pillar"),    { "support_pillar_diameter" } },
        { L("Structure"), { "msla_support_on_model",
                            "msla_support_adaptive_diameter",
                            "msla_support_max_pillar_diameter",
                            "msla_support_cross_bracing",
                            "msla_support_brace_spacing",
                            "msla_support_manual_only",
                            "support_enforcers_only" } },
        { L("Base"),      { "msla_support_base_kind",
                            "msla_support_base_diameter",
                            "support_base_height",
                            "support_base_safety_distance" } },
        { L("Generated mesh"), { "msla_support_precision" } },
    };

    for (const auto &section : sections) {
        auto group = std::make_shared<ConfigOptionsGroup>(m_settings_panel, _(section.title), config);
        group->set_config_category_and_type(_L("Supports"), Preset::TYPE_SLA_PRINT);
        group->m_on_change = [this](const t_config_option_key &opt_key, const boost::any &value) {
            on_option_changed(opt_key, value);
        };
        for (const std::string &key : section.keys)
            group->append_single_option_line(key);
        // Every option is shown whatever the application's simple/expert mode
        // is. A workspace opened specifically to customize supports should not
        // hide the customization; the mode filter belongs on the general
        // settings pages, where these same options also appear.
        if (!group->activate() || group->sizer == nullptr)
            continue;
        column->Add(group->sizer, 0, wxEXPAND | wxALL, em);
        m_groups.push_back(std::move(group));
    }

    auto *points_row = new wxBoxSizer(wxHORIZONTAL);
    // Borderless: the native themed frame and its white hot-state flash do
    // not exist on the neighbouring buttons, and the armed-mode pink is the
    // whole state indicator anyway. Hover feedback is the text turning pink.
    m_place_points = new wxToggleButton(m_settings_panel, wxID_ANY, _L("Place points"),
                                        wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_place_points->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &evt) {
        if (!m_place_points->GetValue()) {
            m_place_points->SetForegroundColour(kPink);
            m_place_points->Refresh();
        }
        evt.Skip();
    });
    m_place_points->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &evt) {
        update_place_points_appearance();
        evt.Skip();
    });
    m_place_points->SetToolTip(_L("While on: left-click the model to place a manual support "
                                  "point there, right-click to remove the nearest one. Points "
                                  "are kept with the project and added to automatic detection - "
                                  "or used alone with 'Manual points only'. Turn the mode off "
                                  "to rotate the view with the mouse again."));
    m_place_points->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &) {
        // Arming point placement disarms hole placement; one click, one meaning.
        if (m_place_points->GetValue() && m_place_holes != nullptr)
            m_place_holes->SetValue(false);
        if (m_canvas != nullptr)
            m_canvas->enable_msla_point_placement(
                m_place_points->GetValue() ||
                (m_place_holes != nullptr && m_place_holes->GetValue()));
        update_place_points_appearance();
    });
    m_clear_points = new wxButton(m_settings_panel, wxID_ANY, _L("Clear points"));
    m_clear_points->SetToolTip(_L("Remove every manually placed support point from every object."));
    m_clear_points->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { clear_manual_points(); });
    wxGetApp().UpdateDarkUI(m_place_points);
    wxGetApp().UpdateDarkUI(m_clear_points);
    points_row->Add(m_place_points, 1, wxRIGHT, int(0.3 * em));
    points_row->Add(m_clear_points, 1);
    column->Add(points_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_auto_orient = new wxButton(m_settings_panel, wxID_ANY, _L("Auto-orient for supports"));
    m_auto_orient->SetToolTip(_L("Rotate each object to the orientation that leaves the least "
                                 "overhanging area to hold up, judged at the detection angle "
                                 "above. Supports already on the plate are regenerated "
                                 "afterwards."));
    m_auto_orient->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxGetApp().plater()->auto_orient_for_msla_supports();
        reload_scene(true);
        update_status();
    });
    wxGetApp().UpdateDarkUI(m_auto_orient);
    column->Add(m_auto_orient, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    auto *actions = new wxBoxSizer(wxHORIZONTAL);
    m_generate = new wxButton(m_settings_panel, wxID_ANY, _L("Generate supports"));
    m_generate->SetToolTip(_L("Build the support geometry for the current settings and add it to "
                              "the plate as a visible object. Slicing uses exactly this mesh."));
    m_generate->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxGetApp().plater()->prepare_msla_supports();
        reload_scene(true);
        update_status();
    });
    m_remove = new wxButton(m_settings_panel, wxID_ANY, _L("Remove supports"));
    m_remove->SetToolTip(_L("Delete the generated support object. Slicing then refuses until "
                            "supports are generated again, or until they are turned off."));
    m_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxGetApp().plater()->remove_prepared_msla_supports();
        reload_scene(true);
        update_status();
    });
    wxGetApp().UpdateDarkUI(m_generate);
    wxGetApp().UpdateDarkUI(m_remove);
    actions->Add(m_generate, 1, wxRIGHT, int(0.3 * em));
    actions->Add(m_remove, 1);
    column->Add(actions, 0, wxEXPAND | wxALL, em);

    // The remaining print-level details, relocated here from the retired
    // MSLA Print Settings tab.
    add_group(_L("Slicing"), { "slice_closing_radius", "slicing_mode" });
    {
        auto group = std::make_shared<ConfigOptionsGroup>(m_settings_panel, _L("Output file"), config);
        group->set_config_category_and_type(_L("Output file"), Preset::TYPE_SLA_PRINT);
        group->m_on_change = [this](const t_config_option_key &opt_key, const boost::any &value) {
            on_option_changed(opt_key, value);
        };
        Option option = group->get_option("output_filename_format");
        option.opt.full_width = true;
        group->append_single_option_line(option);
        if (group->activate() && group->sizer != nullptr) {
            column->Add(group->sizer, 0, wxEXPAND | wxALL, em);
            m_groups.push_back(std::move(group));
        }
    }

    // The MSLA Print Settings tab is not shown, so saving the print profile
    // happens from here.
    auto *save_print = new wxButton(m_settings_panel, wxID_ANY, _L("Save print settings profile") + "...");
    save_print->SetToolTip(_L("Save every setting in this panel that belongs to the print profile "
                              "(everything except the material and printer values) as a named profile."));
    save_print->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
        if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_SLA_PRINT))
            tab->save_preset();
    });
    wxGetApp().UpdateDarkUI(save_print);
    column->Add(save_print, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_status = new wxStaticText(m_settings_panel, wxID_ANY, wxEmptyString);
    m_status->SetForegroundColour(kDimmed);
    m_status->Wrap(30 * em);
    column->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);

    m_settings_panel->SetSizer(column);
    m_settings_panel->FitInside();
    bind_wheel_scroll(m_settings_panel, m_settings_panel, std::make_shared<int>(0));
    // The option-group rows are wider than the nominal panel width, and the
    // panel scrolls only vertically. A fixed width narrower than the content
    // clips every proportionally-sized row at the right edge, so the width
    // follows the content instead.
    m_settings_panel->SetMinSize(wxSize(settings_panel_width(), -1));
    update_status();
}

int SupportsView::settings_panel_width() const
{
    const int em = wxGetApp().em_unit();
    int width = 34 * em;
    if (m_settings_panel != nullptr && m_settings_panel->GetSizer() != nullptr)
        width = std::max(width, m_settings_panel->GetSizer()->GetMinSize().x
                                    + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X));
    return width;
}

void SupportsView::update_place_points_appearance()
{
    // The native toggle state is nearly invisible on the dark theme; while
    // placement mode is armed the button wears the active pink so there is no
    // guessing whether the next click will place something or move the camera.
    auto style = [](wxToggleButton *toggle, const wxString &armed, const wxString &idle) {
        if (toggle == nullptr)
            return;
        if (toggle->GetValue()) {
            toggle->SetBackgroundColour(kPink);
            toggle->SetForegroundColour(kBlack);
            toggle->SetLabel(armed);
        } else {
            toggle->SetBackgroundColour(wxNullColour);
            toggle->SetForegroundColour(wxNullColour);
            wxGetApp().UpdateDarkUI(toggle);
            toggle->SetLabel(idle);
        }
        toggle->Refresh();
    };
    style(m_place_points, _L("Placing points - click model"), _L("Place points"));
    style(m_place_holes, _L("Placing holes - click model"), _L("Place drain holes"));
}

void SupportsView::on_option_changed(const std::string &opt_key, const boost::any &)
{
    if (m_loading)
        return;
    // The group already stored the value in the edited preset, which is the
    // same object the MSLA print settings page edits. Tell that page its
    // preset moved, then apply the shared support policy for this key so a
    // prepared mesh cannot outlive the settings it was generated from.
    //
    // update_dirty() only, matching the sidebar chooser's proven path. The
    // settings page re-reads its fields when it is next activated; calling its
    // update() from here would run its toggling logic mid-edit for no gain.
    if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_SLA_PRINT); tab != nullptr)
        tab->update_dirty();
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        plater->msla_support_option_changed(opt_key);
    update_status();
}

namespace {

// The model surface point under the cursor, as the owning object plus the hit
// in that object's own mesh coordinates. Nothing under the cursor -> nullptr.
// When local_normal is given it receives the surface normal at the hit, also
// in mesh coordinates.
ModelObject *raycast_model_point(const Vec2d &pos, Vec3f &local_hit, Vec3f *local_normal = nullptr)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return nullptr;
    const Camera &camera = plater->get_camera();
    double best_dist = std::numeric_limits<double>::max();
    ModelObject *best_object = nullptr;
    for (ModelObject *object : plater->model().objects) {
        if (!object->printable || MSlicerResinJob::is_prepared_support_object(*object))
            continue;
        const TriangleMesh mesh = object->raw_mesh();
        if (mesh.empty())
            continue;
        // Built per click rather than cached: a cache would need every model
        // edit to invalidate it, and a click is not a hot path.
        MeshRaycaster raycaster(mesh);
        for (const ModelInstance *instance : object->instances) {
            if (!instance->is_printable())
                continue;
            const Transform3d trafo = instance->get_matrix();
            Vec3f hit = Vec3f::Zero();
            Vec3f normal = Vec3f::Zero();
            if (!raycaster.unproject_on_mesh(pos, trafo, camera, hit, normal))
                continue;
            const double dist = (trafo * hit.cast<double>() - camera.get_position()).norm();
            if (dist < best_dist) {
                best_dist = dist;
                best_object = object;
                local_hit = hit;
                if (local_normal != nullptr)
                    *local_normal = normal;
            }
        }
    }
    return best_object;
}

} // namespace

void SupportsView::on_place_point(const Vec2d &pos)
{
    Vec3f local_hit = Vec3f::Zero();
    ModelObject *object = raycast_model_point(pos, local_hit);
    if (object == nullptr)
        // The click missed the model; placing a support on nothing is not a
        // thing, and silently snapping to the nearest surface would put the
        // point somewhere the user did not aim.
        return;
    Plater *plater = wxGetApp().plater();
    plater->take_snapshot(std::string("Add Manual Support Point"));
    const DynamicPrintConfig &config = wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    const float radius = float(config.opt_float("support_head_front_diameter") * 0.5);
    object->sla_support_points.push_back(sla::SupportPoint{local_hit, radius});
    // The prepared mesh was generated without this point.
    plater->remove_prepared_msla_supports();
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
    update_status();
}

void SupportsView::on_delete_point(const Vec2d &pos)
{
    Vec3f local_hit = Vec3f::Zero();
    ModelObject *object = raycast_model_point(pos, local_hit);
    if (object == nullptr || object->sla_support_points.empty())
        return;
    // Nearest stored point to the clicked surface position, within a small
    // reach so a stray click far from any point removes nothing.
    constexpr float max_reach_mm = 3.f;
    size_t best_index = object->sla_support_points.size();
    float best_dist = max_reach_mm;
    for (size_t i = 0; i < object->sla_support_points.size(); ++i) {
        const float dist = (object->sla_support_points[i].pos - local_hit).norm();
        if (dist < best_dist) {
            best_dist = dist;
            best_index = i;
        }
    }
    if (best_index == object->sla_support_points.size())
        return;
    Plater *plater = wxGetApp().plater();
    plater->take_snapshot(std::string("Remove Manual Support Point"));
    object->sla_support_points.erase(object->sla_support_points.begin() + best_index);
    plater->remove_prepared_msla_supports();
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
    update_status();
}

void SupportsView::clear_manual_points()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    bool any = false;
    for (const ModelObject *object : plater->model().objects)
        any |= !object->sla_support_points.empty();
    if (!any)
        return;
    plater->take_snapshot(std::string("Clear Manual Support Points"));
    for (ModelObject *object : plater->model().objects)
        object->sla_support_points.clear();
    plater->remove_prepared_msla_supports();
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
    update_status();
}

void SupportsView::apply_preset(const char *name)
{
    const SupportPreset *preset = nullptr;
    for (const SupportPreset &candidate : kPresets)
        if (std::strcmp(candidate.name, name) == 0)
            preset = &candidate;
    if (preset == nullptr)
        return;

    Tab *tab = wxGetApp().get_tab(Preset::TYPE_SLA_PRINT);
    if (tab == nullptr)
        return;

    DynamicPrintConfig new_conf = wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    new_conf.set_key_value("support_pillar_diameter", new ConfigOptionFloat(preset->pillar_diameter));
    new_conf.set_key_value("support_head_front_diameter",
                           new ConfigOptionFloat(preset->head_front_diameter));
    new_conf.set_key_value("support_head_width", new ConfigOptionFloat(preset->head_width));
    new_conf.set_key_value("support_points_density_relative", new ConfigOptionInt(preset->density));
    new_conf.set_key_value("msla_support_contact_penetration",
                           new ConfigOptionFloat(preset->penetration));
    tab->load_config(new_conf);
    tab->update_dirty();

    reload_settings();
    // One invalidation for the whole preset rather than one per key.
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        plater->msla_support_option_changed("support_pillar_diameter");
    // Requested: choosing a preset also generates right away, so the plate
    // shows what the preset means without a second trip to Generate.
    if (Plater *plater = wxGetApp().plater();
        plater != nullptr && !plater->model().objects.empty()) {
        plater->prepare_msla_supports();
        reload_scene(true);
    }
    update_status();
}

void SupportsView::reload_settings()
{
    if (m_groups.empty())
        return;
    m_loading = true;
    DynamicPrintConfig *config = &wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    for (auto &group : m_groups) {
        // The edited preset is replaced wholesale when a preset is selected, so
        // rebind before reloading rather than reading through a stale pointer.
        group->set_config(config);
        group->reload_config();
    }
    m_loading = false;
    update_status();
}

void SupportsView::update_status()
{
    if (m_status == nullptr)
        return;

    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    size_t prepared_triangles = 0;
    bool   prepared = false;
    size_t printable_objects = 0;
    size_t manual_points = 0;
    size_t drain_holes = 0;
    for (const ModelObject *object : plater->model().objects) {
        if (MSlicerResinJob::is_prepared_support_object(*object)) {
            prepared = true;
            for (const ModelVolume *volume : object->volumes)
                prepared_triangles += volume->mesh().its.indices.size();
        } else if (object->printable) {
            ++printable_objects;
            manual_points += object->sla_support_points.size();
            drain_holes += object->sla_drain_holes.size();
        }
    }

    wxString text;
    if (printable_objects == 0)
        text = _L("There is nothing on the plate to support yet.");
    else if (!prepared)
        text = _L("No supports are prepared. Generate them to add support geometry to the "
                  "plate, or slice as it is to print without supports.");
    else
        // %1% is boost::format syntax; wxString::Format would print it
        // literally. format_wxstr is the wrapper the rest of the GUI uses.
        text = GUI::format_wxstr(
            _L("Supports prepared: %1% triangles, generated from the settings above. Editing "
               "any of them removes this mesh, because it would no longer describe them."),
            prepared_triangles);

    if (manual_points != 0)
        text += "\n" + GUI::format_wxstr(
            _L("%1% manual support points placed; generation supports them too."),
            manual_points);
    if (drain_holes != 0)
        text += "\n" + GUI::format_wxstr(
            _L("%1% drain holes placed; they are drilled when the plate is sliced."),
            drain_holes);
    m_status->SetLabel(text);
    m_status->Wrap(30 * wxGetApp().em_unit());
    if (m_place_points != nullptr)
        m_place_points->Enable(printable_objects > 0);
    if (m_clear_points != nullptr)
        m_clear_points->Enable(manual_points > 0);
    if (m_place_holes != nullptr)
        m_place_holes->Enable(printable_objects > 0);
    if (m_clear_holes != nullptr)
        m_clear_holes->Enable(drain_holes > 0);
    if (m_auto_orient != nullptr)
        m_auto_orient->Enable(printable_objects > 0);
    if (m_generate != nullptr)
        m_generate->Enable(printable_objects > 0);
    if (m_remove != nullptr)
        m_remove->Enable(prepared);
    if (m_settings_panel != nullptr) {
        m_settings_panel->FitInside();
        m_settings_panel->Layout();
    }
}

void SupportsView::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void SupportsView::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void SupportsView::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas != nullptr)
        m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
}

void SupportsView::msw_rescale()
{
    const int em = wxGetApp().em_unit();
    for (auto &group : m_groups)
        group->msw_rescale();
    for (wxButton *button : m_preset_buttons)
        button->SetMinSize(wxSize(7 * em, 2 * em));
    if (m_preset_note != nullptr)
        m_preset_note->Wrap(30 * em);
    // After the groups rescaled: the content width changed with them.
    if (m_settings_panel != nullptr) {
        m_settings_panel->SetMinSize(wxSize(settings_panel_width(), -1));
        // The scroll unit is one em, so it follows the DPI too.
        m_settings_panel->SetScrollRate(0, em);
    }
    update_status();
    Layout();
}

void SupportsView::sys_color_changed()
{
    if (m_settings_panel != nullptr)
        m_settings_panel->SetBackgroundColour(kBlack);
    if (m_heading != nullptr)
        m_heading->SetForegroundColour(kPink);
    if (m_preset_note != nullptr)
        m_preset_note->SetForegroundColour(kDimmed);
    if (m_status != nullptr)
        m_status->SetForegroundColour(kDimmed);
    for (auto &group : m_groups)
        group->sys_color_changed();
    for (wxButton *button : m_preset_buttons)
        wxGetApp().UpdateDarkUI(button);
    if (m_place_points != nullptr) {
        wxGetApp().UpdateDarkUI(m_place_points);
        // A theme change must not quietly strip the armed-mode pink.
        update_place_points_appearance();
    }
    if (m_clear_points != nullptr)
        wxGetApp().UpdateDarkUI(m_clear_points);
    if (m_auto_orient != nullptr)
        wxGetApp().UpdateDarkUI(m_auto_orient);
    if (m_generate != nullptr)
        wxGetApp().UpdateDarkUI(m_generate);
    if (m_remove != nullptr)
        wxGetApp().UpdateDarkUI(m_remove);
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
