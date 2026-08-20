///|/ TigerSlicer five-axis G-code composition.
///|/
///|/ Each segment of a multidirectional print is sliced upright in its own
///|/ local frame. This turns those planar programs into one machine program:
///|/ every move is mapped into the machine frame and the segments are joined
///|/ by a retract / lift / park / reorient block.
///|/
///|/ It works on G-code text, so it is exercised by tests without slicing.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FiveAxisCompose_hpp_
#define slic3r_FiveAxisCompose_hpp_

#include <string>
#include <vector>

#include "libslic3r/FiveAxis.hpp"

namespace Slic3r {

// One sliced segment on its way into the machine program.
struct FiveAxisSegmentProgram
{
    FiveAxisSegment segment;
    // The segment's G-code, sliced upright in its local frame.
    std::string     gcode;
    // Where the slicer put the part on the bed, and where the segment was
    // moved to be sliced (centred on the plate and seated on it). Local
    // coordinates are recovered by taking both back off, so the segment's
    // own origin lines up with its slice plane.
    Vec2d           bed_center { Vec2d(0., 0.) };
    Vec3d           local_offset { Vec3d::Zero() };
};

struct FiveAxisComposeOptions
{
    // Relative E (M83). Absolute E is rebased at each reorientation.
    bool   relative_e { true };
    double retract_length { 0.7 };   // mm of filament
    int    retract_feedrate { 2400 };    // mm/min
    int    deretract_feedrate { 1500 };  // mm/min
    int    travel_feedrate { 10200 };    // mm/min
};

struct FiveAxisComposeResult
{
    std::string              gcode;
    // Empty when the print can be composed; otherwise why it cannot, ready
    // to show. Nothing is silently dropped or approximated.
    std::string              error;
    std::vector<std::string> warnings;
    size_t                   moves_transformed { 0 };
    double                   max_z { 0. };

    bool ok() const { return error.empty(); }
};

// Compose the segments into one machine program. Segment 0 is expected to
// be the flat one and passes through untransformed.
FiveAxisComposeResult five_axis_compose(
    const std::vector<FiveAxisSegmentProgram> &segments,
    const FiveAxisMachine                     &machine,
    const FiveAxisComposeOptions              &options = {});

} // namespace Slic3r

#endif // slic3r_FiveAxisCompose_hpp_
