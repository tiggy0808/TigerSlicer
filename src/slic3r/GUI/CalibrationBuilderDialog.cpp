///|/ TigerSlicer calibration builder dialog.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CalibrationBuilderDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/TextInput.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem.hpp>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

namespace Slic3r { namespace GUI {

using namespace calib_builder;

namespace {

// Saved setups live in this app config section, one JSON value per name.
constexpr const char *kPresetSection = "calibration_builder";

// A value as the user would type it: the setting's decimals, no trailing zeros.
wxString number_text(Setting s, double v) { return wxString::FromUTF8(format_value(s, v)); }

wxString unit_of(Setting s) { return wxString::FromUTF8(info(s).unit); }

bool read_number(TextInput *ti, double &v) { return ti->GetTextCtrl()->GetValue().ToCDouble(&v) || ti->GetTextCtrl()->GetValue().ToDouble(&v); }

// Accepted for the optional layer height, in mm.
constexpr double kMinLayerHeight = 0.01;
constexpr double kMaxLayerHeight = 2.;

// Why the selected printer cannot run a test of `s`, or empty.
std::string printer_problem_now(Setting s) { return printer_problem(s, wxGetApp().preset_bundle->printers.get_edited_preset().config); }

} // namespace

CalibrationBuilderDialog::CalibrationBuilderDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Calibration builder"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const wxSize field = FromDIP(wxSize(110, -1));
    auto        *top   = new wxBoxSizer(wxVERTICAL);

    auto row = [this](wxSizer *into, const wxString &label, std::initializer_list<wxWindow *> controls) {
        auto *s = new wxBoxSizer(wxHORIZONTAL);
        if (!label.empty())
            s->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        for (wxWindow *c : controls)
            s->Add(c, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        into->Add(s, 0, wxTOP, FromDIP(5));
        return s;
    };
    auto group = [this, top](const wxString &title) {
        auto *box = new wxStaticBoxSizer(new LabeledStaticBox(this, title), wxVERTICAL);
        top->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        return box;
    };

    // Presets
    m_preset = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(220, -1)), 0, nullptr, wxCB_READONLY);
    m_save   = new Button(this, _L("Save as..."));
    m_delete = new Button(this, _L("Delete"));
    for (Button *b : { m_save, m_delete })
        b->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto *preset_row = new wxBoxSizer(wxHORIZONTAL);
    preset_row->Add(new wxStaticText(this, wxID_ANY, _L("Preset")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    for (wxWindow *c : std::initializer_list<wxWindow *>{ m_preset, m_save, m_delete })
        preset_row->Add(c, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    top->Add(preset_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    // What to test
    auto *what = group(_L("What to test"));
    m_setting  = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(240, -1)), 0, nullptr, wxCB_READONLY);
    for (const SettingInfo &s : settings())
        m_setting->Append(_(wxString::FromUTF8(s.label)));
    row(what, _L("Setting"), { m_setting });
    m_from = new TextInput(this, "", "", "", wxDefaultPosition, field);
    m_to   = new TextInput(this, "", "", "", wxDefaultPosition, field);
    m_step = new TextInput(this, "", "", "", wxDefaultPosition, field);
    row(what, _L("From"), { m_from, new wxStaticText(this, wxID_ANY, _L("to")), m_to, new wxStaticText(this, wxID_ANY, _L("step")), m_step });
    m_count = new wxStaticText(this, wxID_ANY, wxEmptyString);
    row(what, wxEmptyString, { m_count });

    // Layout
    auto *layout  = group(_L("Layout"));
    m_layout      = new RadioGroup(this, { _L("Tower"), _L("Grid of copies") }, wxHORIZONTAL);
    row(layout, wxEmptyString, { m_layout });
    m_band = new TextInput(this, "5", _L("mm"), "", wxDefaultPosition, field);
    row(layout, _L("Change the value every"), { m_band });
    m_layout_hint = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_layout_hint->SetForegroundColour(wxColour("#6B6B6B"));
    row(layout, wxEmptyString, { m_layout_hint });

    // Test model
    auto *model  = group(_L("Test model"));
    m_model      = new RadioGroup(this, { _L("Built-in, with the values on it"), _L("My own model") }, wxHORIZONTAL);
    row(model, wxEmptyString, { m_model });
    m_browse     = new Button(this, _L("Choose file..."));
    m_browse->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_model_path = new wxStaticText(this, wxID_ANY, _L("No file chosen"));
    row(model, wxEmptyString, { m_browse, m_model_path });

    // Also set
    auto *also  = group(_L("Also set (optional)"));
    m_use_layer = new CheckBox(this);
    m_layer     = new TextInput(this, "0.2", _L("mm"), "", wxDefaultPosition, field);
    row(also, wxEmptyString, { m_use_layer, new wxStaticText(this, wxID_ANY, _L("Layer height")), m_layer });
    m_use_speed = new CheckBox(this);
    m_speed     = new TextInput(this, "100", _L("mm/s"), "", wxDefaultPosition, field);
    row(also, wxEmptyString, { m_use_speed, new wxStaticText(this, wxID_ANY, _L("Print speed")), m_speed });

    auto *buttons = new DialogButtons(this, { "Generate", "Cancel" }, _L("Generate"));
    top->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(10));
    SetSizer(top);

    // Events
    m_setting->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { on_setting_changed(); });
    for (TextInput *ti : { m_from, m_to, m_step, m_band, m_layer, m_speed })
        ti->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) { update_state(); e.Skip(); });
    m_layout->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [this](wxCommandEvent &) { update_state(); });
    m_model->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [this](wxCommandEvent &) { update_state(); });
    for (CheckBox *cb : { m_use_layer, m_use_speed })
        cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) { update_state(); e.Skip(); });
    m_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_browse(); });
    m_preset->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { on_preset_selected(); });
    m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_save_preset(); });
    m_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_delete_preset(); });
    if (Button *generate = buttons->GetButtonFromLabel(_L("Generate")))
        generate->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_generate(); });

    // Start from the first setting's default sweep.
    Setup start;
    start.setting = Setting::Temperature;
    start.from    = info(start.setting).from;
    start.to      = info(start.setting).to;
    start.step    = info(start.setting).step;
    show(start);
    reload_presets();

    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    Fit();
    top->SetSizeHints(this);
    CenterOnParent();
}

