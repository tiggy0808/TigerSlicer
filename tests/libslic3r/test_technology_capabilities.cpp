#include <catch2/catch_test_macros.hpp>

#include "libslic3r/TechnologyCapabilities.hpp"

using namespace Slic3r;

TEST_CASE("technology workspaces expose disjoint slicing and profile capabilities", "[technology-capabilities]")
{
    const TechnologyCapabilities &fdm = technology_capabilities(ptFFF);
    const TechnologyCapabilities &resin = technology_capabilities(ptSLA);

    REQUIRE(fdm.slicing_backend == SlicingBackend::NativeFdm);
    REQUIRE(fdm.fdm_profiles);
    REQUIRE(fdm.gcode_output);
    REQUIRE_FALSE(fdm.resin_profiles);
    REQUIRE_FALSE(fdm.raster_output);

    // R3 routes final Resin rasters and output through the pinned in-process
    // MSlicer engine while keeping the profile families disjoint.
    REQUIRE(resin.slicing_backend == SlicingBackend::MSlicerRaster);
    REQUIRE(resin.resin_profiles);
    REQUIRE(resin.raster_output);
    REQUIRE_FALSE(resin.fdm_profiles);
    REQUIRE_FALSE(resin.gcode_output);
}

TEST_CASE("unknown technologies have no usable workspace capabilities", "[technology-capabilities]")
{
    for (const PrinterTechnology technology : { ptUnknown, ptAny }) {
        const TechnologyCapabilities &capabilities = technology_capabilities(technology);
        REQUIRE(capabilities.slicing_backend == SlicingBackend::None);
        REQUIRE_FALSE(capabilities.model_editing);
        REQUIRE_FALSE(capabilities.project_persistence);
        REQUIRE_FALSE(is_workspace_technology(technology));
    }
}
