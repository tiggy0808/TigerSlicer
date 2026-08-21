///|/ TigerSlicer MSLA Supports workspace.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_SupportsView_hpp_
#define slic3r_GUI_SupportsView_hpp_

#include <memory>
#include <string>
#include <vector>

#include <boost/any.hpp>

#include <wx/panel.h>

#include "libslic3r/Point.hpp"

class wxBoxSizer;
class wxButton;
class wxGLCanvas;
class wxScrolledWindow;
class wxStaticText;
class wxToggleButton;

namespace Slic3r {

class BackgroundSlicingProcess;
class DynamicPrintConfig;
class Model;

namespace GUI {

class Bed3D;
class GLCanvas3D;
class ConfigOptionsGroup;

// The MSLA support workspace: the third plater view, between the 3D editor and
// Preview. Every automatic support setting lives here, and supports are
// generated, regenerated and removed from here.
//
// The settings are presented through ConfigOptionsGroups bound to the edited
// SLA print preset, exactly as the sidebar's frequently-changed parameters
// are. There is therefore one copy of each value - the preset's - and the
// MSLA print settings page and this panel cannot disagree about it.
class SupportsView : public wxPanel
{
public:
    SupportsView(wxWindow *parent, Bed3D &bed, Model *model, DynamicPrintConfig *config,
                 BackgroundSlicingProcess *process);
    ~SupportsView() override;

    wxGLCanvas *get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D *get_canvas3d() { return m_canvas; }
    // The settings column; parented to the plater so it can occupy the
    // sidebar's slot at full height while this view is active.
    wxWindow *get_settings_panel() { return m_settings_panel; }

    void set_as_dirty();
    void bed_shape_changed();
    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);

    // Re-read every field from the edited preset. Called when the view is
    // shown and whenever the active preset changes, so the panel never shows a
    // value the preset no longer holds.
    void reload_settings();
    // Refresh the "what is prepared" summary from the current model.
    void update_status();

    void msw_rescale();
    void sys_color_changed();

private:
    void build_settings_panel(wxWindow *parent);
    // Width the settings panel needs: at least the nominal 34 em, wider when
    // the option rows are wider, since the panel scrolls only vertically.
    int settings_panel_width() const;
    // Write one option into the edited preset through the SLA print tab, so
    // the preset's dirty state and the settings page follow, then apply the
    // shared support policy for that key.
    void on_option_changed(const std::string &opt_key, const boost::any &value);
    // Pink while placement mode is armed, ordinary while it is not.
    void update_place_points_appearance();
    void apply_preset(const char *name);
    // Manual support point placement: a click in placement mode adds a point
    // on the model surface under the cursor / removes the nearest one.
    void on_place_point(const Vec2d &pos);
    void on_delete_point(const Vec2d &pos);
    void clear_manual_points();

    wxGLCanvas       *m_canvas_widget { nullptr };
    GLCanvas3D       *m_canvas { nullptr };
    wxScrolledWindow *m_settings_panel { nullptr };
    wxStaticText     *m_status { nullptr };
    wxStaticText     *m_heading { nullptr };
    wxStaticText     *m_preset_note { nullptr };
    wxToggleButton   *m_place_points { nullptr };
    wxButton         *m_clear_points { nullptr };
    wxToggleButton   *m_place_holes { nullptr };
    wxButton         *m_clear_holes { nullptr };
    wxButton         *m_auto_orient { nullptr };
    wxButton         *m_generate { nullptr };
    wxButton         *m_remove { nullptr };
    std::vector<wxButton *> m_preset_buttons;
    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_groups;
    // Guards the write-back while reload_settings() is populating fields.
    bool m_loading { false };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_SupportsView_hpp_
