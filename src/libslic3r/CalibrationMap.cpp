///|/ TigerSlicer Phase 11: per-tool calibration and offset maps.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CalibrationMap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace Slic3r {

const char *calibration_source_name(CalibrationSource source)
{
    switch (source) {
    case CalibrationSource::PrinterMeasured: return "printer";
    case CalibrationSource::UserEntered:     return "user";
    case CalibrationSource::Imported:        return "imported";
    case CalibrationSource::Assumed:         return "assumed";
    }
    return "assumed";
}

std::optional<CalibrationSource> calibration_source_from_name(const std::string &name)
{
    if (name == "printer")  return CalibrationSource::PrinterMeasured;
    if (name == "user")     return CalibrationSource::UserEntered;
    if (name == "imported") return CalibrationSource::Imported;
    if (name == "assumed")  return CalibrationSource::Assumed;
    return std::nullopt;
}

const char *offset_status_name(OffsetStatus status)
{
    switch (status) {
    case OffsetStatus::Applied:               return "applied";
    case OffsetStatus::FirmwareAuthoritative: return "firmware authoritative";
    case OffsetStatus::NoCalibration:         return "no calibration";
    }
    return "unknown";
}

const ToolCalibration *CalibrationMap::find(int tool) const
{
    for (const ToolCalibration &entry : tools)
        if (entry.tool == tool)
            return &entry;
    return nullptr;
}

namespace {

std::string tool_label(int tool)
{
    return "T" + std::to_string(tool);
}

std::string number(double value, int precision = 3)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

// A dash means "not recorded". Writing 0 there would claim a confidence the
// document never stated.
const char *UNKNOWN_TOKEN = "-";

std::string uncertainty_token(const std::optional<double> &uncertainty)
{
    return uncertainty.has_value() ? number(*uncertainty) : UNKNOWN_TOKEN;
}

bool parse_measurement(std::istringstream &line, Measurement *measurement, std::string *error)
{
    std::string value_text, uncertainty_text, source_text, timestamp_text;
    if (!(line >> value_text >> uncertainty_text >> source_text)) {
        *error = "a measurement needs a value, an uncertainty and a source";
        return false;
    }
    line >> timestamp_text;   // optional

    try {
        measurement->value = std::stod(value_text);
    } catch (...) {
        *error = "'" + value_text + "' is not a number";
        return false;
    }
    if (! std::isfinite(measurement->value)) {
        *error = "'" + value_text + "' is not a finite measurement";
        return false;
    }

    if (uncertainty_text == UNKNOWN_TOKEN) {
        measurement->uncertainty.reset();
    } else {
        try {
            measurement->uncertainty = std::stod(uncertainty_text);
        } catch (...) {
            *error = "'" + uncertainty_text + "' is not an uncertainty";
            return false;
        }
        if (! std::isfinite(*measurement->uncertainty)) {
            *error = "an uncertainty must be finite";
            return false;
        }
        if (*measurement->uncertainty < 0.) {
            *error = "an uncertainty cannot be negative";
            return false;
        }
    }

    const std::optional<CalibrationSource> source = calibration_source_from_name(source_text);
    if (!source.has_value()) {
        *error = "'" + source_text + "' is not a recognised source";
        return false;
    }
    measurement->source    = *source;
    measurement->timestamp = timestamp_text;
    return true;
}

// Linear interpolation between the two characterized points bracketing
// `at`. Returns false when `at` is outside the characterized range, because a
// drift curve says nothing beyond where it was measured.
template<typename Point, typename Axis>
bool interpolate_1d(const std::vector<Point> &points, double at, Axis axis, double *out)
{
    if (points.size() < 2)
        return false;
    std::vector<Point> sorted = points;
    std::sort(sorted.begin(), sorted.end(),
              [](const Point &a, const Point &b) { return a.temperature_c < b.temperature_c; });
    if (at < sorted.front().temperature_c || at > sorted.back().temperature_c)
        return false;
    for (size_t index = 0; index + 1 < sorted.size(); ++index) {
        const Point &a = sorted[index];
        const Point &b = sorted[index + 1];
        if (at < a.temperature_c || at > b.temperature_c)
            continue;
        const double span = b.temperature_c - a.temperature_c;
        const double t    = span > 0. ? (at - a.temperature_c) / span : 0.;
        *out = axis(a) + t * (axis(b) - axis(a));
        return true;
    }
    return false;
}

} // namespace