Setting CalibrationBuilderDialog::selected_setting() const
{
    const int i = m_setting->GetSelection();
    return i >= 0 && size_t(i) < settings().size() ? settings()[size_t(i)].setting : Setting::Temperature;
}

void CalibrationBuilderDialog::show_sweep(Setting setting, double from, double to, double step)
{
    for (size_t i = 0; i < settings().size(); ++i)
        if (settings()[i].setting == setting)
            m_setting->SetSelection(int(i));
    for (TextInput *ti : { m_from, m_to, m_step })
        ti->SetLabel(unit_of(setting));
    m_from->GetTextCtrl()->SetValue(number_text(setting, from));
    m_to->GetTextCtrl()->SetValue(number_text(setting, to));
    m_step->GetTextCtrl()->SetValue(number_text(setting, step));
}

void CalibrationBuilderDialog::show(const Setup &setup)
{
    const SettingInfo &s = info(setup.setting);
    show_sweep(setup.setting, setup.from, setup.to, setup.step);
    m_layout->SetSelection(setup.layout == Layout::Grid && s.grid ? 1 : 0);
    m_band->GetTextCtrl()->SetValue(wxString::FromCDouble(setup.band_height, 1));
    m_model_file = setup.model_path;
    m_model->SetSelection(m_model_file.empty() ? 0 : 1);
    m_model_path->SetLabel(m_model_file.empty() ? _L("No file chosen") :
                                                  wxString::FromUTF8(boost::filesystem::path(m_model_file).filename().string()));
    m_use_layer->SetValue(setup.layer_height > 0.);
    if (setup.layer_height > 0.)
        m_layer->GetTextCtrl()->SetValue(wxString::FromCDouble(setup.layer_height, 2));
    m_use_speed->SetValue(setup.print_speed > 0.);
    if (setup.print_speed > 0.)
        m_speed->GetTextCtrl()->SetValue(wxString::FromCDouble(setup.print_speed, 0));
    update_state();
}

void CalibrationBuilderDialog::on_setting_changed()
{
    // A new setting starts from its own default sweep; the layout, model and the rest stay.
    const SettingInfo &s = info(selected_setting());
    show_sweep(s.setting, s.from, s.to, s.step);
    update_state();
}

