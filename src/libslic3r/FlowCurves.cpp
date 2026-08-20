///|/ TigerSlicer Phase 8: per-tool calibrated flow and speed curves.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FlowCurves.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

bool CalibratedRange::contains(double value) const
{
    if (!valid())
        return false;
    // An unset range (both ends zero) constrains nothing.
    if (minimum == 0. && maximum == 0.)
        return true;
    return value + EPSILON >= minimum && value <= maximum + EPSILON;
}

bool FlowCurve::applies_to(MultiRateFeature feature) const
{
    if (features.empty())
        return true;
    return std::find(features.begin(), features.end(), feature) != features.end();
}

const char *flow_source_name(FlowSource source)
{
    switch (source) {
    case FlowSource::ConstantMvs:            return "constant MVS";
    case FlowSource::CalibratedExact:        return "calibrated sample";
    case FlowSource::CalibratedInterpolated: return "interpolated calibration";
    case FlowSource::CalibratedExtrapolated: return "extrapolated calibration";
    }
    return "unknown";
}

const char *flow_limit_name(FlowLimit limit)
{
    switch (limit) {
    case FlowLimit::ConstantMvs:                  return "constant MVS";
    case FlowLimit::CalibratedCurve:              return "calibrated curve";
    case FlowLimit::OutsideCalibratedLayerHeight: return "outside calibrated layer height";
    case FlowLimit::OutsideCalibratedWidth:       return "outside calibrated width";
    case FlowLimit::OutsideCalibratedTemperature: return "outside calibrated temperature";
    case FlowLimit::NoCalibrationForTool:         return "no calibration for this tool";
    }
    return "unknown";
}

namespace {

std::string number(double value, int precision = 2)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

bool same_nozzle(double a, double b)
{
    return std::abs(a - b) <= EPSILON;
}

// A curve applies only when every index it declares matches.
bool indices_match(const FlowCurve &curve, const FlowQuery &query)
{
    if (curve.tool != query.tool)
        return false;
    if (curve.nozzle_diameter_mm > 0. &&
        !same_nozzle(curve.nozzle_diameter_mm, query.nozzle_diameter_mm))
        return false;
    if (!curve.material.empty() && curve.material != query.material)
        return false;
    return curve.applies_to(query.feature);
}

struct Interpolation
{
    bool        ok { false };
    double      flow { 0. };
    FlowSource  source { FlowSource::CalibratedInterpolated };
    std::string detail;
};

Interpolation interpolate(const FlowCurve &curve, double temperature_c, bool allow_extrapolation)
{
    Interpolation result;
    if (curve.samples.empty())
        return result;

    std::vector<FlowSample> samples = curve.samples;
    std::sort(samples.begin(), samples.end(),
              [](const FlowSample &a, const FlowSample &b) {
                  return a.temperature_c < b.temperature_c;
              });

    // Exactly on a measured point: no interpolation is involved at all.
    for (const FlowSample &sample : samples)
        if (std::abs(sample.temperature_c - temperature_c) <= EPSILON) {
            result.ok     = true;
            result.flow   = sample.max_flow_mm3_s;
            result.source = FlowSource::CalibratedExact;
            result.detail = "measured at " + number(sample.temperature_c, 0) + " C";
            return result;
        }

    const FlowSample &lowest  = samples.front();
    const FlowSample &highest = samples.back();

    if (temperature_c > lowest.temperature_c && temperature_c < highest.temperature_c) {
        for (size_t index = 0; index + 1 < samples.size(); ++index) {
            const FlowSample &a = samples[index];
            const FlowSample &b = samples[index + 1];
            if (temperature_c < a.temperature_c || temperature_c > b.temperature_c)
                continue;
            const double span = b.temperature_c - a.temperature_c;
            const double t    = span > 0. ? (temperature_c - a.temperature_c) / span : 0.;
            result.ok     = true;
            result.flow   = a.max_flow_mm3_s + t * (b.max_flow_mm3_s - a.max_flow_mm3_s);
            result.source = FlowSource::CalibratedInterpolated;
            result.detail = "interpolated between " + number(a.temperature_c, 0) + " C and " +
                            number(b.temperature_c, 0) + " C";
            return result;
        }
    }

    // Outside the measured range. This is where a curve stops being evidence.
    if (!allow_extrapolation) {
        result.detail = "the calibration covers " + number(lowest.temperature_c, 0) + " C to " +
                        number(highest.temperature_c, 0) + " C, and extrapolation is not enabled";
        return result;
    }

    if (samples.size() < 2) {
        result.detail = "a single calibrated sample cannot be extrapolated from";
        return result;
    }

    const FlowSample &a = temperature_c < lowest.temperature_c ? samples[0] : samples[samples.size() - 2];
    const FlowSample &b = temperature_c < lowest.temperature_c ? samples[1] : samples.back();
    const double span   = b.temperature_c - a.temperature_c;
    const double slope  = span > 0. ? (b.max_flow_mm3_s - a.max_flow_mm3_s) / span : 0.;

    result.ok     = true;
    result.flow   = std::max(0., a.max_flow_mm3_s + slope * (temperature_c - a.temperature_c));
    result.source = FlowSource::CalibratedExtrapolated;
    result.detail = "extrapolated beyond the calibrated " + number(lowest.temperature_c, 0) +
                    " C to " + number(highest.temperature_c, 0) + " C range";
    return result;
}

} // namespace

