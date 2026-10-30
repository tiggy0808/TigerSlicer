#include "SketchDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Gizmos/GLGizmoSVG.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

namespace {

enum Tool { ToolSelect, ToolRectangle, ToolCircle, ToolPolygon, ToolOutline, ToolCount };

constexpr double kGridMm       = 1.;   // snapping step
constexpr double kMajorGridMm  = 10.;
constexpr double kCloseRadiusPx = 8.;  // clicking this close to an outline's first point closes it
constexpr int    kDefaultSides  = 6;

const wxColour kPink(0xF6, 0xA5, 0xC1);
const wxColour kPinkDark(0xD9, 0x6B, 0x93);

wxString mm(double value) { return wxString::Format("%.1f mm", value); }

} // namespace

// The drawing area: a grid in mm with the y axis up, zoom with the wheel, pan by dragging with the right or middle
// button (or the left button with Space held).
class SketchCanvas : public wxPanel
{
public:
    SketchCanvas(wxWindow *parent, SketchDialog *dialog) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS), m_dialog(dialog)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(560, 440)));
        Bind(wxEVT_PAINT, &SketchCanvas::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &e) { if (!m_view_set) fit_view(); Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, &SketchCanvas::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &SketchCanvas::on_left_up, this);
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &) { finish_outline(); });
        Bind(wxEVT_MOTION, &SketchCanvas::on_motion, this);
        Bind(wxEVT_MOUSEWHEEL, &SketchCanvas::on_wheel, this);
        Bind(wxEVT_RIGHT_DOWN, &SketchCanvas::on_pan_down, this);
        Bind(wxEVT_MIDDLE_DOWN, &SketchCanvas::on_pan_down, this);
        Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent &) { end_pan(); });
        Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent &) { end_pan(); });
        Bind(wxEVT_KEY_DOWN, &SketchCanvas::on_key, this);
        Bind(wxEVT_KEY_UP, [this](wxKeyEvent &e) { if (e.GetKeyCode() == WXK_SPACE) m_space = false; e.Skip(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { m_panning = false; m_dragging = false; });
    }

    std::vector<SketchShape> shapes;
    int                      selected = -1;
    int                      sides    = kDefaultSides;
    bool                     snap     = true;

    void set_tool(int tool)
    {
        cancel_drawing();
        m_tool = tool;
        SetCursor(tool == ToolSelect ? wxNullCursor : wxCursor(wxCURSOR_CROSS));
        SetFocus();
        Refresh();
    }

    void remember() // before every change, for undo
    {
        m_undo.push_back(shapes);
        if (m_undo.size() > 100)
            m_undo.erase(m_undo.begin());
    }
    bool can_undo() const { return !m_undo.empty(); }
    void undo()
    {
        if (m_undo.empty())
            return;
        shapes = m_undo.back();
        m_undo.pop_back();
        selected = std::min<int>(selected, int(shapes.size()) - 1);
        changed();
    }
    void delete_selected()
    {
        if (selected < 0 || selected >= int(shapes.size()))
            return;
        remember();
        shapes.erase(shapes.begin() + selected);
        selected = -1;
        changed();
    }
    void changed()
    {
        Refresh();
        m_dialog->on_sketch_changed();
    }

private:
    SketchDialog *m_dialog;
    int           m_tool = ToolRectangle;
    // View: screen = m_origin + (x, -y) * m_scale
    double  m_scale = 4.;
    wxPoint2DDouble m_origin{0., 0.};
    bool    m_view_set = false;
    // Drawing
    bool               m_dragging = false; // a rectangle, circle or polygon, or moving a shape
    Vec2d              m_start = Vec2d::Zero(), m_now = Vec2d::Zero();
    Vec2d              m_cursor = Vec2d::Zero();
    std::vector<Vec2d> m_outline; // points of the outline being drawn
    bool               m_moved_shape = false;
    // Panning
    bool    m_panning = false, m_space = false;
    wxPoint m_pan_from;
    std::vector<std::vector<SketchShape>> m_undo;

    wxPoint2DDouble to_screen(const Vec2d &p) const { return {m_origin.m_x + p.x() * m_scale, m_origin.m_y - p.y() * m_scale}; }
    Vec2d to_world(const wxPoint &p) const { return {(p.x - m_origin.m_x) / m_scale, (m_origin.m_y - p.y) / m_scale}; }
    Vec2d snapped(const Vec2d &p) const
    {
        if (!snap)
            return p;
        // Snap to the outline's first point to close it, then to the grid.
        if (!m_outline.empty() && (to_screen_vec(m_outline.front()) - to_screen_vec(p)).norm() < kCloseRadiusPx * GetContentScaleFactor())
            return m_outline.front();
        return {std::round(p.x() / kGridMm) * kGridMm, std::round(p.y() / kGridMm) * kGridMm};
    }
    Vec2d to_screen_vec(const Vec2d &p) const { const auto s = to_screen(p); return {s.m_x, s.m_y}; }

    void fit_view()
    {
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;
        // About 100 mm across, origin in the middle.
        m_scale  = std::max(1., std::min(size.x, size.y) / 110.);
        m_origin = {size.x / 2., size.y / 2.};
    }

    SketchShape preview_shape() const
    {
        switch (m_tool) {
        case ToolRectangle: return SketchShape::rectangle(m_start, m_now);
        case ToolCircle: return SketchShape::circle(m_start, (m_now - m_start).norm());
        case ToolPolygon: return SketchShape::polygon(m_start, (m_now - m_start).norm(), sides);
        default: return {};
        }
    }

    wxString describe(const SketchShape &shape) const
    {
        const auto [lo, hi] = shape.bounds();
        switch (shape.kind) {
        case SketchShape::Kind::Rectangle: return wxString::Format("%.1f × %.1f mm", hi.x() - lo.x(), hi.y() - lo.y());
        case SketchShape::Kind::Circle:    return wxString::Format("Ø %.1f mm", 2. * shape.radius());
        case SketchShape::Kind::Polygon:   return wxString::Format("%d sides, Ø %.1f mm", shape.sides, 2. * shape.radius());
        default:                           return wxString::Format("%.1f × %.1f mm", hi.x() - lo.x(), hi.y() - lo.y());
        }
    }

    void status()
    {
        wxString text = wxString::Format("X %.1f  Y %.1f", m_cursor.x(), m_cursor.y());
        if (m_dragging && m_tool != ToolSelect)
            text += "     " + describe(preview_shape());
        else if (!m_outline.empty())
            text += wxString::Format("     %s", _L("Click the first point, double-click or press Enter to close the outline"));
        else if (selected >= 0 && selected < int(shapes.size()))
            text += "     " + describe(shapes[selected]);
        m_dialog->set_status(text);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const bool dark = wxGetApp().dark_mode();
        dc.SetBackground(wxBrush(dark ? wxColour(0x2B, 0x2B, 0x2E) : *wxWHITE));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize size = GetClientSize();

        // Grid: every mm when there is room, every 10 mm darker, axes darker still.
        const Vec2d lo = to_world({0, size.y}), hi = to_world({size.x, 0});
        auto grid = [&](double step, const wxColour &colour) {
            gc->SetPen(wxPen(colour, 1));
            for (double x = std::floor(lo.x() / step) * step; x <= hi.x(); x += step) {
                const auto a = to_screen({x, 0});
                gc->StrokeLine(a.m_x, 0, a.m_x, size.y);
            }
            for (double y = std::floor(lo.y() / step) * step; y <= hi.y(); y += step) {
                const auto a = to_screen({0, y});
                gc->StrokeLine(0, a.m_y, size.x, a.m_y);
            }
        };
        if (m_scale * kGridMm >= 6.)
            grid(kGridMm, dark ? wxColour(0x36, 0x36, 0x3A) : wxColour(0xF0, 0xF0, 0xF0));
        grid(kMajorGridMm, dark ? wxColour(0x45, 0x45, 0x4A) : wxColour(0xDC, 0xDC, 0xDC));
        const auto o = to_screen({0, 0});
        gc->SetPen(wxPen(dark ? wxColour(0x70, 0x70, 0x76) : wxColour(0xB0, 0xB0, 0xB0), 1));
        gc->StrokeLine(o.m_x, 0, o.m_x, size.y);
        gc->StrokeLine(0, o.m_y, size.x, o.m_y);

        // The area the shapes enclose, holes left open (even-odd), as it will be extruded.
        wxGraphicsPath area = gc->CreatePath();
        for (const SketchShape &shape : shapes) {
            const std::vector<Vec2d> pts = shape.contour(0.5 / m_scale);
            if (pts.empty())
                continue;
            area.MoveToPoint(to_screen(pts.front()));
            for (size_t i = 1; i < pts.size(); ++i)
                area.AddLineToPoint(to_screen(pts[i]));
            area.CloseSubpath();
        }
        gc->SetBrush(wxBrush(wxColour(kPink.Red(), kPink.Green(), kPink.Blue(), 110)));
        gc->FillPath(area, wxODDEVEN_RULE);

        auto stroke = [&](const SketchShape &shape, const wxPen &pen) {
            const std::vector<Vec2d> pts = shape.contour(0.5 / m_scale);
            if (pts.empty())
                return;
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(to_screen(pts.front()));
            for (size_t i = 1; i < pts.size(); ++i)
                path.AddLineToPoint(to_screen(pts[i]));
            path.CloseSubpath();
            gc->SetPen(pen);
            gc->StrokePath(path);
        };
        for (size_t i = 0; i < shapes.size(); ++i)
            stroke(shapes[i], int(i) == selected ? wxPen(kPinkDark, 3) : wxPen(dark ? wxColour(0xC8, 0xC8, 0xCC) : wxColour(0x50, 0x50, 0x55), 2));

        // What is being drawn.
        wxPen draft(kPinkDark, 2, wxPENSTYLE_SHORT_DASH);
        if (m_dragging && m_tool != ToolSelect)
            stroke(preview_shape(), draft);
        if (!m_outline.empty()) {
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(to_screen(m_outline.front()));
            for (size_t i = 1; i < m_outline.size(); ++i)
                path.AddLineToPoint(to_screen(m_outline[i]));
            path.AddLineToPoint(to_screen(m_cursor));
            gc->SetPen(draft);
            gc->StrokePath(path);
            const auto first = to_screen(m_outline.front());
            const double r = kCloseRadiusPx * GetContentScaleFactor() / 2.;
            gc->SetBrush(wxBrush(kPinkDark));
            gc->DrawEllipse(first.m_x - r, first.m_y - r, 2 * r, 2 * r);
        }
    }

    void on_left_down(wxMouseEvent &e)
    {
        SetFocus();
        if (m_space) {
            on_pan_down(e);
            return;
        }
        const Vec2d p = snapped(to_world(e.GetPosition()));
        m_cursor = p;
        if (m_tool == ToolSelect) {
            selected = -1;
            for (int i = int(shapes.size()) - 1; i >= 0; --i)
                if (shapes[i].contains(to_world(e.GetPosition()))) {
                    selected = i;
                    break;
                }
            m_dragging    = selected >= 0;
            m_moved_shape = false;
            m_start = m_now = p;
            if (m_dragging)
                CaptureMouse();
            changed();
            return;
        }
        if (m_tool == ToolOutline) {
            if (m_outline.size() >= 3 && p == m_outline.front()) {
                finish_outline();
                return;
            }
            if (m_outline.empty() || p != m_outline.back())
                m_outline.push_back(p);
            Refresh();
            status();
            return;
        }
        m_dragging = true;
        m_start = m_now = p;
        CaptureMouse();
    }

    void on_left_up(wxMouseEvent &e)
    {
        if (m_panning) {
            end_pan();
            return;
        }
        if (!m_dragging)
            return;
        m_dragging = false;
        if (HasCapture())
            ReleaseMouse();
        if (m_tool == ToolSelect) {
            if (m_moved_shape)
                changed();
            return;
        }
        m_now = snapped(to_world(e.GetPosition()));
        SketchShape shape = preview_shape();
        if (shape.contour().empty()) {
            Refresh();
            return;
        }
        remember();
        shapes.push_back(shape);
        selected = int(shapes.size()) - 1;
        changed();
    }

    void on_motion(wxMouseEvent &e)
    {
        if (m_panning) {
            const wxPoint d = e.GetPosition() - m_pan_from;
            m_pan_from = e.GetPosition();
            m_origin.m_x += d.x;
            m_origin.m_y += d.y;
            m_view_set = true;
            Refresh();
            return;
        }
        m_cursor = snapped(to_world(e.GetPosition()));
        if (m_dragging) {
            if (m_tool == ToolSelect && selected >= 0) {
                const Vec2d offset = m_cursor - m_now;
                if (offset != Vec2d::Zero()) {
                    if (!m_moved_shape)
                        remember();
                    m_moved_shape = true;
                    shapes[selected].translate(offset);
                }
            }
            m_now = m_cursor;
        }
        Refresh();
        status();
    }

    void on_wheel(wxMouseEvent &e)
    {
        const double factor = std::pow(1.15, e.GetWheelRotation() / std::max(1., double(e.GetWheelDelta())));
        const double scale  = std::clamp(m_scale * factor, 0.2, 200.);
        const Vec2d  anchor = to_world(e.GetPosition());
        m_scale  = scale;
        m_origin = {e.GetPosition().x - anchor.x() * m_scale, e.GetPosition().y + anchor.y() * m_scale};
        m_view_set = true;
        Refresh();
    }

    void on_pan_down(wxMouseEvent &e)
    {
        SetFocus();
        m_panning  = true;
        m_pan_from = e.GetPosition();
        if (!HasCapture())
            CaptureMouse();
    }
    void end_pan()
    {
        m_panning = false;
        if (HasCapture())
            ReleaseMouse();
    }

    void on_key(wxKeyEvent &e)
    {
        switch (e.GetKeyCode()) {
        case WXK_SPACE: m_space = true; break;
        case WXK_ESCAPE:
            if (!m_outline.empty() || m_dragging)
                cancel_drawing();
            else
                e.Skip(); // closes the dialog
            break;
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER: finish_outline(); break;
        case WXK_DELETE:
        case WXK_BACK: delete_selected(); break;
        default:
            if ((e.GetKeyCode() == 'Z') && e.CmdDown())
                undo();
            else
                e.Skip();
        }
    }

    void finish_outline()
    {
        if (m_tool != ToolOutline || m_outline.size() < 3) {
            return;
        }
        SketchShape shape = SketchShape::outline(m_outline);
        m_outline.clear();
        if (!shape.contour().empty()) {
            remember();
            shapes.push_back(shape);
            selected = int(shapes.size()) - 1;
        }
        changed();
    }

    void cancel_drawing()
    {
        m_outline.clear();
        if (m_dragging && HasCapture())
            ReleaseMouse();
        m_dragging = false;
        Refresh();
    }
};

