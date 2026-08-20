///|/ TigerSlicer Phase 11: per-tool calibration and offset maps.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CalibrationMap_hpp_
#define slic3r_CalibrationMap_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Where a number came from. This travels with every value, because a figure
// the printer measured and a figure somebody typed are not the same evidence.
enum class CalibrationSource : uint8_t
{
    PrinterMeasured,
    UserEntered,
    Imported,
    Assumed,
};

const char *calibration_source_name(CalibrationSource source);
std::optional<CalibrationSource> calibration_source_from_name(const std::string &name);

// One calibrated quantity. `uncertainty` is deliberately optional: a value
// with no stated uncertainty is not a value known perfectly, it is a value
// whose confidence nobody recorded.
struct Measurement
{
    double                value { 0. };
    std::optional<double> uncertainty { std::nullopt };
    CalibrationSource     source { CalibrationSource::Assumed };
    // ISO-8601, as written by whatever produced the document.
    std::string timestamp;
};

// How a tool's offsets move with temperature.
struct ThermalDriftPoint
{
    double temperature_c { 0. };
    double dx { 0. }, dy { 0. }, dz { 0. };
};

// A measured Z correction at one place on the bed.
struct BedCorrectionPoint
{
    double x { 0. }, y { 0. }, dz { 0. };
};

struct ToolCalibration
{
    int tool { 0 };
    // Monotonic per tool. A document may only replace an entry with a higher
    // version, so a stale import cannot silently undo a newer calibration.
    int version { 0 };

    Measurement offset_x;
    Measurement offset_y;
    Measurement offset_z;

    std::vector<ThermalDriftPoint>  thermal_drift;
    std::vector<BedCorrectionPoint> bed_corrections;

    std::optional<Measurement> first_layer_flow;

    // When the firmware owns this tool's offsets, TigerSlicer records them but
    // never applies them: applying them as well would double-correct.
    bool firmware_authoritative { false };
};

struct CalibrationMap
{
    int                          schema_version { 1 };
    std::vector<ToolCalibration> tools;

    const ToolCalibration *find(int tool) const;
};

// --- import / export ------------------------------------------------------
//
// Calibration arrives as a document the printer (or a script) writes. Nothing
// here requires control of, or a connection to, the machine.

// Returns false and fills `error` on a malformed document. A partially parsed
// map is never returned.
bool parse_calibration_document(const std::string &text,
                                CalibrationMap    *map,
                                std::string       *error);

std::string serialize_calibration_document(const CalibrationMap &map);

// Merge `incoming` into `into`. A tool entry is replaced only when the
// incoming version is strictly higher; every decision is reported.
struct MergeOutcome
{
    std::vector<std::string> applied;
    std::vector<std::string> rejected;
};

MergeOutcome merge_calibration(const CalibrationMap &incoming, CalibrationMap *into);

// --- resolution -----------------------------------------------------------

enum class OffsetStatus : uint8_t
{
    Applied,
    FirmwareAuthoritative,  // recorded, deliberately not applied
    NoCalibration,
};

const char *offset_status_name(OffsetStatus status);

struct ResolvedOffset
{
    OffsetStatus status { OffsetStatus::NoCalibration };
    double       dx { 0. }, dy { 0. }, dz { 0. };

    // Combined 1-sigma uncertainty, or absent when any contributing term had
    // none. Absent means unknown, never zero.
    std::optional<double> uncertainty;

    // True when the queried temperature fell outside the characterized drift
    // range, in which case no drift was applied.
    bool drift_uncharacterized { false };
    // True when the queried position fell outside the sampled bed points.
    bool bed_correction_uncharacterized { false };

    std::vector<std::string> notes;
};

// Resolve a tool's effective offset at a temperature and bed position.
//
// Thermal drift is interpolated only between characterized points; outside
// them no drift is applied and the result says so. The same holds for bed
// corrections.
ResolvedOffset resolve_offset(const CalibrationMap &map,
                              int                   tool,
                              double                temperature_c,
                              double                x,
                              double                y);

// Compare a required accuracy against what the calibration can actually
// support. Returns an empty string when the calibration is good enough.
//
// An unknown uncertainty never passes: "nobody recorded the confidence" is
// not evidence of adequate confidence.
std::string check_accuracy_against_calibration(const ResolvedOffset &resolved,
                                               double                required_accuracy_mm);

} // namespace Slic3r

#endif // slic3r_CalibrationMap_hpp_
