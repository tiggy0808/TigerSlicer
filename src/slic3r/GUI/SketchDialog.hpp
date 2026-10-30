#ifndef slic3r_GUI_SketchDialog_hpp_
#define slic3r_GUI_SketchDialog_hpp_

// TigerSlicer: the Sketch tool. Draw closed shapes on a 2D grid in mm and extrude them into a new object, a part
// of the selected object, or a cut into it. Not parametric: once extruded, the result is an ordinary part, which
// the SVG tool can still move, resize and change the depth of.

#include "GUI_Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Sketch.hpp"

#include <optional>
#include <vector>

class wxBoxSizer;
class wxRadioButton;
class wxSpinCtrlDouble;
class wxStaticText;
class wxToggleButton;
class wxButton;

namespace Slic3r::GUI {

class SketchCanvas;

class SketchDialog : public DPIDialog
{
public:
    enum class Result { NewObject, AddPart, Cut };

    // has_object: an object is selected, so the sketch can be added to it or cut into it.
    // preferred: the result to start with, e.g. Cut when opened from "Negative part".
    SketchDialog(wxWindow *parent, bool has_object, Result preferred);

    const std::vector<SketchShape> &shapes() const;
    double depth() const;
    Result result() const;

    // Called by the canvas whenever the shapes or the selection change.
    void on_sketch_changed();
    void set_status(const wxString &text);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    enum class Field { X, Y, Width, Height, Corner, Diameter, Sides };

    void select_tool(int tool);
    void rebuild_properties();
    void apply_field(Field field, double value);
    void update_buttons();

    SketchCanvas                 *m_canvas{nullptr};
    std::vector<wxToggleButton *> m_tool_buttons;
    wxBoxSizer                   *m_properties{nullptr};
    wxWindow                     *m_properties_panel{nullptr};
    int                           m_properties_for{-2}; // shape index the fields show, -1 for none
    std::vector<std::pair<Field, wxSpinCtrlDouble *>> m_fields;
    wxStaticText                 *m_status{nullptr};
    wxSpinCtrlDouble             *m_depth{nullptr};
    wxRadioButton                *m_new_object{nullptr};
    wxRadioButton                *m_add_part{nullptr};
    wxRadioButton                *m_cut{nullptr};
    wxButton                     *m_extrude{nullptr};
    wxButton                     *m_delete{nullptr};
    wxButton                     *m_undo{nullptr};
};

// Extrudes shapes to the given depth: a new object, or a part of / a cut into the selected object. Writes the
// sketch as an SVG in the data folder's "sketches" folder and hands it to the SVG tool.
bool extrude_sketch(const std::vector<SketchShape> &shapes, double depth, SketchDialog::Result result,
                    const std::optional<Vec2d> &mouse_pos);

// Opens the Sketch tool and extrudes the result. type: MODEL_PART or NEGATIVE_VOLUME when opened for the selected
// object, INVALID to let the person choose. mouse_pos: where on the 3D view the result goes, if known.
void open_sketch_tool(ModelVolumeType type, const std::optional<Vec2d> &mouse_pos);

} // namespace Slic3r::GUI

#endif // slic3r_GUI_SketchDialog_hpp_