void CalibrationBuilderDialog::update_state()
{
    const Setting      setting = selected_setting();
    const SettingInfo &s       = info(setting);
    // Settings sent as printer commands cannot differ between copies, so they are always towers.
    if (!s.grid && m_layout->GetSelection() != 0)
        m_layout->SetSelection(0);
    m_layout->SetRadioTooltip(1, s.grid ? wxString() : _L("This setting is changed by the printer while it prints, so it can only be a tower."));
    const bool tower = m_layout->GetSelection() == 0;
    m_band->Enable(tower);
    m_layout_hint->SetLabel(tower ? _L("One model; the value changes as the print gets taller.") :
                                    _L("One copy per value, placed on the plate in order."));
    m_browse->Enable(m_model->GetSelection() == 1);
    m_layer->Enable(m_use_layer->GetValue());
    const bool speed_is_tested = setting == Setting::PrintSpeed;
    m_use_speed->Enable(!speed_is_tested);
    m_speed->Enable(!speed_is_tested && m_use_speed->GetValue());

    double from = 0., to = 0., step = 0.;
    // A test the printer cannot run says so whatever the range.
    std::string error = printer_problem_now(setting);
    std::vector<double> values;
    if (error.empty()) {
        if (read_number(m_from, from) && read_number(m_to, to) && read_number(m_step, step))
            values = sweep(from, to, step, &error);
        else
            error = "Enter numbers for from, to and step.";
    }
    if (values.empty()) {
        m_count->SetForegroundColour(wxColour("#D01B1B"));
        m_count->SetLabel(_(wxString::FromUTF8(error)));
        m_count->Wrap(FromDIP(430)); // a reason can be longer than the dialog is wide
    } else {
        m_count->SetForegroundColour(wxColour("#6B6B6B"));
        wxString text = wxString::Format(_L("%d values: %s to %s"), int(values.size()), wxString::FromUTF8(format_value(setting, values.front())),
                                         wxString::FromUTF8(format_value(setting, values.back())));
        double band = 0.;
        if (tower && read_number(m_band, band) && band > 0.)
            text += wxString::Format(_L(", a %s mm tall tower"), wxString::FromCDouble(band * double(values.size()), 1));
        m_count->SetLabel(text);
    }
    Layout();
}

