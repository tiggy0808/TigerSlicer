#ifndef slic3r_TechnologyCapabilities_hpp_
#define slic3r_TechnologyCapabilities_hpp_

#include "Config.hpp"

#include <string_view>

namespace Slic3r {

enum class SlicingBackend {
    None,
    NativeFdm,
    LegacySla,
    MSlicerRaster
};

// One authoritative description of which product surfaces belong to a
// technology. Shared model/plater code should query this object instead of
// growing new ptFFF/ptSLA conditionals.
struct TechnologyCapabilities {
    PrinterTechnology technology { ptUnknown };
    std::string_view workspace_id;
    std::string_view display_name;
    SlicingBackend slicing_backend { SlicingBackend::None };
    bool model_editing { false };
    bool project_persistence { false };
    bool fdm_profiles { false };
    bool resin_profiles { false };
    bool gcode_output { false };
    bool raster_output { false };
    bool multiple_extruders { false };
    bool variable_layer_height { false };
    bool filament_painting { false };
    bool resin_support_editing { false };
};

const TechnologyCapabilities &technology_capabilities(PrinterTechnology technology) noexcept;
bool is_workspace_technology(PrinterTechnology technology) noexcept;

} // namespace Slic3r

#endif
