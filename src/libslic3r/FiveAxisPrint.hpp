///|/ TigerSlicer five-axis printing: slicing a multidirectional model.
///|/
///|/ Ties the kinematics and the composition together. Every printable
///|/ object on the plate is a segment: the flat ones print first as sliced,
///|/ and a tilted one is rotated into its own upright frame, sliced there,
///|/ then mapped back into the machine frame by FiveAxisCompose.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FiveAxisPrint_hpp_
#define slic3r_FiveAxisPrint_hpp_

#include <functional>
#include <string>
#include <vector>

#include "libslic3r/FiveAxis.hpp"
#include "libslic3r/FiveAxisCompose.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Model;
class ModelObject;

// Whether a model has anything to print multidirectionally: at least one
// printable object carrying a build direction away from vertical.
bool five_axis_model_has_tilted_segments(const Model &model);

// The build direction an object carries, from its per-object configuration.
FiveAxisSegment five_axis_segment_of(const ModelObject &object);

struct FiveAxisPrintResult
{
    std::string              gcode;
    std::string              error;      // empty when it worked
    std::vector<std::string> warnings;
    size_t                   segments { 0 };
    size_t                   moves_transformed { 0 };
    double                   max_z { 0. };

    bool ok() const { return error.empty(); }
};

// Slice `model` as a multidirectional print and return the machine program.
// `status` receives a short message per segment so a caller can show
// progress; it may be empty.
//
// Refuses rather than approximates: a build direction the machine cannot
// reach, a toolpath that would hit it, or a segment whose G-code cannot be
// rotated all stop the export with a reason.
FiveAxisPrintResult five_axis_slice(const Model                &model,
                                    const DynamicPrintConfig   &config,
                                    std::function<void(const std::string &)> status = {});

} // namespace Slic3r

#endif // slic3r_FiveAxisPrint_hpp_
