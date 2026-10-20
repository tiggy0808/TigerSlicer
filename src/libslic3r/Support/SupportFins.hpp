#pragma once

#include <cstddef>
#include <utility>
#include <vector>
#include "../Polygon.hpp"

namespace Slic3r {

class PrintObject;

// For each object layer, whether its fins get tines: support_fin_tine_spacing apart, with extra rows
// just above the foot when support_fin_tine_base_rows is set. The layers on the bed never get tines.
std::vector<bool> fin_tine_layers(const PrintObject& object);

// Support type "fins": generates breakaway fins for the object and stores them as its support layers.
// See SupportFins.cpp for the geometry. Returns true if some fin is slender enough to fail mid-print.
bool generate_fin_support(PrintObject& object, Polygons* reserved_columns = nullptr);

// Fin holes (TigerSlicer): the stretches of one lattice period, [0, pitch), that are open at height z
// along a fin. The struts, `strut` wide, run along u + z = k * pitch and u - z = k * pitch of the fin's plane.
std::vector<std::pair<double, double>> fin_hole_intervals(double z, double pitch, double strut);

} // namespace Slic3r
