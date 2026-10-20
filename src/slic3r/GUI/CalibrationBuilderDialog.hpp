///|/ TigerSlicer calibration builder dialog.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_CalibrationBuilderDialog_hpp_
#define slic3r_GUI_CalibrationBuilderDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/CalibrationBuilder.hpp"

class wxStaticText;
class ComboBox;
class TextInput;
class RadioGroup;
class CheckBox;
class Button;

namespace Slic3r { namespace GUI {

// One dialog for every calibration print: what to test, its range, a tower or a grid of copies, the
// model, and a few fixed settings. Setups can be saved as presets (in the app config). It returns
// wxID_OK with a valid setup() for the caller to build.
class CalibrationBuilderDialog : public DPIDialog
{
public:
    explicit CalibrationBuilderDialog(wxWindow *parent);

    const calib_builder::Setup &setup() const { return m_setup; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    calib_builder::Setting selected_setting() const;
    // Reads the controls into a setup; false (with a message shown) when something is not valid.
    bool read(calib_builder::Setup &setup);
    void show(const calib_builder::Setup &setup);
    void show_sweep(calib_builder::Setting setting, double from, double to, double step);
    void on_setting_changed();
    void update_state();
    void reload_presets(const std::string &select = {});
    void on_preset_selected();
    void on_save_preset();
    void on_delete_preset();
    void on_browse();
    void on_generate();

    ComboBox     *m_preset { nullptr };
    Button       *m_save { nullptr };
    Button       *m_delete { nullptr };
    ComboBox     *m_setting { nullptr };
    TextInput    *m_from { nullptr };
    TextInput    *m_to { nullptr };
    TextInput    *m_step { nullptr };
    wxStaticText *m_count { nullptr };
    RadioGroup   *m_layout { nullptr };
    TextInput    *m_band { nullptr };
    wxStaticText *m_layout_hint { nullptr };
    RadioGroup   *m_model { nullptr };
    Button       *m_browse { nullptr };
    wxStaticText *m_model_path { nullptr };
    CheckBox     *m_use_layer { nullptr };
    TextInput    *m_layer { nullptr };
    CheckBox     *m_use_speed { nullptr };
    TextInput    *m_speed { nullptr };
    std::string   m_model_file;
    calib_builder::Setup m_setup;
};

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_CalibrationBuilderDialog_hpp_