FlowResult resolve_flow_limit(const FlowQuery              &query,
                              const std::vector<FlowCurve> &curves,
                              const FlowOptions            &options)
{
    FlowResult result;

    // The configured constant is the default and the ceiling. A calibration
    // can only ever lower it.
    const double ceiling = std::max(0., options.constant_max_volumetric_speed_mm3_s);
    result.max_flow_mm3_s = ceiling;
    result.source         = FlowSource::ConstantMvs;
    result.limit          = FlowLimit::ConstantMvs;

    auto finish = [&](const std::string &explanation) {
        result.explanation = explanation;
        if (query.requested_flow_mm3_s > 0. && result.max_flow_mm3_s > 0.) {
            result.utilization   = query.requested_flow_mm3_s / result.max_flow_mm3_s;
            result.exceeds_limit = result.utilization > 1. + EPSILON;
        }
        return result;
    };

    std::vector<const FlowCurve *> matching;
    for (const FlowCurve &curve : curves)
        if (indices_match(curve, query))
            matching.push_back(&curve);

    if (matching.empty()) {
        result.limit = FlowLimit::NoCalibrationForTool;
        return finish("no calibration matches T" + std::to_string(query.tool) +
                      " with this nozzle, material and feature type; the configured constant " +
                      number(ceiling) + " mm3/s applies");
    }

    // Bounds are part of the measurement. A curve taken at 0.2 mm layers is
    // not evidence about 0.6 mm layers.
    std::vector<const FlowCurve *> in_bounds;
    bool height_excluded = false;
    bool width_excluded  = false;
    for (const FlowCurve *curve : matching) {
        if (!curve->layer_height_mm.contains(query.layer_height_mm)) {
            height_excluded = true;
            continue;
        }
        if (!curve->extrusion_width_mm.contains(query.extrusion_width_mm)) {
            width_excluded = true;
            continue;
        }
        in_bounds.push_back(curve);
    }

    if (in_bounds.empty()) {
        result.limit = height_excluded ? FlowLimit::OutsideCalibratedLayerHeight
                                       : FlowLimit::OutsideCalibratedWidth;
        (void) width_excluded;
        return finish("a calibration exists for this tool but not at " +
                      number(query.layer_height_mm) + " mm layers and " +
                      number(query.extrusion_width_mm) +
                      " mm width; the configured constant " + number(ceiling) +
                      " mm3/s applies unchanged");
    }

    // Several applicable calibrations: the most restrictive wins, because a
    // measured limit is a limit.
    double      best        = -1.;
    FlowSource  best_source = FlowSource::ConstantMvs;
    std::string best_detail;
    bool        any_usable  = false;
    std::string refusal;

    for (const FlowCurve *curve : in_bounds) {
        const Interpolation interpolated =
            interpolate(*curve, query.temperature_c, options.allow_extrapolation);
        if (!interpolated.ok) {
            if (refusal.empty())
                refusal = interpolated.detail;
            continue;
        }
        any_usable = true;
        if (best < 0. || interpolated.flow < best) {
            best        = interpolated.flow;
            best_source = interpolated.source;
            best_detail = interpolated.detail;
        }
    }

    if (!any_usable) {
        result.limit = FlowLimit::OutsideCalibratedTemperature;
        return finish("the calibration for T" + std::to_string(query.tool) +
                      " does not cover " + number(query.temperature_c, 0) + " C: " + refusal +
                      "; the configured constant " + number(ceiling) + " mm3/s applies");
    }

    result.extrapolated = best_source == FlowSource::CalibratedExtrapolated;

    if (ceiling > 0. && best >= ceiling) {
        // The calibration says the tool could do more, but the profile's
        // constant is a deliberate choice and stays the ceiling.
        return finish("the calibrated limit of " + number(best) + " mm3/s (" + best_detail +
                      ") is above the configured constant " + number(ceiling) +
                      " mm3/s, which remains the ceiling");
    }

    result.max_flow_mm3_s = best;
    result.source         = best_source;
    result.limit          = FlowLimit::CalibratedCurve;
    return finish("limited to " + number(best) + " mm3/s by T" + std::to_string(query.tool) +
                  "'s calibration (" + best_detail + ")" +
                  (result.extrapolated ? "; this value is extrapolated, not measured" : ""));
}

std::vector<std::string> validate_flow_curves(const std::vector<FlowCurve> &curves)
{
    std::vector<std::string> problems;
    for (const FlowCurve &curve : curves) {
        const std::string label = "T" + std::to_string(curve.tool) +
                                  (curve.material.empty() ? std::string()
                                                          : " / " + curve.material);
        if (curve.samples.empty()) {
            problems.push_back(label + " has a calibration with no samples");
            continue;
        }

        std::vector<double> temperatures;
        for (const FlowSample &sample : curve.samples) {
            if (sample.max_flow_mm3_s <= 0.)
                problems.push_back(label + " has a sample with a non-positive flow at " +
                                   number(sample.temperature_c, 0) + " C");
            temperatures.push_back(sample.temperature_c);
        }
        std::sort(temperatures.begin(), temperatures.end());
        for (size_t index = 0; index + 1 < temperatures.size(); ++index)
            if (std::abs(temperatures[index + 1] - temperatures[index]) <= EPSILON) {
                problems.push_back(label + " has two samples at " +
                                   number(temperatures[index], 0) + " C");
                break;
            }

        if (!curve.layer_height_mm.valid())
            problems.push_back(label + " has an inverted layer-height range");
        if (!curve.extrusion_width_mm.valid())
            problems.push_back(label + " has an inverted extrusion-width range");
    }
    return problems;
}

} // namespace Slic3r