bool parse_calibration_document(const std::string &text, CalibrationMap *map, std::string *error)
{
    if (map == nullptr) {
        if (error != nullptr)
            *error = "no output map was supplied";
        return false;
    }
    if (error != nullptr)
        error->clear();

    CalibrationMap parsed;
    parsed.schema_version = 0;
    ToolCalibration current;
    bool            have_tool = false;
    size_t          line_number = 0;

    auto fail = [&](const std::string &message) {
        if (error != nullptr)
            *error = "line " + std::to_string(line_number) + ": " + message;
        return false;
    };

    std::istringstream document(text);
    std::string        raw;
    while (std::getline(document, raw)) {
        ++line_number;
        // Strip a trailing carriage return so documents written on either
        // platform parse identically.
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();

        std::istringstream line(raw);
        std::string        keyword;
        if (!(line >> keyword) || keyword.empty() || keyword[0] == '#')
            continue;

        std::string detail;
        if (keyword == "schema") {
            if (!(line >> parsed.schema_version))
                return fail("schema needs a version number");
        } else if (keyword == "tool") {
            if (have_tool)
                parsed.tools.push_back(current);
            current = ToolCalibration {};
            if (!(line >> current.tool))
                return fail("tool needs a number");
            have_tool = true;
        } else if (!have_tool) {
            return fail("'" + keyword + "' appears before any tool");
        } else if (keyword == "version") {
            if (!(line >> current.version))
                return fail("version needs a number");
        } else if (keyword == "firmware_authoritative") {
            int flag = 0;
            if (!(line >> flag))
                return fail("firmware_authoritative needs 0 or 1");
            current.firmware_authoritative = flag != 0;
        } else if (keyword == "offset_x" || keyword == "offset_y" || keyword == "offset_z") {
            Measurement measurement;
            if (!parse_measurement(line, &measurement, &detail))
                return fail(detail);
            if (keyword == "offset_x")      current.offset_x = measurement;
            else if (keyword == "offset_y") current.offset_y = measurement;
            else                            current.offset_z = measurement;
        } else if (keyword == "flow") {
            Measurement measurement;
            if (!parse_measurement(line, &measurement, &detail))
                return fail(detail);
            current.first_layer_flow = measurement;
        } else if (keyword == "drift") {
            ThermalDriftPoint point;
            if (!(line >> point.temperature_c >> point.dx >> point.dy >> point.dz))
                return fail("drift needs a temperature and dx dy dz");
            current.thermal_drift.push_back(point);
        } else if (keyword == "bed") {
            BedCorrectionPoint point;
            if (!(line >> point.x >> point.y >> point.dz))
                return fail("bed needs x y dz");
            current.bed_corrections.push_back(point);
        } else {
            return fail("unrecognised keyword '" + keyword + "'");
        }
    }

    if (parsed.schema_version <= 0)
        return fail("the document does not declare a schema version");
    if (have_tool)
        parsed.tools.push_back(current);
    if (parsed.tools.empty())
        return fail("the document describes no tools");

    *map = std::move(parsed);
    return true;
}

std::string serialize_calibration_document(const CalibrationMap &map)
{
    std::ostringstream out;
    out << "schema " << map.schema_version << "\n";
    for (const ToolCalibration &entry : map.tools) {
        out << "tool " << entry.tool << "\n";
        out << "version " << entry.version << "\n";
        out << "firmware_authoritative " << (entry.firmware_authoritative ? 1 : 0) << "\n";

        auto write = [&](const char *keyword, const Measurement &measurement) {
            out << keyword << " " << number(measurement.value) << " "
                << uncertainty_token(measurement.uncertainty) << " "
                << calibration_source_name(measurement.source);
            if (!measurement.timestamp.empty())
                out << " " << measurement.timestamp;
            out << "\n";
        };
        write("offset_x", entry.offset_x);
        write("offset_y", entry.offset_y);
        write("offset_z", entry.offset_z);
        if (entry.first_layer_flow.has_value())
            write("flow", *entry.first_layer_flow);

        for (const ThermalDriftPoint &point : entry.thermal_drift)
            out << "drift " << number(point.temperature_c, 1) << " " << number(point.dx) << " "
                << number(point.dy) << " " << number(point.dz) << "\n";
        for (const BedCorrectionPoint &point : entry.bed_corrections)
            out << "bed " << number(point.x, 1) << " " << number(point.y, 1) << " "
                << number(point.dz) << "\n";
    }
    return out.str();
}

MergeOutcome merge_calibration(const CalibrationMap &incoming, CalibrationMap *into)
{
    MergeOutcome outcome;
    if (into == nullptr)
        return outcome;

    for (const ToolCalibration &entry : incoming.tools) {
        ToolCalibration *existing = nullptr;
        for (ToolCalibration &candidate : into->tools)
            if (candidate.tool == entry.tool)
                existing = &candidate;

        if (existing == nullptr) {
            into->tools.push_back(entry);
            outcome.applied.push_back(tool_label(entry.tool) + " added at version " +
                                      std::to_string(entry.version));
            continue;
        }
        if (entry.version > existing->version) {
            outcome.applied.push_back(tool_label(entry.tool) + " updated from version " +
                                      std::to_string(existing->version) + " to " +
                                      std::to_string(entry.version));
            *existing = entry;
        } else {
            // A stale document must not silently undo a newer calibration.
            outcome.rejected.push_back(tool_label(entry.tool) + " kept at version " +
                                       std::to_string(existing->version) +
                                       "; the incoming document is version " +
                                       std::to_string(entry.version));
        }
    }
    into->schema_version = std::max(into->schema_version, incoming.schema_version);
    return outcome;
}

