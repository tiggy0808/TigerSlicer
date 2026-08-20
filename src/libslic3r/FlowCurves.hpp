///|/ TigerSlicer Phase 8: per-tool calibrated flow and speed curves.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FlowCurves_hpp_
#define slic3r_FlowCurves_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/MultiRateLayerSchedule.hpp"

namespace Slic3r {

// One measured point on a maximum-volumetric-flow calibration: the flow the
// tool actually sustained at this temperature.
struct FlowSample
{
    double temperature_c { 0. };
    double max_flow_mm3_s { 0. };
};

// A closed interval a calibration is valid over. A curve measured at 0.2 mm
// layers says nothing about 0.6 mm layers, so the bounds travel with it.
struct CalibratedRange
{
    double minimum { 0. };
    double maximum { 0. };

    bool valid() const { return maximum >= minimum; }
    bool contains(double value) const;
};

// A calibration is indexed by everything that changes the answer. A curve
// only applies when all of its declared indices match.
struct FlowCurve
{
    int         tool { 0 };
    double      nozzle_diameter_mm { 0. };
    std::string material;

    // Ascending in temperature. Two or more samples are needed to interpolate;
    // a single sample is usable only at that exact temperature.
    std::vector<FlowSample> samples;

    CalibratedRange layer_height_mm;
    CalibratedRange extrusion_width_mm;

    // Empty means the calibration applies to every feature type.
    std::vector<MultiRateFeature> features;

    bool applies_to(MultiRateFeature feature) const;
};

struct FlowQuery
{
    int              tool { 0 };
    double           nozzle_diameter_mm { 0. };
    std::string      material;
    double           temperature_c { 0. };
    double           layer_height_mm { 0. };
    double           extrusion_width_mm { 0. };
    MultiRateFeature feature { MultiRateFeature::Walls };

    // Flow this extrusion actually wants, for utilization reporting.
    double requested_flow_mm3_s { 0. };
};

// Where the answer came from. Preview shows this so a limit is never an
// unexplained number.
enum class FlowSource : uint8_t
{
    ConstantMvs,        // the configured default; no calibration applied
    CalibratedExact,    // a sample sat exactly on the queried temperature
    CalibratedInterpolated,
    CalibratedExtrapolated,  // only ever with extrapolation explicitly allowed
};

const char *flow_source_name(FlowSource source);

// Why the flow is limited to what it is.
enum class FlowLimit : uint8_t
{
    ConstantMvs,
    CalibratedCurve,
    OutsideCalibratedLayerHeight,
    OutsideCalibratedWidth,
    OutsideCalibratedTemperature,
    NoCalibrationForTool,
};

const char *flow_limit_name(FlowLimit limit);

struct FlowOptions
{
    // The constant maximum volumetric speed that has always been the default.
    // A calibration never removes it: it can only lower the result.
    double constant_max_volumetric_speed_mm3_s { 0. };

    // Interpolation happens only inside the calibrated bounds. Going outside
    // them requires this explicit opt-in, and is flagged when it happens.
    bool allow_extrapolation { false };
};

struct FlowResult
{
    double     max_flow_mm3_s { 0. };
    FlowSource source { FlowSource::ConstantMvs };
    FlowLimit  limit { FlowLimit::ConstantMvs };

    // True when the answer came from outside the calibrated data.
    bool extrapolated { false };

    // requested / max, or 0 when nothing was requested. Above 1 means the
    // extrusion wants more than the tool can deliver.
    double utilization { 0. };
    bool   exceeds_limit { false };

    std::string explanation;
};

// Resolve the effective flow limit for one extrusion.
//
// The constant MVS remains the default and the ceiling: a calibrated curve
// may lower the limit but never raise it above what the profile allows.
// Interpolation is confined to the calibrated bounds; outside them the curve
// is not used at all unless extrapolation was explicitly allowed.
FlowResult resolve_flow_limit(const FlowQuery              &query,
                              const std::vector<FlowCurve> &curves,
                              const FlowOptions            &options);

// Report configuration problems in a curve set: unsorted or duplicated
// samples, empty sample lists, inverted ranges, non-positive flows.
std::vector<std::string> validate_flow_curves(const std::vector<FlowCurve> &curves);

} // namespace Slic3r

#endif // slic3r_FlowCurves_hpp_
