// Phase N: drag-and-drop upload guard. Dropping a file onto a printer card is
// the easiest way to accidentally start a print, so these tests pin down that
// a drop never starts anything, never sends an unverified or incompatible
// file, and routes models back through slicing.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/UploadGuard.hpp"

using namespace Slic3r;

namespace {

DroppedArtifact sliced(uint64_t bytes = 4096, bool verified = true)
{
    DroppedArtifact artifact;
    artifact.path              = "/prints/part.gcode";
    artifact.kind              = DroppedArtifactKind::SlicedGCode;
    artifact.bytes             = bytes;
    artifact.sha256            = "abc123";
    artifact.contents_verified = verified;
    return artifact;
}

CompatibilityReport ok_report()
{
    return CompatibilityReport{};
}

CompatibilityReport blocked_report()
{
    CompatibilityReport report;
    report.state = CompatibilityState::Blocked;
    report.findings.push_back({ CompatibilityState::Blocked, "Build volume", "Too large." });
    return report;
}

} // namespace

TEST_CASE("A valid sliced file awaits confirmation rather than uploading", "[upload-guard]")
{
    const DropDecision decision =
        evaluate_drop(sliced(), "printer1", ok_report(), false, false);

    CHECK(decision.outcome == DropOutcome::AwaitConfirmation);
    CHECK(decision.destination_printer == "printer1");
    CHECK(decision.action_label == "Upload only");
}

TEST_CASE("A drop never offers to start a print by default", "[upload-guard]")
{
    // Even on a printer that can start remotely, starting stays off unless the
    // user has deliberately enabled it.
    CHECK_FALSE(evaluate_drop(sliced(), "printer1", ok_report(), true, false).may_offer_start);
    CHECK_FALSE(evaluate_drop(sliced(), "printer1", ok_report(), false, true).may_offer_start);
    // Both conditions together are required.
    CHECK(evaluate_drop(sliced(), "printer1", ok_report(), true, true).may_offer_start);
}

TEST_CASE("Models and projects are routed back through slicing", "[upload-guard]")
{
    DroppedArtifact model = sliced();
    model.kind = DroppedArtifactKind::Model;
    const DropDecision model_decision = evaluate_drop(model, "printer1", ok_report(), true, true);
    CHECK(model_decision.outcome == DropOutcome::RouteToSlicing);
    CHECK_FALSE(model_decision.reason.empty());

    DroppedArtifact project = sliced();
    project.kind = DroppedArtifactKind::Project;
    CHECK(evaluate_drop(project, "printer1", ok_report(), true, true).outcome ==
          DropOutcome::RouteToSlicing);
}

TEST_CASE("An unidentified file is refused", "[upload-guard]")
{
    DroppedArtifact unknown = sliced();
    unknown.kind = DroppedArtifactKind::Unknown;

    CHECK(evaluate_drop(unknown, "printer1", ok_report(), true, true).outcome ==
          DropOutcome::Refused);
}

TEST_CASE("A file whose contents were never verified is refused", "[upload-guard]")
{
    // Trusting the extension alone is not identification.
    const DropDecision decision =
        evaluate_drop(sliced(4096, false), "printer1", ok_report(), true, true);

    CHECK(decision.outcome == DropOutcome::Refused);
    CHECK_FALSE(decision.reason.empty());
}

TEST_CASE("An empty file is refused", "[upload-guard]")
{
    CHECK(evaluate_drop(sliced(0), "printer1", ok_report(), true, true).outcome ==
          DropOutcome::Refused);
}

TEST_CASE("A blocked compatibility report prevents upload", "[upload-guard]")
{
    const DropDecision decision =
        evaluate_drop(sliced(), "printer1", blocked_report(), true, true);

    CHECK(decision.outcome == DropOutcome::Refused);
    // The findings travel with the decision so the user learns why.
    CHECK(decision.compatibility.blocked());
    CHECK_FALSE(decision.compatibility.findings.empty());
}

TEST_CASE("A drop with no destination is refused", "[upload-guard]")
{
    CHECK(evaluate_drop(sliced(), "", ok_report(), true, true).outcome == DropOutcome::Refused);
}

TEST_CASE("Compatibility warnings still reach the confirmation step", "[upload-guard]")
{
    CompatibilityReport warned;
    warned.state = CompatibilityState::Warning;
    warned.findings.push_back({ CompatibilityState::Warning, "Materials", "PETG vs PLA." });

    const DropDecision decision = evaluate_drop(sliced(), "printer1", warned, false, false);
    CHECK(decision.outcome == DropOutcome::AwaitConfirmation);
    // The user must be able to see the warning before agreeing.
    CHECK(decision.compatibility.state == CompatibilityState::Warning);
}
