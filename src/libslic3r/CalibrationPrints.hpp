///|/ TigerSlicer Phase 15: automatic calibration-print generator.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CalibrationPrints_hpp_
#define slic3r_CalibrationPrints_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Confidence and ResultSource-adjacent vocabulary is shared with the
// telemetry importer on purpose: a calibration result fed from telemetry
// carries the same confidence scale as one entered by hand.
#include "libslic3r/TelemetryImport.hpp"

namespace Slic3r {

enum class CalibrationKind : uint8_t
{
    MaximumFlow,
    PressureAdvance,
    Retraction,
    StandbyOoze,
    Priming,
    Temperature,
    XyOffset,
    ZOffset,
    FirstLayer,
    Bridging,
    Cooling,
    Interface,
    ToolchangeReliability,
};

const char *calibration_kind_name(CalibrationKind kind);
bool        parse_calibration_kind(const std::string &text, CalibrationKind *kind);

// True for the calibrations that only mean anything between two tools: an XY
// offset, an interface, toolchange reliability.
bool calibration_kind_is_relative(CalibrationKind kind);

// The unit the swept value is expressed in, so a specimen label is not just a
// bare number.
const char *calibration_kind_unit(CalibrationKind kind);

// What the machine can actually do. A calibration that needs a capability the
// printer never declared is refused rather than generated and quietly wrong.
struct CalibrationCapabilities
{
    int  tool_count { 1 };
    bool can_change_tools { false };
    bool can_set_temperature { false };
    bool can_set_pressure_advance { false };
    bool can_control_fan { false };
};

// The conditions a calibration was run under. These travel with the result,
// because a maximum flow measured at 240 C says nothing about 260 C.
struct CalibrationConditions
{
    std::string           material;
    std::optional<double> nozzle_diameter_mm;
    std::optional<double> layer_height_mm;
    std::optional<double> temperature_c;

    std::string describe() const;
};

struct CalibrationRequest
{
    CalibrationKind kind { CalibrationKind::MaximumFlow };

    // The tool being calibrated first, then any tool it is calibrated
    // against. A relative calibration needs at least two.
    std::vector<int> tools;

    // Inclusive sweep. A step of zero or an end below the start is refused.
    double start { 0. };
    double end { 0. };
    double step { 0. };

    CalibrationConditions conditions;
};

struct CalibrationSpecimen
{
    size_t      index { 0 };
    double      value { 0. };
    std::string label;
};

struct CalibrationProject
{
    CalibrationKind  kind { CalibrationKind::MaximumFlow };
    std::vector<int> tools;

    std::vector<CalibrationSpecimen> specimens;
    // What the operator has to do and look at. Generated, not guessed: each
    // line names the kind it belongs to.
    std::vector<std::string> instructions;
    std::vector<std::string> notes;

    // Non-empty when nothing was generated.
    std::string error;

    bool ok() const { return error.empty(); }
};

// Build a calibration sweep.
//
// The sweep is refused rather than clamped when it is degenerate, when a
// relative calibration names fewer than two tools, when a named tool does not
// exist on the machine, or when the machine never declared the capability the
// calibration depends on.
CalibrationProject generate_calibration_project(const CalibrationRequest      &request,
                                                const CalibrationCapabilities &capabilities);

// --- results ---------------------------------------------------------------

enum class ResultSource : uint8_t
{
    ManualObservation,
    Telemetry,
    Imported,
};

const char *result_source_name(ResultSource source);

struct CalibrationResult
{
    CalibrationKind kind { CalibrationKind::MaximumFlow };
    std::vector<int> tools;

    double       value { 0. };
    ResultSource source { ResultSource::ManualObservation };
    Confidence   confidence { Confidence::Insufficient };

    CalibrationConditions conditions;

    // Monotonic within a store; a lower version never displaces a higher one.
    int version { 1 };
    // When the result was produced, in seconds since an epoch the caller
    // chooses. Recorded, never invented.
    std::optional<double> recorded_at_s;

    // Set once a result has been trusted enough to print with. Overwriting one
    // requires explicit confirmation.
    bool known_good { false };

    std::vector<std::string> notes;

    bool same_subject(const CalibrationResult &other) const;
};

enum class RecordOutcome : uint8_t
{
    // Stored; there was nothing there before.
    Recorded,
    // Stored over an earlier result that was not marked known-good.
    Superseded,
    // A known-good result is already there. Nothing was changed.
    NeedsConfirmation,
    // The result cannot be stored at all.
    Rejected,
};

const char *record_outcome_name(RecordOutcome outcome);

// Record a result against a store.
//
// A known-good result is never overwritten unless `confirmed` is true, and a
// result that is older or less confident than what is already there is
// rejected rather than quietly winning on arrival order.
RecordOutcome record_calibration_result(const CalibrationResult        &incoming,
                                        bool                            confirmed,
                                        std::vector<CalibrationResult> *store,
                                        std::string                    *message);

// Turn a telemetry observation into a calibration result, carrying its
// confidence and sample count across rather than promoting it.
std::optional<CalibrationResult> result_from_observation(const ObservationSet &set,
                                                         const std::string    &metric,
                                                         int                   tool,
                                                         CalibrationKind       kind,
                                                         const TuningPolicy   &policy,
                                                         std::string          *reason);

} // namespace Slic3r

#endif // slic3r_CalibrationPrints_hpp_
