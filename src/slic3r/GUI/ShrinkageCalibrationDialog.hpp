#ifndef slic3r_GUI_ShrinkageCalibrationDialog_hpp_
#define slic3r_GUI_ShrinkageCalibrationDialog_hpp_

// TigerSlicer: shrinkage calibration (experimental). Adds the test part to the plate, takes its measurements,
// fits scale and offset per axis (libslic3r/ShrinkageCalibration) and writes the shrinkage into the selected
// filament preset as unsaved changes.

class wxWindow;

namespace Slic3r::GUI {

void open_shrinkage_calibration(wxWindow *parent);

} // namespace Slic3r::GUI

#endif // slic3r_GUI_ShrinkageCalibrationDialog_hpp_
