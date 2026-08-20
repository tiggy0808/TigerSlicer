#include "TechnologyCapabilities.hpp"

namespace Slic3r {

const TechnologyCapabilities &technology_capabilities(PrinterTechnology technology) noexcept
{
    static constexpr TechnologyCapabilities fdm {
        ptFFF, "fdm", "FDM", SlicingBackend::NativeFdm,
        true, true, true, false, true, false, true, true, true, false
    };
    static constexpr TechnologyCapabilities resin {
        // The workspace is named for its printing technology, like FDM beside
        // it, and matches the MSLA print/material profile labels. "Resin" is
        // the material, not the technology.
        ptSLA, "resin", "MSLA", SlicingBackend::MSlicerRaster,
        true, true, false, true, false, true, false, false, false, true
    };
    static constexpr TechnologyCapabilities unknown {};

    switch (technology) {
    case ptFFF: return fdm;
    case ptSLA: return resin;
    default:    return unknown;
    }
}

bool is_workspace_technology(PrinterTechnology technology) noexcept
{
    return technology_capabilities(technology).slicing_backend != SlicingBackend::None;
}

} // namespace Slic3r
