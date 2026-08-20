///|/ TigerSlicer Phase UV6 groundwork: resin calibration generation and its
///|/ safety gating.
///|/
///|/ The decision layer over UVtools' calibration capabilities: which tests are
///|/ offered given a machine's *confirmed* support, their safe starting ranges,
///|/ the provenance a result needs to be reproducible, the disclaimer that
///|/ always travels with a generated calibration, and the rule that a
///|/ calibration project never destructively replaces anything without
///|/ confirmation. Testable libslic3r logic; it generates no test geometry and
///|/ links no UVtools code - that runs behind the UV1 bridge.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ResinCalibration_hpp_
#define slic3r_ResinCalibration_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "PrinterCapabilityRegistry.hpp"

namespace Slic3r {

// The audited resin calibration tests. Each measures one thing; the descriptor
// below says what, and what it needs.
enum class ResinCalibrationKind : uint8_t
{
    Exposure,            // normal-exposure sweep
    Tolerance,           // fit / clearance
    DimensionalAccuracy, // XY / Z scaling
    ElephantFoot,        // first-layer compression
    GrayscaleBlooming,   // anti-alias / light bleed
    Lift,                // lift distance / speed
};

const char *resin_calibration_kind_name(ResinCalibrationKind kind);

// A machine capability a test needs before it can be generated safely. Reuses
// the registry's capability vocabulary conceptually; expressed as strings so
// the gate checks against a CapabilityRegistry's resolved fields by name.
struct CalibrationRequirement
{
    std::string capability_field;   // e.g. "per_layer_exposure", "light_pwm"
    std::string why;
    // True when this is an advanced per-layer setting: it needs confirmed
    // support (vendor spec or hardware), not merely an inference.
    bool advanced { false };
};

// A safe starting range for a swept parameter. These are conservative
// defaults, never a claim that the range is right for a given resin or screen.
struct CalibrationRange
{
    std::string parameter;
    double      minimum { 0. };
    double      maximum { 0. };
    double      step { 0. };
    std::string unit;
};

// What one calibration test is and needs. Static description, independent of
// any machine.
struct ResinCalibrationTest
{
    ResinCalibrationKind kind { ResinCalibrationKind::Exposure };
    std::string          measures;      // what the user learns from it
    std::vector<CalibrationRequirement> requirements;
    std::vector<CalibrationRange>       ranges;
};

// The catalog of audited tests and their descriptors.
const std::vector<ResinCalibrationTest> &resin_calibration_catalog();
const ResinCalibrationTest              *find_calibration_test(ResinCalibrationKind kind);

// --- generating a calibration -----------------------------------------------

enum class CalibrationVerdict : uint8_t
{
    // The test can be generated for this machine.
    Ready,
    // A required capability is not present at all.
    Unsupported,
    // A required advanced capability is present but only weakly evidenced;
    // generating it would gate an advanced per-layer setting on a guess.
    NeedsConfirmedSupport,
};

const char *calibration_verdict_name(CalibrationVerdict verdict);

// Everything needed to reproduce a calibration result later.
struct CalibrationProvenance
{
    std::string test;
    std::string machine_vendor, machine_model, machine_firmware;
    std::string mslicer_version, tigerslicer_version, uvtools_version;
    // The ranges actually used.
    std::vector<CalibrationRange> ranges;
    // Caller-supplied timestamp; nothing here reads a clock.
    int64_t generated_s { 0 };
};

struct ResinCalibrationPlan
{
    CalibrationVerdict verdict { CalibrationVerdict::Unsupported };
    ResinCalibrationKind kind { ResinCalibrationKind::Exposure };

    // The ranges the plan would sweep, defaulted from the descriptor unless the
    // caller overrode them.
    std::vector<CalibrationRange> ranges;

    // Requirements that failed, with why, when the verdict is not Ready.
    std::vector<std::string> unmet;

    // The disclaimer, always present on a Ready plan and never omittable: a
    // generated calibration is not claimed safe for every resin, screen, vat or
    // firmware.
    std::string disclaimer;

    CalibrationProvenance provenance;

    bool ready() const { return verdict == CalibrationVerdict::Ready; }
};

struct ResinCalibrationRequest
{
    ResinCalibrationKind kind { ResinCalibrationKind::Exposure };
    // The machine's reconciled capabilities (from Phase UV7). A requirement is
    // met when the corresponding field resolved; an advanced one needs
    // confirmed evidence.
    CapabilityRegistry machine;

    std::string machine_vendor, machine_model, machine_firmware;
    std::string mslicer_version, tigerslicer_version, uvtools_version;
    int64_t     generated_s { 0 };

    // Optional range overrides, by parameter name. An override outside the safe
    // range is clamped and noted rather than obeyed blindly.
    std::map<std::string, CalibrationRange> range_overrides;
};

// Plan a calibration for a machine. Refuses when a required capability is
// absent, and refuses to gate an advanced per-layer setting on anything weaker
// than confirmed support. A Ready plan always carries the disclaimer and a
// complete provenance record.
ResinCalibrationPlan plan_resin_calibration(const ResinCalibrationRequest &request);

// --- destructive-replacement guard (UV6) ------------------------------------

enum class ProjectAction : uint8_t
{
    // Create a new, separate calibration project. Always safe.
    CreateNew,
    // Replace an existing project. Requires confirmation and must never touch
    // an ordinary user model.
    Replace,
};

const char *project_action_name(ProjectAction action);

struct CalibrationSaveTarget
{
    std::string path;
    bool        exists { false };
    // True when the target is an ordinary user model rather than a prior
    // calibration project.
    bool is_user_model { false };
};

enum class SaveVerdict : uint8_t
{
    // Safe to write without confirmation (a new project).
    Allowed,
    // Would replace an existing calibration project; needs confirmation.
    NeedsConfirmation,
    // Would overwrite a user model; refused outright - a calibration never
    // destroys a user's work, confirmation or not.
    Refused,
};

const char *save_verdict_name(SaveVerdict verdict);

struct CalibrationSaveDecision
{
    SaveVerdict verdict { SaveVerdict::Refused };
    std::string message;

    bool may_write_without_confirmation() const { return verdict == SaveVerdict::Allowed; }
};

// Decide whether a calibration project may be written to a target. A new path
// is Allowed; an existing calibration project NeedsConfirmation; an ordinary
// user model is Refused however the caller confirms - destructive replacement
// of a user model is made impossible, not merely gated.
CalibrationSaveDecision evaluate_calibration_save(const CalibrationSaveTarget &target,
                                                  ProjectAction                intended);

} // namespace Slic3r

#endif // slic3r_ResinCalibration_hpp_