ResolvedOffset resolve_offset(const CalibrationMap &map,
                              int                   tool,
                              double                temperature_c,
                              double                x,
                              double                y)
{
    ResolvedOffset resolved;

    const ToolCalibration *entry = map.find(tool);
    if (entry == nullptr) {
        resolved.status = OffsetStatus::NoCalibration;
        resolved.notes.push_back("no calibration is recorded for " + tool_label(tool));
        return resolved;
    }

    if (entry->firmware_authoritative) {
        // Recorded, deliberately not applied: applying it here as well would
        // double-correct against what the firmware already does.
        resolved.status = OffsetStatus::FirmwareAuthoritative;
        resolved.notes.push_back(tool_label(tool) +
                                 "'s offsets are owned by the printer firmware, so they are "
                                 "recorded but not applied");
        return resolved;
    }

    resolved.status = OffsetStatus::Applied;
    resolved.dx     = entry->offset_x.value;
    resolved.dy     = entry->offset_y.value;
    resolved.dz     = entry->offset_z.value;

    // Uncertainties add in quadrature, but only while every term has one.
    // A single unrecorded confidence makes the total unknown, not smaller.
    bool   any_unknown = !entry->offset_x.uncertainty.has_value() ||
                       !entry->offset_y.uncertainty.has_value() ||
                       !entry->offset_z.uncertainty.has_value();
    double variance = 0.;
    for (const Measurement *measurement : { &entry->offset_x, &entry->offset_y, &entry->offset_z })
        if (measurement->uncertainty.has_value())
            variance += *measurement->uncertainty * *measurement->uncertainty;

    // --- thermal drift ------------------------------------------------------
    if (entry->thermal_drift.empty()) {
        resolved.drift_uncharacterized = true;
        resolved.notes.push_back("no thermal drift is characterized for " + tool_label(tool));
    } else {
        double dx = 0., dy = 0., dz = 0.;
        const bool ok_x = interpolate_1d(entry->thermal_drift, temperature_c,
                                         [](const ThermalDriftPoint &p) { return p.dx; }, &dx);
        const bool ok_y = interpolate_1d(entry->thermal_drift, temperature_c,
                                         [](const ThermalDriftPoint &p) { return p.dy; }, &dy);
        const bool ok_z = interpolate_1d(entry->thermal_drift, temperature_c,
                                         [](const ThermalDriftPoint &p) { return p.dz; }, &dz);
        if (ok_x && ok_y && ok_z) {
            resolved.dx += dx;
            resolved.dy += dy;
            resolved.dz += dz;
        } else {
            // Outside the measured range the curve is not evidence.
            resolved.drift_uncharacterized = true;
            any_unknown                    = true;
            resolved.notes.push_back("thermal drift for " + tool_label(tool) +
                                     " is not characterized at " + number(temperature_c, 1) +
                                     " C, so none was applied");
        }
    }

    // --- bed location -------------------------------------------------------
    if (entry->bed_corrections.empty()) {
        resolved.bed_correction_uncharacterized = true;
        resolved.notes.push_back("no bed-location correction is characterized for " +
                                 tool_label(tool));
    } else {
        // Nearest sampled point, with the distance reported rather than
        // silently extrapolated across the bed.
        const BedCorrectionPoint *nearest = nullptr;
        double best = std::numeric_limits<double>::max();
        for (const BedCorrectionPoint &point : entry->bed_corrections) {
            const double distance = std::hypot(point.x - x, point.y - y);
            if (distance < best) {
                best    = distance;
                nearest = &point;
            }
        }
        if (nearest != nullptr) {
            resolved.dz += nearest->dz;
            resolved.notes.push_back("bed correction taken from the sample " + number(best, 1) +
                                     " mm away at (" + number(nearest->x, 1) + ", " +
                                     number(nearest->y, 1) + ")");
        }
    }

    if (!any_unknown)
        resolved.uncertainty = std::sqrt(variance);
    else
        resolved.notes.push_back("the combined uncertainty is unknown because at least one "
                                 "contributing value has no recorded confidence");

    return resolved;
}

std::string check_accuracy_against_calibration(const ResolvedOffset &resolved,
                                               double                required_accuracy_mm)
{
    if (required_accuracy_mm <= 0.)
        return {};

    if (resolved.status == OffsetStatus::NoCalibration)
        return "an accuracy of " + number(required_accuracy_mm) +
               " mm is required but this tool has no calibration at all";

    if (!resolved.uncertainty.has_value())
        // Unknown confidence is not adequate confidence.
        return "an accuracy of " + number(required_accuracy_mm) +
               " mm is required but the calibration's confidence was never recorded, so it "
               "cannot be shown to be good enough";

    if (*resolved.uncertainty > required_accuracy_mm)
        return "an accuracy of " + number(required_accuracy_mm) +
               " mm is required but the calibration is only confident to " +
               number(*resolved.uncertainty) + " mm";

    return {};
}

} // namespace Slic3r
