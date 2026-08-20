///|/ TigerSlicer Phase 15: automatic calibration-print generator.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CalibrationPrints.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *calibration_kind_name(CalibrationKind kind)
{
    switch (kind) {
    case CalibrationKind::MaximumFlow:           return "maximum_flow";
    case CalibrationKind::PressureAdvance:       return "pressure_advance";
    case CalibrationKind::Retraction:            return "retraction";
    case CalibrationKind::StandbyOoze:           return "standby_ooze";
    case CalibrationKind::Priming:               return "priming";
    case CalibrationKind::Temperature:           return "temperature";
    case CalibrationKind::XyOffset:              return "xy_offset";
    case CalibrationKind::ZOffset:               return "z_offset";
    case CalibrationKind::FirstLayer:            return "first_layer";
    case CalibrationKind::Bridging:              return "bridging";
    case CalibrationKind::Cooling:               return "cooling";
    case CalibrationKind::Interface:             return "interface";
    case CalibrationKind::ToolchangeReliability: return "toolchange_reliability";
    }
    return "maximum_flow";
}

namespace {

const CalibrationKind kAllKinds[] = {
    CalibrationKind::MaximumFlow,   CalibrationKind::PressureAdvance,
    CalibrationKind::Retraction,    CalibrationKind::StandbyOoze,
    CalibrationKind::Priming,       CalibrationKind::Temperature,
    CalibrationKind::XyOffset,      CalibrationKind::ZOffset,
    CalibrationKind::FirstLayer,    CalibrationKind::Bridging,
    CalibrationKind::Cooling,       CalibrationKind::Interface,
    CalibrationKind::ToolchangeReliability,
};

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

} // namespace

bool parse_calibration_kind(const std::string &text, CalibrationKind *kind)
{
    for (CalibrationKind candidate : kAllKinds)
        if (text == calibration_kind_name(candidate)) {
            if (kind != nullptr)
                *kind = candidate;
            return true;
        }
    return false;
}

bool calibration_kind_is_relative(CalibrationKind kind)
{
    return kind == CalibrationKind::XyOffset || kind == CalibrationKind::Interface ||
           kind == CalibrationKind::ToolchangeReliability;
}

const char *calibration_kind_unit(CalibrationKind kind)
{
    switch (kind) {
    case CalibrationKind::MaximumFlow:           return "mm3/s";
    case CalibrationKind::PressureAdvance:       return "s";
    case CalibrationKind::Retraction:            return "mm";
    case CalibrationKind::StandbyOoze:           return "mm3";
    case CalibrationKind::Priming:               return "mm3";
    case CalibrationKind::Temperature:           return "C";
    case CalibrationKind::XyOffset:              return "mm";
    case CalibrationKind::ZOffset:               return "mm";
    case CalibrationKind::FirstLayer:            return "mm";
    case CalibrationKind::Bridging:              return "mm";
    case CalibrationKind::Cooling:               return "%";
    case CalibrationKind::Interface:             return "mm";
    case CalibrationKind::ToolchangeReliability: return "changes";
    }
    return "";
}

const char *result_source_name(ResultSource source)
{
    switch (source) {
    case ResultSource::ManualObservation: return "manual";
    case ResultSource::Telemetry:         return "telemetry";
    case ResultSource::Imported:          return "imported";
    }
    return "manual";
}

const char *record_outcome_name(RecordOutcome outcome)
{
    switch (outcome) {
    case RecordOutcome::Recorded:          return "recorded";
    case RecordOutcome::Superseded:        return "superseded";
    case RecordOutcome::NeedsConfirmation: return "needs confirmation";
    case RecordOutcome::Rejected:          return "rejected";
    }
    return "rejected";
}

std::string CalibrationConditions::describe() const
{
    std::ostringstream out;
    out << (material.empty() ? "an unrecorded material" : material);
    // Each unrecorded condition says so. A calibration whose conditions are
    // half unknown must not read as fully characterized.
    out << ", nozzle "
        << (nozzle_diameter_mm.has_value() ? number(*nozzle_diameter_mm, 2) + " mm"
                                           : "unrecorded");
    out << ", layer "
        << (layer_height_mm.has_value() ? number(*layer_height_mm, 2) + " mm" : "unrecorded");
    out << ", "
        << (temperature_c.has_value() ? number(*temperature_c, 0) + " C" : "temperature "
                                                                           "unrecorded");
    return out.str();
}

bool CalibrationResult::same_subject(const CalibrationResult &other) const
{
    return kind == other.kind && tools == other.tools &&
           conditions.material == other.conditions.material;
}

// --- generation ------------------------------------------------------------

