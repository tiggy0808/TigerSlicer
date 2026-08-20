///|/ TigerSlicer Phase N: drag-and-drop upload guard.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_UploadGuard_hpp_
#define slic3r_UploadGuard_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/PlateCompatibility.hpp"

namespace Slic3r {

// What was dropped. A model is not a print job; only an already-sliced
// artifact may be uploaded.
enum class DroppedArtifactKind
{
    Unknown,
    SlicedGCode,
    SlicedResinFile,
    Model,          // STL/3MF/OBJ - must go back through slicing
    Project,        // a project file, likewise
};

struct DroppedArtifact
{
    std::string         path;
    DroppedArtifactKind kind { DroppedArtifactKind::Unknown };
    uint64_t            bytes { 0 };
    std::string         sha256;
    // Whether the file's contents were actually inspected, as opposed to its
    // extension being trusted.
    bool                contents_verified { false };
};

enum class DropOutcome
{
    // Ready to upload once the user confirms.
    AwaitConfirmation,
    // Not a printable artifact; the UI should route it back through slicing.
    RouteToSlicing,
    Refused,
};

struct DropDecision
{
    DropOutcome outcome { DropOutcome::Refused };
    std::string reason;
    // Populated for AwaitConfirmation so the confirmation names the target.
    std::string destination_printer;
    std::string action_label;      // e.g. "Upload only"
    // Starting a print is never implied by a drop.
    bool        may_offer_start { false };
    // Findings the user must see before confirming.
    CompatibilityReport compatibility;
};

// Decide what dropping `artifact` onto `printer` should do.
//
// This never uploads and never starts a print; it produces the decision the UI
// must act on, with the confirmation step always required.
DropDecision evaluate_drop(const DroppedArtifact &artifact,
                           const std::string &printer_name,
                           const CompatibilityReport &compatibility,
                           bool printer_supports_remote_start,
                           bool start_explicitly_enabled);

} // namespace Slic3r

#endif // slic3r_UploadGuard_hpp_
