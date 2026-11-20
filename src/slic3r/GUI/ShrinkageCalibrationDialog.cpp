#include "ShrinkageCalibrationDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ShrinkageCalibration.hpp"
#include "libslic3r/format.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <array>
#include <cstdio>
#include <sstream>
#include <cmath>
#include <optional>

namespace Slic3r::GUI {

namespace sc = shrinkage_calibration;

namespace {

constexpr const char *kNominalsKey = "shrinkage_calibration_nominals";

// What was typed, kept while the app runs so the window can be closed to print the part.
struct Session
{
    std::array<wxString, size_t(sc::Field::Count)> values;
    int  condition    = -1;
    bool verification = false;
};
Session &session()
{
    static Session s;
    return s;
}

std::string fmt(double v, int digits = 2)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

// The nominals of the last test part added, so the fields match the part on the bed; else the current printer's.
std::optional<sc::Nominals> current_nominals(bool *from_last_part)
{
    if (from_last_part != nullptr)
        *from_last_part = false;
    const std::string stored = wxGetApp().app_config->get(kNominalsKey);
    if (!stored.empty()) {
        std::vector<double> v;
        std::stringstream   ss(stored);
        for (std::string item; std::getline(ss, item, ',');)
            v.push_back(std::atof(item.c_str()));
        if (v.size() == 9) {
            sc::Nominals n;
            for (int i = 0; i < 3; ++i) {
                n.x[i] = v[i];
                n.y[i] = v[3 + i];
                n.z[i] = v[6 + i];
            }
            if (from_last_part != nullptr)
                *from_last_part = true;
            return n;
        }
    }
    const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const BoundingBoxf        bed(printer.option<ConfigOptionPoints>("printable_area")->values);
    return sc::nominals_for_bed(bed, printer.opt_float("printable_height"));
}

void remember_nominals(const sc::Nominals &n)
{
    std::string s;
    for (const auto &axis : {n.x, n.y, n.z})
        for (double v : axis)
            s += (s.empty() ? "" : ",") + fmt(v, 2);
    wxGetApp().app_config->set(kNominalsKey, s);
}

wxString condition_label(int i)
{
    switch (i) {
    case 0: return _L("As printed");
    case 1: return _L("Annealed");
    default: return _L("Moisture conditioned");
    }
}

wxString field_label(sc::Field f, const sc::Nominals &n)
{
    const double nominal = sc::nominal(n, f);
    switch (f) {
    case sc::Field::X1: case sc::Field::X2: case sc::Field::X3: return wxString::Format(_L("X at %s mm"), fmt(nominal, 1));
    case sc::Field::Y1: case sc::Field::Y2: case sc::Field::Y3: return wxString::Format(_L("Y at %s mm"), fmt(nominal, 1));
    case sc::Field::Z1: case sc::Field::Z2: case sc::Field::Z3: return wxString::Format(_L("Z at %s mm"), fmt(nominal, 1));
    case sc::Field::HoleSmall: case sc::Field::HoleLarge: return wxString::Format(_L("Hole of %s mm"), fmt(nominal, 1));
    default: return wxString::Format(_L("Wall of %s mm"), fmt(nominal, 1));
    }
}

class ShrinkageCalibrationDialog : public DPIDialog
{
public:
    explicit ShrinkageCalibrationDialog(wxWindow *parent)
        : DPIDialog(parent, wxID_ANY, _L("Shrinkage calibration (Experimental)"), wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        bool from_last_part = false;
        m_nominals          = current_nominals(&from_last_part);
        const Preset &filament = wxGetApp().preset_bundle->filaments.get_edited_preset();
        const int     gap      = FromDIP(8);
        auto         *main     = new wxBoxSizer(wxVERTICAL);

        auto *intro = new wxStaticText(this, wxID_ANY,
            wxString::Format(_L("Measures how much \"%s\" shrinks, separately from a constant size error such as line width. "
                                "Print the test part, let it cool (and anneal or condition it if you use the part that way), "
                                "measure it with calipers, and enter the measurements below."),
                             from_u8(filament.name)));
        intro->Wrap(FromDIP(560));
        main->Add(intro, 0, wxALL, gap);

        // 1. Print
        auto *print_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("1. Print"));
        auto *buttons   = new wxBoxSizer(wxHORIZONTAL);
        auto *add_test  = new wxButton(print_box->GetStaticBox(), wxID_ANY, _L("Add test part"));
        add_test->SetToolTip(_L("Prints at its true size: shrinkage, X-Y, hole and elephant foot compensation are turned off for it."));
        auto *add_verify = new wxButton(print_box->GetStaticBox(), wxID_ANY, _L("Add verification part"));
        add_verify->SetToolTip(_L("Prints with your current compensation, to measure what is left after applying the result."));
        add_test->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_part(false); });
        add_verify->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_part(true); });
        add_test->Enable(m_nominals.has_value());
        add_verify->Enable(m_nominals.has_value());
        buttons->Add(add_test, 0, wxRIGHT, gap);
        buttons->Add(add_verify);
        print_box->Add(buttons, 0, wxALL, gap / 2);
        auto *how = new wxStaticText(print_box->GetStaticBox(), wxID_ANY,
            !m_nominals ? _L("The bed of this printer is too small for the test part.") :
                          _L("Each arm is a staircase of three tiers. Measure X and Y from the corner end of the arm (0) to the end of "
                             "each tier, holding the calipers on that tier, above the base plate. The engraved X and Y show which arm is "
                             "which. Measure Z from the bottom of the part to the top of each step of the Z staircase, then the two "
                             "holes and the thin wall."));
        how->Wrap(FromDIP(560));
        print_box->Add(how, 0, wxALL, gap / 2);
        main->Add(print_box, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);

        // 2. Measurements
        auto *measure_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("2. Measurements (mm)"));
        if (m_nominals) {
            if (!from_last_part)
                measure_box->Add(new wxStaticText(measure_box->GetStaticBox(), wxID_ANY,
                                                  _L("Sizes for this printer's bed. Add the test part to print it.")), 0, wxALL, gap / 2);
            auto *grid = new wxFlexGridSizer(0, 6, gap / 2, gap);
            for (int i = 0; i < int(sc::Field::Count); ++i) {
                const sc::Field f = sc::Field(i);
                grid->Add(new wxStaticText(measure_box->GetStaticBox(), wxID_ANY, field_label(f, *m_nominals)), 0, wxALIGN_CENTER_VERTICAL);
                auto *ctrl = new wxTextCtrl(measure_box->GetStaticBox(), wxID_ANY, session().values[i], wxDefaultPosition, FromDIP(wxSize(80, -1)));
                ctrl->SetHint(fmt(sc::nominal(*m_nominals, f), 2));
                ctrl->Bind(wxEVT_TEXT, [this, i, ctrl](wxCommandEvent &) {
                    session().values[i] = ctrl->GetValue();
                    ctrl->SetBackgroundColour(wxNullColour);
                    ctrl->Refresh();
                    m_apply->Enable(false);
                });
                m_fields[i] = ctrl;
                grid->Add(ctrl, 0);
            }
            measure_box->Add(grid, 0, wxALL, gap / 2);

            auto *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(measure_box->GetStaticBox(), wxID_ANY, _L("Part condition")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
            m_condition = new wxChoice(measure_box->GetStaticBox(), wxID_ANY);
            m_condition->Append(_L("Choose..."));
            for (int i = 0; i < 3; ++i)
                m_condition->Append(condition_label(i));
            m_condition->SetSelection(session().condition + 1);
            m_condition->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { session().condition = m_condition->GetSelection() - 1; m_apply->Enable(false); });
            row->Add(m_condition, 0, wxRIGHT, 2 * gap);
            m_printed_test   = new wxRadioButton(measure_box->GetStaticBox(), wxID_ANY, _L("Test part"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
            m_printed_verify = new wxRadioButton(measure_box->GetStaticBox(), wxID_ANY, _L("Verification part"));
            (session().verification ? m_printed_verify : m_printed_test)->SetValue(true);
            for (wxRadioButton *rb : {m_printed_test, m_printed_verify}) {
                rb->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &) { session().verification = m_printed_verify->GetValue(); m_apply->Enable(false); });
                row->Add(rb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
            }
            measure_box->Add(row, 0, wxALL, gap / 2);
        }
        main->Add(measure_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

        // 3. Result
        auto *result_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("3. Result"));
        m_result = new wxTextCtrl(result_box->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(560, 220)),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        result_box->Add(m_result, 1, wxEXPAND | wxALL, gap / 2);
        main->Add(result_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

        auto *bottom    = new wxBoxSizer(wxHORIZONTAL);
        auto *calculate_btn = new wxButton(this, wxID_ANY, _L("Calculate"));
        calculate_btn->Enable(m_nominals.has_value());
        calculate_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { calculate(); });
        m_apply = new wxButton(this, wxID_ANY, _L("Apply to filament"));
        m_apply->SetToolTip(_L("Sets Shrinkage (XY) and Shrinkage (Z) of the selected filament and adds this calibration to its notes. "
                               "The filament is marked as modified; save it to keep the values."));
        m_apply->Enable(false);
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { apply(); });
        bottom->Add(calculate_btn, 0, wxRIGHT, gap);
        bottom->Add(m_apply);
        bottom->AddStretchSpacer();
        bottom->Add(new wxButton(this, wxID_CANCEL, _L("Close")));
        main->Add(bottom, 0, wxEXPAND | wxALL, gap);

        SetSizerAndFit(main);
        SetMinSize(GetSize());
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