namespace {

std::string capability_gap(CalibrationKind kind, const CalibrationCapabilities &capabilities)
{
    switch (kind) {
    case CalibrationKind::Temperature:
        if (!capabilities.can_set_temperature)
            return "the machine does not declare temperature control, so a temperature sweep "
                   "cannot be printed";
        break;
    case CalibrationKind::PressureAdvance:
        if (!capabilities.can_set_pressure_advance)
            return "the machine does not declare pressure advance, so the sweep would print "
                   "identical specimens";
        break;
    case CalibrationKind::Cooling:
        if (!capabilities.can_control_fan)
            return "the machine does not declare fan control, so a cooling sweep would print "
                   "identical specimens";
        break;
    case CalibrationKind::XyOffset:
    case CalibrationKind::Interface:
    case CalibrationKind::ToolchangeReliability:
        if (!capabilities.can_change_tools)
            return "the machine does not declare toolchanging, so a two-tool calibration "
                   "cannot be printed";
        break;
    default: break;
    }
    return {};
}

std::vector<std::string> instructions_for(CalibrationKind kind, const std::vector<int> &tools)
{
    const std::string first = "T" + std::to_string(tools.front());
    const std::string other = tools.size() > 1 ? "T" + std::to_string(tools[1]) : first;

    switch (kind) {
    case CalibrationKind::MaximumFlow:
        return { "print the sweep and find the first specimen where extrusion visibly under-"
                 "delivers on " + first,
                 "take the value one step below that specimen, not the failing one" };
    case CalibrationKind::PressureAdvance:
        return { "look along the corners of each specimen for bulging and thinning on " + first,
                 "choose the specimen with the most even corner width" };
    case CalibrationKind::Retraction:
        return { "inspect the travel gaps between the towers for stringing on " + first,
                 "choose the smallest value that leaves no strings" };
    case CalibrationKind::StandbyOoze:
        return { "weigh or measure the material deposited at the purge position after each "
                 "idle period on " + first };
    case CalibrationKind::Priming:
        return { "find the first specimen that starts a clean full-width line on " + first };
    case CalibrationKind::Temperature:
        return { "check layer adhesion and surface finish on each band of " + first,
                 "choose the coolest band that still bonds" };
    case CalibrationKind::XyOffset:
        return { "read the vernier where the " + other + " marks align with the " + first +
                 " marks",
                 "the aligned pair gives the offset directly" };
    case CalibrationKind::ZOffset:
        return { "find the first band on " + first + " that is fully squished without "
                 "ridging" };
    case CalibrationKind::FirstLayer:
        return { "check the first layer of " + first + " for gaps between adjacent lines" };
    case CalibrationKind::Bridging:
        return { "inspect the underside of each span printed by " + first + " for sag" };
    case CalibrationKind::Cooling:
        return { "check overhang quality and layer adhesion together on " + first,
                 "more cooling is not better if the layers separate" };
    case CalibrationKind::Interface:
        return { "separate the " + other + " interface from the " + first + " body and judge "
                 "the surface left behind" };
    case CalibrationKind::ToolchangeReliability:
        return { "run the specified number of changes between " + first + " and " + other,
                 "record every retry and every failure, not only the failures" };
    }
    return {};
}

} // namespace

CalibrationProject generate_calibration_project(const CalibrationRequest      &request,
                                                const CalibrationCapabilities &capabilities)
{
    CalibrationProject project;
    project.kind  = request.kind;
    project.tools = request.tools;

    if (request.tools.empty()) {
        project.error = "no tool was named for this calibration";
        return project;
    }
    if (calibration_kind_is_relative(request.kind) && request.tools.size() < 2) {
        // A relative calibration against nothing is meaningless, not a
        // degenerate case worth generating.
        project.error = std::string(calibration_kind_name(request.kind)) +
                        " is measured between two tools, but only one was named";
        return project;
    }
    for (int tool : request.tools)
        if (tool < 0 || tool >= capabilities.tool_count) {
            project.error = "T" + std::to_string(tool) + " does not exist on this machine";
            return project;
        }
    for (size_t a = 0; a < request.tools.size(); ++a)
        for (size_t b = a + 1; b < request.tools.size(); ++b)
            if (request.tools[a] == request.tools[b]) {
                project.error = "T" + std::to_string(request.tools[a]) +
                                " was named twice; a tool cannot be calibrated against itself";
                return project;
            }
    if (!project.error.empty())
        return project;

    const std::string gap = capability_gap(request.kind, capabilities);
    if (!gap.empty()) {
        project.error = gap;
        return project;
    }

    if (request.step <= EPSILON) {
        project.error = "the sweep step must be positive; a zero step would print the same "
                        "specimen repeatedly";
        return project;
    }
    if (request.end < request.start - EPSILON) {
        project.error = "the sweep ends below where it starts";
        return project;
    }

    const char *unit = calibration_kind_unit(request.kind);
    for (double value = request.start; value <= request.end + EPSILON; value += request.step) {
        CalibrationSpecimen specimen;
        specimen.index = project.specimens.size();
        specimen.value = value;
        specimen.label = number(value, 3) + " " + unit;
        project.specimens.push_back(specimen);
        if (project.specimens.size() >= 200) {
            // A capped sweep says it was capped.
            project.notes.push_back("the sweep was capped at 200 specimens; the requested "
                                    "range and step would have produced more");
            break;
        }
    }

    project.instructions = instructions_for(request.kind, request.tools);
    project.notes.push_back("conditions: " + request.conditions.describe());
    if (request.conditions.material.empty())
        project.notes.push_back("no material was recorded, so this result will not be "
                                "comparable against another run");
    return project;
}