SketchDialog::SketchDialog(wxWindow *parent, bool has_object, Result preferred)
    : DPIDialog(parent, wxID_ANY, _L("Sketch"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto *main = new wxBoxSizer(wxVERTICAL);
    auto *body = new wxBoxSizer(wxHORIZONTAL);

    // Tools
    auto *tools = new wxBoxSizer(wxVERTICAL);
    const wxString tool_names[ToolCount] = {_L("Select"), _L("Rectangle"), _L("Circle"), _L("Polygon"), _L("Outline")};
    const wxString tool_tips[ToolCount]  = {
        _L("Click a shape to select it, drag to move it. Delete removes it."),
        _L("Drag from one corner to the opposite one."),
        _L("Drag from the centre outwards."),
        _L("Drag from the centre outwards. Set the number of sides on the right."),
        _L("Click each corner. Click the first point again, double-click or press Enter to close it."),
    };
    for (int i = 0; i < ToolCount; ++i) {
        auto *btn = new wxToggleButton(this, wxID_ANY, tool_names[i]);
        btn->SetToolTip(tool_tips[i]);
        btn->Bind(wxEVT_TOGGLEBUTTON, [this, i](wxCommandEvent &) { select_tool(i); });
        tools->Add(btn, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        m_tool_buttons.push_back(btn);
    }
    tools->AddSpacer(FromDIP(8));
    m_undo = new wxButton(this, wxID_ANY, _L("Undo"));
    m_undo->SetToolTip(_L("Undo the last change (Cmd/Ctrl+Z)"));
    m_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_canvas->undo(); });
    tools->Add(m_undo, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    m_delete = new wxButton(this, wxID_ANY, _L("Delete"));
    m_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_canvas->delete_selected(); });
    tools->Add(m_delete, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    auto *snap = new wxToggleButton(this, wxID_ANY, _L("Snap to 1 mm"));
    snap->SetValue(true);
    snap->Bind(wxEVT_TOGGLEBUTTON, [this, snap](wxCommandEvent &) { m_canvas->snap = snap->GetValue(); });
    tools->Add(snap, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    body->Add(tools, 0, wxALL, FromDIP(8));

    // Canvas
    m_canvas = new SketchCanvas(this, this);
    body->Add(m_canvas, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

    // Selected shape
    auto *side = new wxBoxSizer(wxVERTICAL);
    auto *props_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Selected shape"));
    m_properties_panel = props_box->GetStaticBox();
    m_properties = new wxBoxSizer(wxVERTICAL);
    props_box->Add(m_properties, 0, wxEXPAND | wxALL, FromDIP(4));
    side->Add(props_box, 0, wxEXPAND);
    auto *hint = new wxStaticText(this, wxID_ANY,
                                  _L("A shape inside another one cuts a hole in it.\nScroll to zoom. Drag with the right mouse button to pan."));
    hint->Wrap(FromDIP(200));
    side->Add(hint, 0, wxTOP, FromDIP(8));
    body->Add(side, 0, wxALL, FromDIP(8));
    main->Add(body, 1, wxEXPAND);

    m_status = new wxStaticText(this, wxID_ANY, "");
    main->Add(m_status, 0, wxLEFT | wxRIGHT, FromDIP(8));

    // Extrude
    auto *extrude = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Extrude"));
    extrude->Add(new wxStaticText(extrude->GetStaticBox(), wxID_ANY, _L("Depth")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_depth = new wxSpinCtrlDouble(extrude->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS, 0.1, 1000., 5., 0.5);
    m_depth->SetDigits(2);
    extrude->Add(m_depth, 0, wxALIGN_CENTER_VERTICAL);
    extrude->Add(new wxStaticText(extrude->GetStaticBox(), wxID_ANY, "mm"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    extrude->AddSpacer(FromDIP(16));
    m_new_object = new wxRadioButton(extrude->GetStaticBox(), wxID_ANY, _L("New object"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_add_part   = new wxRadioButton(extrude->GetStaticBox(), wxID_ANY, _L("Add to selected object"));
    m_cut        = new wxRadioButton(extrude->GetStaticBox(), wxID_ANY, _L("Cut into selected object"));
    m_add_part->Enable(has_object);
    m_cut->Enable(has_object);
    if (!has_object)
        preferred = Result::NewObject;
    (preferred == Result::Cut ? m_cut : preferred == Result::AddPart ? m_add_part : m_new_object)->SetValue(true);
    for (wxRadioButton *rb : {m_new_object, m_add_part, m_cut})
        extrude->Add(rb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    main->Add(extrude, 0, wxEXPAND | wxALL, FromDIP(8));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxRIGHT, FromDIP(8));
    m_extrude = new wxButton(this, wxID_OK, _L("Extrude"));
    m_extrude->SetDefault();
    buttons->Add(m_extrude);
    main->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    SetSizerAndFit(main);
    SetMinSize(GetSize());
    CenterOnParent();
    select_tool(ToolRectangle);
    on_sketch_changed();
    wxGetApp().UpdateDlgDarkUI(this);
}

const std::vector<SketchShape> &SketchDialog::shapes() const { return m_canvas->shapes; }
double SketchDialog::depth() const { return m_depth->GetValue(); }
SketchDialog::Result SketchDialog::result() const
{
    return m_cut->GetValue() ? Result::Cut : m_add_part->GetValue() ? Result::AddPart : Result::NewObject;
}

void SketchDialog::set_status(const wxString &text) { m_status->SetLabel(text); }

void SketchDialog::on_dpi_changed(const wxRect &) { Layout(); Refresh(); }

void SketchDialog::select_tool(int tool)
{
    for (int i = 0; i < int(m_tool_buttons.size()); ++i)
        m_tool_buttons[i]->SetValue(i == tool);
    m_canvas->set_tool(tool);
}

void SketchDialog::on_sketch_changed()
{
    rebuild_properties();
    update_buttons();
}

void SketchDialog::update_buttons()
{
    m_extrude->Enable(!sketch_to_expolygons(m_canvas->shapes).empty());
    m_undo->Enable(m_canvas->can_undo());
    m_delete->Enable(m_canvas->selected >= 0);
}

void SketchDialog::rebuild_properties()
{
    const int index = m_canvas->selected;
    if (index >= int(m_canvas->shapes.size()))
        m_canvas->selected = -1;
    const SketchShape *shape = m_canvas->selected >= 0 ? &m_canvas->shapes[m_canvas->selected] : nullptr;

    // Values of the selected shape, in the order the fields are shown.
    std::vector<std::pair<Field, double>> values;
    wxString title;
    if (shape != nullptr) {
        const auto [lo, hi] = shape->bounds();
        switch (shape->kind) {
        case SketchShape::Kind::Rectangle:
            title  = _L("Rectangle");
            values = {{Field::X, lo.x()}, {Field::Y, lo.y()}, {Field::Width, hi.x() - lo.x()}, {Field::Height, hi.y() - lo.y()}, {Field::Corner, shape->corner_radius}};
            break;
        case SketchShape::Kind::Circle:
            title  = _L("Circle");
            values = {{Field::X, shape->a.x()}, {Field::Y, shape->a.y()}, {Field::Diameter, 2. * shape->radius()}};
            break;
        case SketchShape::Kind::Polygon:
            title  = _L("Polygon");
            values = {{Field::X, shape->a.x()}, {Field::Y, shape->a.y()}, {Field::Diameter, 2. * shape->radius()}, {Field::Sides, double(shape->sides)}};
            break;
        case SketchShape::Kind::Outline:
            title  = _L("Outline");
            values = {{Field::X, lo.x()}, {Field::Y, lo.y()}};
            break;
        }
    }

    // Rebuild the fields only when another shape (or kind) is selected, so typing is not interrupted.
    const int key = shape == nullptr ? -1 : m_canvas->selected * 8 + int(shape->kind);
    if (key == m_properties_for && m_fields.size() == values.size()) {
        for (size_t i = 0; i < values.size(); ++i)
            if (!m_fields[i].second->HasFocus())
                m_fields[i].second->SetValue(values[i].second);
        return;
    }
    m_properties_for = key;
    m_fields.clear();
    m_properties->Clear(true);
    wxWindow *parent = m_properties_panel;
    if (shape == nullptr) {
        // Polygon sides for the next polygon drawn.
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(parent, wxID_ANY, _L("Polygon sides")), 1, wxALIGN_CENTER_VERTICAL);
        auto *sides = new wxSpinCtrlDouble(parent, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(80, -1)), wxSP_ARROW_KEYS, 3, 64, m_canvas->sides, 1);
        sides->SetDigits(0);
        sides->Bind(wxEVT_SPINCTRLDOUBLE, [this, sides](wxSpinDoubleEvent &) { m_canvas->sides = int(sides->GetValue()); });
        row->Add(sides, 0);
        m_properties->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        m_properties->Add(new wxStaticText(parent, wxID_ANY, _L("Nothing selected.")), 0);
    } else {
        m_properties->Add(new wxStaticText(parent, wxID_ANY, title), 0, wxBOTTOM, FromDIP(6));
        for (const auto &[field, value] : values) {
            const bool  is_sides = field == Field::Sides;
            const wxString label = field == Field::X        ? _L("X") :
                                   field == Field::Y        ? _L("Y") :
                                   field == Field::Width    ? _L("Width") :
                                   field == Field::Height   ? _L("Height") :
                                   field == Field::Corner   ? _L("Corner radius") :
                                   field == Field::Diameter ? _L("Diameter") : _L("Sides");
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(parent, wxID_ANY, label), 1, wxALIGN_CENTER_VERTICAL);
            const bool may_be_negative = field == Field::X || field == Field::Y;
            auto *ctrl = new wxSpinCtrlDouble(parent, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
                                              is_sides ? 3. : may_be_negative ? -10000. : 0., is_sides ? 64. : 10000., value, is_sides ? 1. : 0.5);
            ctrl->SetDigits(is_sides ? 0 : 2);
            auto apply = [this, field = field, ctrl](auto &) { apply_field(field, ctrl->GetValue()); };
            ctrl->Bind(wxEVT_SPINCTRLDOUBLE, apply);
            ctrl->Bind(wxEVT_TEXT_ENTER, apply);
            row->Add(ctrl, 0);
            if (!is_sides)
                row->Add(new wxStaticText(parent, wxID_ANY, "mm"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
            m_properties->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
            m_fields.emplace_back(field, ctrl);
        }
    }
    Layout();
}

void SketchDialog::apply_field(Field field, double value)
{
    if (m_canvas->selected < 0 || m_canvas->selected >= int(m_canvas->shapes.size()))
        return;
    m_canvas->remember();
    SketchShape &shape = m_canvas->shapes[m_canvas->selected];
    const auto [lo, hi] = shape.bounds();
    switch (field) {
    case Field::X:
        shape.translate({value - (shape.kind == SketchShape::Kind::Circle || shape.kind == SketchShape::Kind::Polygon ? shape.a.x() : lo.x()), 0.});
        break;
    case Field::Y:
        shape.translate({0., value - (shape.kind == SketchShape::Kind::Circle || shape.kind == SketchShape::Kind::Polygon ? shape.a.y() : lo.y())});
        break;
    case Field::Width:  shape.a.x() = lo.x(); shape.b.x() = lo.x() + std::max(0.01, value); break;
    case Field::Height: shape.a.y() = lo.y(); shape.b.y() = lo.y() + std::max(0.01, value); break;
    case Field::Corner: shape.corner_radius = std::max(0., value); break;
    case Field::Diameter: {
        const Vec2d dir = shape.b - shape.a;
        const Vec2d unit = dir.norm() > 0. ? Vec2d(dir.normalized()) : Vec2d(1., 0.);
        shape.b = shape.a + unit * std::max(0.005, value / 2.);
        break;
    }
    case Field::Sides: shape.sides = std::clamp(int(std::lround(value)), 3, 64); break;
    }
    m_canvas->changed();
}

bool extrude_sketch(const std::vector<SketchShape> &shapes, double depth, SketchDialog::Result result,
                    const std::optional<Vec2d> &mouse_pos)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return false;
    const std::string svg = sketch_to_svg(shapes);
    if (svg.empty())
        return false;

    // Keep the file: the SVG tool reads it again to change the part later.
    namespace fs = boost::filesystem;
    const fs::path dir = fs::path(data_dir()) / "sketches";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file;
    for (int i = 1;; ++i) {
        file = dir / ("Sketch " + std::to_string(i) + ".svg");
        if (!fs::exists(file))
            break;
    }
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary);
        out << svg;
        if (!out) {
            show_error(wxGetApp().mainframe, _L("The sketch could not be saved to") + "\n" + from_u8(file.string()));
            return false;
        }
    }

    // The result is placed and edited in the 3D view of Prepare.
    wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    GLCanvas3D *canvas = plater->canvas3D();
    const Selection &selection = canvas->get_selection();
    const bool has_object = !selection.is_empty() && selection.get_object_idx() >= 0;
    if (result == SketchDialog::Result::NewObject && has_object)
        plater->deselect_all(); // a new object, not a part of the selected one
    auto *svg_gizmo = dynamic_cast<GLGizmoSVG *>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg));
    if (svg_gizmo == nullptr)
        return false;
    const ModelVolumeType volume_type = result == SketchDialog::Result::Cut ? ModelVolumeType::NEGATIVE_VOLUME : ModelVolumeType::MODEL_PART;
    // Placing on the clicked spot only makes sense when the person right-clicked where it should go.
    const std::optional<Vec2d> position = result == SketchDialog::Result::NewObject && has_object ? std::nullopt : mouse_pos;
    if (!svg_gizmo->create_volume_from_sketch(file.string(), volume_type, depth, position)) {
        BOOST_LOG_TRIVIAL(error) << "Sketch: the SVG tool could not create a volume from " << file.string();
        return false;
    }
    return true;
}

void open_sketch_tool(ModelVolumeType type, const std::optional<Vec2d> &mouse_pos)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    const Selection &selection = plater->canvas3D()->get_selection();
    const bool has_object = !selection.is_empty() && selection.get_object_idx() >= 0;

    const SketchDialog::Result preferred = type == ModelVolumeType::NEGATIVE_VOLUME ? SketchDialog::Result::Cut :
                                           type == ModelVolumeType::MODEL_PART      ? SketchDialog::Result::AddPart :
                                                                                       SketchDialog::Result::NewObject;
    SketchDialog dialog(wxGetApp().mainframe, has_object, preferred);
    if (dialog.ShowModal() == wxID_OK)
        extrude_sketch(dialog.shapes(), dialog.depth(), dialog.result(), mouse_pos);
}

} // namespace Slic3r::GUI