bool CalibrationBuilderDialog::read(Setup &setup)
{
    auto fail = [this](const wxString &why) {
        MessageDialog(this, why, _L("Calibration builder"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    };
    setup.setting = selected_setting();
    const SettingInfo &s     = info(setup.setting);
    const wxString     label = _(wxString::FromUTF8(s.label));
    if (!read_number(m_from, setup.from) || !read_number(m_to, setup.to) || !read_number(m_step, setup.step))
        return fail(_L("Enter numbers for from, to and step."));
    // The value printed on the model must be the value used.
    if (!fits_decimals(setup.setting, setup.from) || !fits_decimals(setup.setting, setup.to) || !fits_decimals(setup.setting, setup.step))
        return fail(s.decimals == 0 ? wxString::Format(_L("Use whole numbers for %s."), label) :
                                      wxString::Format(_L("Use at most %d decimals for %s."), s.decimals, label));
    std::string error;
    const std::vector<double> values = sweep(setup.from, setup.to, setup.step, &error);
    if (values.empty())
        return fail(_(wxString::FromUTF8(error)));
    for (double v : values)
        if (v < s.min - 1e-9 || v > s.max + 1e-9)
            return fail(wxString::Format(_L("%s must stay between %s and %s."), label, wxString::FromUTF8(format_value(setup.setting, s.min)),
                                         wxString::FromUTF8(format_value(setup.setting, s.max))));
    setup.layout = m_layout->GetSelection() == 1 && s.grid ? Layout::Grid : Layout::Tower;
    // Ranges are checked as "inside", so NaN fails them.
    const bool band_ok = read_number(m_band, setup.band_height) && setup.band_height >= kMinBandHeight && setup.band_height <= kMaxBandHeight;
    if (!band_ok && setup.layout == Layout::Tower)
        return fail(wxString::Format(_L("The value must change every %s to %s mm."), wxString::FromCDouble(kMinBandHeight, 0),
                                     wxString::FromCDouble(kMaxBandHeight, 0)));
    if (!band_ok)
        setup.band_height = Setup().band_height; // a grid has no bands; the preset keeps a valid height
    setup.model_path = m_model->GetSelection() == 1 ? m_model_file : std::string();
    boost::system::error_code ec;
    if (m_model->GetSelection() == 1 && (m_model_file.empty() || !boost::filesystem::exists(m_model_file, ec)))
        return fail(_L("Choose the model file to print, or use the built-in model."));
    setup.layer_height = 0.;
    if (m_use_layer->GetValue() &&
        !(read_number(m_layer, setup.layer_height) && setup.layer_height >= kMinLayerHeight && setup.layer_height <= kMaxLayerHeight))
        return fail(wxString::Format(_L("Enter a layer height from %s to %s mm."), wxString::FromCDouble(kMinLayerHeight, 2),
                                     wxString::FromCDouble(kMaxLayerHeight, 0)));
    setup.print_speed = 0.;
    const SettingInfo &speed = info(Setting::PrintSpeed);
    if (setup.setting != Setting::PrintSpeed && m_use_speed->GetValue() &&
        !(read_number(m_speed, setup.print_speed) && setup.print_speed >= speed.min && setup.print_speed <= speed.max))
        return fail(wxString::Format(_L("Enter a print speed from %s to %s mm/s."), wxString::FromUTF8(format_value(Setting::PrintSpeed, speed.min)),
                                     wxString::FromUTF8(format_value(Setting::PrintSpeed, speed.max))));
    return true;
}

void CalibrationBuilderDialog::reload_presets(const std::string &select)
{
    m_preset->Clear();
    m_preset->Append(_L("(new setup)"));
    AppConfig *config = wxGetApp().app_config;
    int        chosen = 0;
    if (config->has_section(kPresetSection))
        for (const auto &[name, json] : config->get_section(kPresetSection)) {
            const int i = m_preset->Append(wxString::FromUTF8(name));
            if (name == select)
                chosen = i;
        }
    m_preset->SetSelection(chosen);
    m_delete->Enable(chosen > 0);
}

void CalibrationBuilderDialog::on_preset_selected()
{
    const int i = m_preset->GetSelection();
    m_delete->Enable(i > 0);
    if (i <= 0)
        return;
    const std::string name = m_preset->GetString(i).ToUTF8().data();
    Setup setup;
    if (from_json(wxGetApp().app_config->get(kPresetSection, name), setup))
        show(setup);
}

void CalibrationBuilderDialog::on_save_preset()
{
    Setup setup;
    if (!read(setup))
        return;
    const int current = m_preset->GetSelection();
    wxTextEntryDialog ask(this, _L("Name for this setup:"), _L("Save calibration setup"), current > 0 ? m_preset->GetString(current) : wxString());
    if (ask.ShowModal() != wxID_OK)
        return;
    const std::string name = ask.GetValue().Trim().Trim(false).ToUTF8().data();
    if (name.empty())
        return;
    wxGetApp().app_config->set(kPresetSection, name, to_json(setup));
    reload_presets(name);
}

void CalibrationBuilderDialog::on_delete_preset()
{
    const int i = m_preset->GetSelection();
    if (i <= 0)
        return;
    wxGetApp().app_config->erase(kPresetSection, m_preset->GetString(i).ToUTF8().data());
    reload_presets();
}

void CalibrationBuilderDialog::on_browse()
{
    wxFileDialog dlg(this, _L("Choose the model to print"), wxEmptyString, wxEmptyString,
                     "STL, 3MF, OBJ (*.stl;*.3mf;*.obj)|*.stl;*.STL;*.3mf;*.3MF;*.obj;*.OBJ",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    m_model_file = dlg.GetPath().ToUTF8().data();
    m_model_path->SetLabel(dlg.GetFilename());
    m_model->SetSelection(1);
    update_state();
}

void CalibrationBuilderDialog::on_generate()
{
    Setup setup;
    if (!read(setup))
        return;
    if (const std::string problem = printer_problem_now(setup.setting); !problem.empty()) {
        MessageDialog(this, _(wxString::FromUTF8(problem)), _L("Calibration builder"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    m_setup = setup;
    EndModal(wxID_OK);
}

void CalibrationBuilderDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

} } // namespace Slic3r::GUI