// --- results ---------------------------------------------------------------

RecordOutcome record_calibration_result(const CalibrationResult        &incoming,
                                        bool                            confirmed,
                                        std::vector<CalibrationResult> *store,
                                        std::string                    *message)
{
    auto say = [message](const std::string &text) {
        if (message != nullptr)
            *message = text;
    };

    if (store == nullptr) {
        say("there is nowhere to record this result");
        return RecordOutcome::Rejected;
    }
    if (incoming.confidence == Confidence::Insufficient) {
        say("a result with insufficient confidence is not stored; it would look like "
            "calibration without being any");
        return RecordOutcome::Rejected;
    }

    auto existing = std::find_if(store->begin(), store->end(),
                                 [&](const CalibrationResult &candidate) {
                                     return candidate.same_subject(incoming);
                                 });

    if (existing == store->end()) {
        store->push_back(incoming);
        say("recorded");
        return RecordOutcome::Recorded;
    }

    if (existing->version > incoming.version) {
        say("a newer result (version " + std::to_string(existing->version) +
            ") is already recorded; this one is version " + std::to_string(incoming.version));
        return RecordOutcome::Rejected;
    }

    if (existing->known_good && !confirmed) {
        // The whole point of marking a calibration known-good is that a later
        // run does not quietly replace it.
        say("a known-good result for " + std::string(calibration_kind_name(incoming.kind)) +
            " is already recorded; confirm to replace it");
        return RecordOutcome::NeedsConfirmation;
    }

    if (existing->version == incoming.version && existing->confidence > incoming.confidence &&
        !confirmed) {
        say("the recorded result has " + std::string(confidence_name(existing->confidence)) +
            " confidence and this one has " + confidence_name(incoming.confidence) +
            "; confirm to replace it anyway");
        return RecordOutcome::NeedsConfirmation;
    }

    *existing = incoming;
    say("superseded the previous result");
    return RecordOutcome::Superseded;
}

std::optional<CalibrationResult> result_from_observation(const ObservationSet &set,
                                                         const std::string    &metric,
                                                         int                   tool,
                                                         CalibrationKind       kind,
                                                         const TuningPolicy   &policy,
                                                         std::string          *reason)
{
    auto refuse = [reason](const std::string &text) -> std::optional<CalibrationResult> {
        if (reason != nullptr)
            *reason = text;
        return std::nullopt;
    };

    if (set.completion != JobCompletion::Completed)
        return refuse("the job did not complete, so its measurements do not describe a print "
                      "that ran to the end");

    const Observation *observation = set.find(metric, tool);
    if (observation == nullptr)
        return refuse("nothing was measured for " + metric + " on T" + std::to_string(tool));
    if (observation->samples < policy.min_samples)
        return refuse(metric + " has only " + std::to_string(observation->samples) +
                      " sample(s), below the " + std::to_string(policy.min_samples) +
                      " required");

    CalibrationResult result;
    result.kind   = kind;
    result.tools  = { tool };
    result.value  = observation->mean;
    result.source = ResultSource::Telemetry;
    // Confidence comes from the evidence, not from the fact that a printer
    // produced it. Telemetry is not automatically better than an eye.
    result.confidence = observation->samples >= policy.min_samples * 4
                            ? Confidence::High
                            : (observation->samples >= policy.min_samples * 2
                                   ? Confidence::Medium
                                   : Confidence::Low);
    result.notes.push_back("derived from " + std::to_string(observation->samples) +
                           " telemetry sample(s), range " + number(observation->min, 3) +
                           " to " + number(observation->max, 3));
    return result;
}

} // namespace Slic3r
