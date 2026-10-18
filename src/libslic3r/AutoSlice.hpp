#ifndef slic3r_AutoSlice_hpp_
#define slic3r_AutoSlice_hpp_

// TigerSlicer's Auto slice: settings chosen from what a print is for rather than set by hand.
// The GUI drives the flow (selecting presets, generating supports, slicing); the choices it makes
// live here so they can be tested without it.

#include "libslic3r/Point.hpp"
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class PresetCollection;

namespace auto_slice {

// What a filament print is for. Each purpose picks the process profile whose layer height is
// nearest a fraction of the nozzle diameter: Quality 0.3, Structural 0.5, Fast 0.7.
enum class Purpose { Structural, Fast, Quality };

// How much support a resin print gets.
enum class SupportLevel { Low, Medium, High };

struct ProcessCandidate
{
    std::string name;
    double      layer_height = 0.;
};

// The process profiles Auto slice chooses from: the visible ones compatible with the selected
// printer. Only system profiles when the printer has any, so the choice does not depend on what
// the user saved.
std::vector<ProcessCandidate> process_candidates(const PresetCollection &prints);

// Index into `candidates` of the profile for `purpose`, or -1 when there are none. Structural
// keeps to profiles named "Strength" when the printer has them. A nozzle diameter of 0 or less
// counts as 0.4 mm.
int pick_process(const std::vector<ProcessCandidate> &candidates, double nozzle_diameter, Purpose purpose);

// The SLA print settings a resin support level uses.
struct ResinSupportSettings
{
    double pillar_diameter;     // support_pillar_diameter, mm
    double head_front_diameter; // support_head_front_diameter, mm
    double head_width;          // support_head_width, mm
    int    points_density;      // support_points_density_relative, %
    double contact_penetration; // msla_support_contact_penetration, mm
};

const ResinSupportSettings &resin_support_settings(SupportLevel level);

// Writes the level's settings into `config`, an SLA print configuration.
void apply_resin_support_settings(DynamicPrintConfig &config, SupportLevel level);

// True when hollowing `mesh` with the hollowing settings in `config` (hollowing_min_thickness,
// hollowing_quality, hollowing_closing_distance) leaves an interior. A model too thin for the
// wall has none, and slicing it hollowed would fail.
bool can_hollow(const indexed_triangle_set &mesh, const DynamicPrintConfig &config);

// Up to `count` drain holes for a hollowed object: on its lowest downward-facing surfaces, the
// ones nearest the build plate when it prints, so resin drains out and no suction cup forms;
// spread out so one can vent while the other drains. Positions and directions are in the
// object's mesh coordinates, as ModelObject::sla_drain_holes stores them; `instance` places the
// mesh on the bed. Each hole starts sla::HoleStickOutLength outside the surface and reaches
// `depth` into the model. Empty when the mesh has no downward-facing surface.
sla::DrainHoles place_drain_holes(const indexed_triangle_set &mesh, const Transform3d &instance,
                                  float radius, float depth, size_t count = 2);

} // namespace auto_slice
} // namespace Slic3r

#endif // slic3r_AutoSlice_hpp_
