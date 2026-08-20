///|/ TigerSlicer Phase N: drag-and-drop upload guard.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "UploadGuard.hpp"

namespace Slic3r {

DropDecision evaluate_drop(const DroppedArtifact &artifact,
                           const std::string &printer_name,
                           const CompatibilityReport &compatibility,
                           bool printer_supports_remote_start,
                           bool start_explicitly_enabled)
{
    DropDecision decision;
    decision.destination_printer = printer_name;
    decision.compatibility       = compatibility;

    if (printer_name.empty()) {
        decision.outcome = DropOutcome::Refused;
        decision.reason  = "No destination printer was identified for this drop.";
        return decision;
    }

    switch (artifact.kind) {
    case DroppedArtifactKind::Model:
    case DroppedArtifactKind::Project:
        // A model is not a print. Route it back through slicing rather than
        // sending something the printer cannot run.
        decision.outcome = DropOutcome::RouteToSlicing;
        decision.reason  = "This is a model, not a sliced file. Slice it first, then send the result.";
        return decision;

    case DroppedArtifactKind::Unknown:
        decision.outcome = DropOutcome::Refused;
        decision.reason  = "TigerSlicer could not identify this file as a printable artifact.";
        return decision;

    case DroppedArtifactKind::SlicedGCode:
    case DroppedArtifactKind::SlicedResinFile:
        break;
    }

    if (artifact.bytes == 0) {
        decision.outcome = DropOutcome::Refused;
        decision.reason  = "The file is empty.";
        return decision;
    }

    // Trusting a file extension is not identification; an artifact whose
    // contents were never inspected must not be sent to a machine.
    if (!artifact.contents_verified) {
        decision.outcome = DropOutcome::Refused;
        decision.reason  = "The file's contents could not be verified as a printable artifact.";
        return decision;
    }

    if (compatibility.blocked()) {
        decision.outcome = DropOutcome::Refused;
        decision.reason  = "This file is not compatible with the selected printer.";
        return decision;
    }

    // Everything checks out - but the drop itself never sends anything.
    decision.outcome      = DropOutcome::AwaitConfirmation;
    decision.action_label = "Upload only";
    // Starting remotely is offered only where the connector supports it and
    // the user has deliberately enabled it; a drop never starts a print.
    decision.may_offer_start = printer_supports_remote_start && start_explicitly_enabled;
    return decision;
}

} // namespace Slic3r