protected:
    void on_dpi_changed(const wxRect &) override { Layout(); Refresh(); }

private:
    std::optional<sc::Nominals>                            m_nominals;
    std::array<wxTextCtrl *, size_t(sc::Field::Count)>     m_fields{};
    wxChoice                                              *m_condition{nullptr};
    wxRadioButton                                         *m_printed_test{nullptr};
    wxRadioButton                                         *m_printed_verify{nullptr};
    wxTextCtrl                                            *m_result{nullptr};
    wxButton                                              *m_apply{nullptr};
    std::optional<sc::Measurements>                        m_measurements;
    sc::Result                                             m_analysis;

    void add_part(bool verification)
    {
        // The part is sized for the current printer; remember its sizes for the measurement fields.
        const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        const auto nominals = sc::nominals_for_bed(BoundingBoxf(printer.option<ConfigOptionPoints>("printable_area")->values),
                                                   printer.opt_float("printable_height"));
        if (!nominals) {
            show_error(this, _L("The bed of this printer is too small for the test part."));
            return;
        }
        remember_nominals(*nominals);
        session().verification = verification;
        EndModal(wxID_OK);
        wxGetApp().plater()->add_shrinkage_test_part(*nominals, verification);
    }

    void calculate()
    {
        m_apply->Enable(false);
        sc::Measurements m;
        wxString         problems;
        for (int i = 0; i < int(sc::Field::Count); ++i) {
            wxString text = m_fields[i]->GetValue();
            text.Replace(",", ".");
            double v = 0.;
            if (!text.Trim().Trim(false).ToCDouble(&v))
                v = 0.;
            const sc::Field f = sc::Field(i);
            switch (f) {
            case sc::Field::X1: case sc::Field::X2: case sc::Field::X3: m.x[i - int(sc::Field::X1)] = v; break;
            case sc::Field::Y1: case sc::Field::Y2: case sc::Field::Y3: m.y[i - int(sc::Field::Y1)] = v; break;
            case sc::Field::Z1: case sc::Field::Z2: case sc::Field::Z3: m.z[i - int(sc::Field::Z1)] = v; break;
            case sc::Field::HoleSmall: m.hole_small = v; break;
            case sc::Field::HoleLarge: m.hole_large = v; break;
            default: m.wall = v; break;
            }
        }
        for (sc::Field f : sc::out_of_range(*m_nominals, m)) {
            wxTextCtrl *ctrl = m_fields[int(f)];
            ctrl->SetBackgroundColour(wxColour(0xF6, 0xA5, 0xC1));
            ctrl->Refresh();
            const double v = sc::measured(m, f);
            problems += "\n" + (v > 0. ? wxString::Format(_L("%s: %s mm is more than 5 %% from nominal."), field_label(f, *m_nominals), fmt(v)) :
                                         wxString::Format(_L("%s: no measurement."), field_label(f, *m_nominals)));
        }
        if (!problems.empty()) {
            m_result->SetValue(_L("Check your measurements:") + problems);
            return;
        }
        if (m_condition->GetSelection() <= 0) {
            m_result->SetValue(_L("Choose the part condition: as printed, annealed or moisture conditioned."));
            return;
        }
        m_measurements = m;
        m_analysis     = sc::analyse(*m_nominals, m);
        m_result->SetValue(describe());
        m_apply->Enable(true);
    }

    // The filament's current values, and what Apply would set.
    std::pair<double, double> current_percents() const
    {
        const DynamicPrintConfig &f = wxGetApp().preset_bundle->filaments.get_edited_preset().config;
        return {f.option<ConfigOptionPercents>("filament_shrink")->get_at(0), f.option<ConfigOptionPercents>("filament_shrinkage_compensation_z")->get_at(0)};
    }
    std::pair<double, double> new_percents() const
    {
        const bool verification = m_printed_verify->GetValue();
        const auto [xy, z]      = current_percents();
        // Rounded as the settings show them.
        auto round2 = [](double v) { return std::round(v * 100.) / 100.; };
        return {round2(sc::corrected_percent(verification ? xy : 100., m_analysis.xy_percent)),
                round2(sc::corrected_percent(verification ? z : 100., m_analysis.z.percent))};
    }

    wxString describe() const
    {
        const sc::Result &r            = m_analysis;
        const bool        verification = m_printed_verify->GetValue();
        wxString          out;
        auto axis = [&](const wxString &name, const sc::AxisResult &a) {
            out += verification ? wxString::Format(_L("%s: %s %% of nominal, remaining error %+.2f %%, constant offset %+.3f mm, fit residual %.3f mm"),
                                                   name, fmt(a.percent), a.percent - 100., a.fit.intercept, a.fit.max_residual) :
                                  wxString::Format(_L("%s: shrinks to %s %%, constant offset %+.3f mm, fit residual %.3f mm"),
                                                   name, fmt(a.percent), a.fit.intercept, a.fit.max_residual);
            out += "\n";
            if (a.inconsistent)
                out += wxString::Format(_L("  The %s measurements don't lie on a straight line (one is %.2f mm off). Measure them again."), name,
                                        a.fit.max_residual) + "\n";
        };
        axis("X", r.x);
        axis("Y", r.y);
        axis("Z", r.z);
        out += _L("Z uses only the slope: its offset comes mostly from the first layer.") + "\n\n";

        if (r.anisotropic)
            out += wxString::Format(_L("X and Y differ by %.2f percentage points: the material shrinks differently along the two arms, usually "
                                       "along and across the fibres. Shrinkage (XY) is one value, so their mean, %s %%, is used and the "
                                       "difference is discarded. Fibres follow the print paths, so on another part the difference also "
                                       "depends on how it is oriented."),
                                    std::abs(r.anisotropy), fmt(r.xy_percent)) + "\n\n";

        out += _L("Suggested, not applied (process settings, Quality):") + "\n";
        out += wxString::Format(_L("  X-Y contour compensation %+.3f mm, from the constant offset of X and Y (%+.3f mm)."),
                                r.suggested_contour_compensation, r.xy_offset) + "\n";
        out += wxString::Format(_L("  X-Y hole compensation %+.3f mm, from the two holes."), r.suggested_hole_compensation) + "\n";
        out += wxString::Format(_L("  Thin wall: %s mm, %s mm expected from the X and Y fit (%+.3f mm)."), fmt(m_measurements->wall, 3),
                                fmt(r.wall_expected, 3), r.wall_error) + "\n";
        if (r.large_offset)
            out += _L("The constant offset is large. That usually means the flow ratio or line width is off: tune the flow first, "
                      "then repeat this test.") + "\n";

        const auto [cur_xy, cur_z] = current_percents();
        const auto [new_xy, new_z] = new_percents();
        out += "\n" + wxString::Format(_L("Apply sets Shrinkage (XY) from %s %% to %s %% and Shrinkage (Z) from %s %% to %s %%."),
                                       fmt(cur_xy), fmt(new_xy), fmt(cur_z), fmt(new_z));
        return out;
    }

    void apply()
    {
        if (!m_measurements)
            return;
        PresetBundle &bundle   = *wxGetApp().preset_bundle;
        Tab          *tab      = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
        if (tab == nullptr)
            return;
        const auto [new_xy, new_z] = new_percents();
        DynamicPrintConfig config  = bundle.filaments.get_edited_preset().config;
        auto set_percents = [&config](const char *key, double value) {
            auto *opt = config.option<ConfigOptionPercents>(key, true);
            opt->values.assign(std::max<size_t>(1, opt->values.size()), value);
        };
        set_percents("filament_shrink", new_xy);
        set_percents("filament_shrinkage_compensation_z", new_z);

        // The calibration, kept in the filament's notes.
        const sc::Measurements &m = *m_measurements;
        const sc::Nominals     &n = *m_nominals;
        auto list = [](const std::array<double, 3> &a, int digits) { return fmt(a[0], digits) + "/" + fmt(a[1], digits) + "/" + fmt(a[2], digits); };
        std::string record = format(_u8L("Shrinkage calibration %1% (%2%%3%): XY %4%%% (X %5%%%, Y %6%%%), Z %7%%%; constant offset X %8% mm, "
                                         "Y %9% mm. Measured X %10%, Y %11%, Z %12% for %13%, %14%, %15% mm; holes %16%/%17% for %18%/%19% mm; "
                                         "wall %20% for %21% mm."),
                                    into_u8(wxDateTime::Now().FormatISODate()), into_u8(condition_label(m_condition->GetSelection() - 1)),
                                    m_printed_verify->GetValue() ? ", " + _u8L("verification") : std::string(), fmt(new_xy), fmt(m_analysis.x.percent),
                                    fmt(m_analysis.y.percent), fmt(new_z), fmt(m_analysis.x.fit.intercept, 3), fmt(m_analysis.y.fit.intercept, 3),
                                    list(m.x, 2), list(m.y, 2), list(m.z, 2), list(n.x, 1), list(n.y, 1), list(n.z, 1), fmt(m.hole_small),
                                    fmt(m.hole_large), fmt(n.hole_small, 1), fmt(n.hole_large, 1), fmt(m.wall), fmt(n.wall, 1));
        auto *notes = config.option<ConfigOptionStrings>("filament_notes", true);
        if (notes->values.empty())
            notes->values.emplace_back();
        notes->values.front() += (notes->values.front().empty() ? "" : "\n") + record;

        tab->load_config(config);
        wxGetApp().plater()->on_config_change(bundle.full_config());
        m_apply->Enable(false);
        m_result->AppendText("\n\n" + wxString::Format(_L("Applied to \"%s\". The filament is marked as modified: save it to keep the values."),
                                                       from_u8(bundle.filaments.get_edited_preset().name)));
    }
};

} // namespace

void open_shrinkage_calibration(wxWindow *parent)
{
    ShrinkageCalibrationDialog dialog(parent);
    dialog.ShowModal();
}

} // namespace Slic3r::GUI
