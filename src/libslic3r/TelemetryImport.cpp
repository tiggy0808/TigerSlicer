///|/ TigerSlicer Phase 14: telemetry-driven self-tuning.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "TelemetryImport.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *const kMetricHeatTimeS         = "heat_time_s";
const char *const kMetricToolchangeTimeS   = "toolchange_time_s";
const char *const kMetricToolchangeRetries = "toolchange_retries";
const char *const kMetricPurgeVolumeMm3    = "purge_volume_mm3";
const char *const kMetricPauses            = "pauses";
const char *const kMetricFaults            = "faults";

namespace {
constexpr int kDocumentVersion = 1;
} // namespace

int telemetry_document_version() { return kDocumentVersion; }

const char *telemetry_source_name(TelemetrySource source)
{
    switch (source) {
    case TelemetrySource::KlipperLog:          return "klipper";
    case TelemetrySource::MoonrakerHistory:    return "moonraker";
    case TelemetrySource::TigerSlicerManifest: return "manifest";
    case TelemetrySource::Unknown:             return "unknown";
    }
    return "unknown";
}

const char *job_completion_name(JobCompletion completion)
{
    switch (completion) {
    case JobCompletion::Completed:   return "completed";
    case JobCompletion::Interrupted: return "interrupted";
    case JobCompletion::Unknown:     return "unknown";
    }
    return "unknown";
}

const char *telemetry_event_kind_name(TelemetryEventKind kind)
{
    switch (kind) {
    case TelemetryEventKind::JobStart:        return "job_start";
    case TelemetryEventKind::JobEnd:          return "job_end";
    case TelemetryEventKind::HeatStart:       return "heat_start";
    case TelemetryEventKind::HeatReached:     return "heat_reached";
    case TelemetryEventKind::ToolchangeStart: return "toolchange_start";
    case TelemetryEventKind::ToolchangeEnd:   return "toolchange_end";
    case TelemetryEventKind::ToolchangeRetry: return "toolchange_retry";
    case TelemetryEventKind::PurgeStart:      return "purge_start";
    case TelemetryEventKind::PurgeEnd:        return "purge_end";
    case TelemetryEventKind::Pause:           return "pause";
    case TelemetryEventKind::Resume:          return "resume";
    case TelemetryEventKind::Fault:           return "fault";
    }
    return "job_start";
}

bool parse_telemetry_event_kind(const std::string &text, TelemetryEventKind *kind)
{
    static const TelemetryEventKind all[] = {
        TelemetryEventKind::JobStart,        TelemetryEventKind::JobEnd,
        TelemetryEventKind::HeatStart,       TelemetryEventKind::HeatReached,
        TelemetryEventKind::ToolchangeStart, TelemetryEventKind::ToolchangeEnd,
        TelemetryEventKind::ToolchangeRetry, TelemetryEventKind::PurgeStart,
        TelemetryEventKind::PurgeEnd,        TelemetryEventKind::Pause,
        TelemetryEventKind::Resume,          TelemetryEventKind::Fault,
    };
    for (TelemetryEventKind candidate : all)
        if (text == telemetry_event_kind_name(candidate)) {
            if (kind != nullptr)
                *kind = candidate;
            return true;
        }
    return false;
}

const char *identity_match_name(IdentityMatch match)
{
    switch (match) {
    case IdentityMatch::Match:           return "match";
    case IdentityMatch::MachineMismatch: return "machine mismatch";
    case IdentityMatch::ManifestMismatch: return "manifest mismatch";
    case IdentityMatch::ProfileMismatch: return "profile mismatch";
    case IdentityMatch::Unidentified:    return "unidentified";
    }
    return "unidentified";
}

const char *confidence_name(Confidence confidence)
{
    switch (confidence) {
    case Confidence::Insufficient: return "insufficient";
    case Confidence::Low:          return "low";
    case Confidence::Medium:       return "medium";
    case Confidence::High:         return "high";
    }
    return "insufficient";
}

const Observation *ObservationSet::find(const std::string &metric, int tool) const
{
    for (const Observation &observation : observations)
        if (observation.metric == metric && observation.tool == tool)
            return &observation;
    return nullptr;
}

// --- document --------------------------------------------------------------

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

bool parse_source(const std::string &text, TelemetrySource *source)
{
    // "unknown" is accepted so a document that recorded no source round-trips
    // as having recorded no source, rather than becoming unreadable.
    static const TelemetrySource all[] = { TelemetrySource::KlipperLog,
                                           TelemetrySource::MoonrakerHistory,
                                           TelemetrySource::TigerSlicerManifest,
                                           TelemetrySource::Unknown };
    for (TelemetrySource candidate : all)
        if (text == telemetry_source_name(candidate)) {
            *source = candidate;
            return true;
        }
    return false;
}

bool parse_completion(const std::string &text, JobCompletion *completion)
{
    static const JobCompletion all[] = { JobCompletion::Completed, JobCompletion::Interrupted,
                                         JobCompletion::Unknown };
    for (JobCompletion candidate : all)
        if (text == job_completion_name(candidate)) {
            *completion = candidate;
            return true;
        }
    return false;
}

bool fail(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
    return false;
}

} // namespace

bool parse_telemetry_document(const std::string &text, TelemetryJob *job, std::string *error)
{
    if (job == nullptr)
        return fail(error, "no destination for the parsed telemetry");

    TelemetryJob parsed;
    std::istringstream input(text);
    std::string        line;
    size_t             line_number = 0;
    bool               saw_version = false;

    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream fields(line);
        std::string        key;
        fields >> key;
        const std::string at = " at line " + std::to_string(line_number);

        if (key == "telemetry") {
            int version = 0;
            if (!(fields >> version))
                return fail(error, "the telemetry version is missing" + at);
            if (version > kDocumentVersion)
                // A newer document may mean things this build does not know.
                // Reading the parts we recognize would silently drop the rest.
                return fail(error, "this telemetry document is version " +
                                       std::to_string(version) +
                                       ", which this build does not understand");
            parsed.version = version;
            saw_version    = true;
        } else if (key == "source") {
            std::string value;
            fields >> value;
            if (!parse_source(value, &parsed.source))
                return fail(error, "unknown telemetry source '" + value + "'" + at);
        } else if (key == "machine" || key == "manifest") {
            std::string value;
            fields >> value;
            // A dash is how the document says "not recorded"; it must not
            // become an identity that looks like a real one.
            if (value == "-")
                value.clear();
            (key == "machine" ? parsed.identity.machine_id
                              : parsed.identity.manifest_id) = value;
        } else if (key == "profile") {
            std::string value;
            fields >> value;
            if (value != "-")
                parsed.identity.profile_version = value;
        } else if (key == "completion") {
            std::string value;
            fields >> value;
            if (!parse_completion(value, &parsed.completion))
                return fail(error, "unknown completion state '" + value + "'" + at);
        } else if (key == "filament") {
            std::string value;
            fields >> value;
            if (value != "-") {
                try {
                    parsed.filament_used_mm3 = std::stod(value);
                } catch (...) {
                    return fail(error, "the filament figure is not a number" + at);
                }
            }
        } else if (key == "event") {
            TelemetryEvent event;
            std::string    kind_text, tool_text, value_text;
            if (!(fields >> event.at_s >> kind_text >> tool_text >> value_text))
                return fail(error, "an event is missing fields" + at);
            if (!parse_telemetry_event_kind(kind_text, &event.kind))
                return fail(error, "unknown event kind '" + kind_text + "'" + at);
            try {
                event.tool = std::stoi(tool_text);
            } catch (...) {
                return fail(error, "the event tool is not a number" + at);
            }
            if (value_text != "-") {
                try {
                    event.value = std::stod(value_text);
                } catch (...) {
                    return fail(error, "the event value is not a number" + at);
                }
            }
            std::getline(fields, event.detail);
            while (!event.detail.empty() && event.detail.front() == ' ')
                event.detail.erase(event.detail.begin());
            parsed.events.push_back(event);
        } else {
            return fail(error, "unknown telemetry key '" + key + "'" + at);
        }
    }

    if (!saw_version)
        return fail(error, "the document does not declare a telemetry version");

    std::stable_sort(parsed.events.begin(), parsed.events.end(),
                     [](const TelemetryEvent &a, const TelemetryEvent &b) {
                         return a.at_s < b.at_s;
                     });

    *job = parsed;
    return true;
}

std::string serialize_telemetry_document(const TelemetryJob &job)
{
    std::ostringstream out;
    out << "telemetry " << kDocumentVersion << "\n";
    out << "source " << telemetry_source_name(job.source) << "\n";
    out << "machine " << (job.identity.machine_id.empty() ? "-" : job.identity.machine_id)
        << "\n";
    out << "manifest " << (job.identity.manifest_id.empty() ? "-" : job.identity.manifest_id)
        << "\n";
    // A dash means "not recorded". It must never round-trip into a value.
    out << "profile "
        << (job.identity.profile_version.has_value() ? *job.identity.profile_version : "-")
        << "\n";
    out << "completion " << job_completion_name(job.completion) << "\n";
    out << "filament "
        << (job.filament_used_mm3.has_value() ? number(*job.filament_used_mm3, 3) : "-") << "\n";
    for (const TelemetryEvent &event : job.events) {
        out << "event " << number(event.at_s, 3) << " "
            << telemetry_event_kind_name(event.kind) << " " << event.tool << " "
            << (event.value.has_value() ? number(*event.value, 3) : "-");
        if (!event.detail.empty())
            out << " " << event.detail;
        out << "\n";
    }
    return out.str();
}

// --- identity --------------------------------------------------------------

IdentityMatch match_identity(const JobIdentity &imported, const JobIdentity &expected)
{
    if (imported.machine_id.empty() || imported.manifest_id.empty())
        return IdentityMatch::Unidentified;
    if (imported.machine_id != expected.machine_id)
        return IdentityMatch::MachineMismatch;
    if (imported.manifest_id != expected.manifest_id)
        return IdentityMatch::ManifestMismatch;
    if (expected.profile_version.has_value()) {
        if (!imported.profile_version.has_value())
            // The log never said which profile produced it, so it cannot be
            // claimed to describe this one.
            return IdentityMatch::Unidentified;
        if (*imported.profile_version != *expected.profile_version)
            return IdentityMatch::ProfileMismatch;
    }
    return IdentityMatch::Match;
}

// --- observations ----------------------------------------------------------

namespace {

struct Accumulator
{
    double sum { 0. };
    double min { 0. };
    double max { 0. };
    size_t count { 0 };

    void add(double value)
    {
        if (count == 0) {
            min = max = value;
        } else {
            min = std::min(min, value);
            max = std::max(max, value);
        }
        sum += value;
        ++count;
    }
};

using AccumulatorKey = std::pair<std::string, int>;

void emit(std::vector<Observation> &into, const std::map<AccumulatorKey, Accumulator> &from)
{
    for (const auto &entry : from) {
        Observation observation;
        observation.metric  = entry.first.first;
        observation.tool    = entry.first.second;
        observation.samples = entry.second.count;
        observation.mean = entry.second.count > 0
                               ? entry.second.sum / double(entry.second.count)
                               : 0.;
        observation.min  = entry.second.min;
        observation.max  = entry.second.max;
        into.push_back(observation);
    }
}

} // namespace

ObservationSet extract_observations(const TelemetryJob &job)
{
    ObservationSet set;
    set.identity   = job.identity;
    set.completion = job.completion;

    if (job.completion == JobCompletion::Interrupted)
        set.notes.push_back("this job was interrupted, so its timings describe a print that "
                            "stopped rather than one that ran");
    else if (job.completion == JobCompletion::Unknown)
        set.notes.push_back("the log does not say whether this job completed");

    std::map<AccumulatorKey, Accumulator> accumulators;
    std::map<int, double>                 heat_open, change_open, purge_open;
    std::map<int, size_t>                 retries;
    size_t                                pauses = 0, faults = 0;

    auto open = [](std::map<int, double> &pending, int tool, double at_s,
                   std::vector<std::string> &notes, const char *what) {
        if (pending.count(tool) != 0)
            notes.push_back(std::string(what) + " for T" + std::to_string(tool) +
                            " started again before the previous one finished; the earlier one "
                            "is not counted");
        pending[tool] = at_s;
    };

    auto close = [&](std::map<int, double> &pending, int tool, double at_s,
                     const char *metric, const char *what) {
        auto found = pending.find(tool);
        if (found == pending.end()) {
            set.notes.push_back(std::string(what) + " for T" + std::to_string(tool) +
                                " ended without a recorded start, so it contributes no timing");
            return;
        }
        accumulators[{ metric, tool }].add(at_s - found->second);
        pending.erase(found);
    };

    for (const TelemetryEvent &event : job.events) {
        switch (event.kind) {
        case TelemetryEventKind::HeatStart:
            open(heat_open, event.tool, event.at_s, set.notes, "heating");
            break;
        case TelemetryEventKind::HeatReached:
            close(heat_open, event.tool, event.at_s, kMetricHeatTimeS, "heating");
            break;
        case TelemetryEventKind::ToolchangeStart:
            open(change_open, event.tool, event.at_s, set.notes, "a toolchange");
            break;
        case TelemetryEventKind::ToolchangeEnd:
            close(change_open, event.tool, event.at_s, kMetricToolchangeTimeS, "a toolchange");
            break;
        case TelemetryEventKind::ToolchangeRetry:
            ++retries[event.tool];
            break;
        case TelemetryEventKind::PurgeStart:
            open(purge_open, event.tool, event.at_s, set.notes, "a purge");
            break;
        case TelemetryEventKind::PurgeEnd:
            purge_open.erase(event.tool);
            if (event.value.has_value())
                accumulators[{ kMetricPurgeVolumeMm3, event.tool }].add(*event.value);
            else
                set.notes.push_back("a purge for T" + std::to_string(event.tool) +
                                    " recorded no volume, so it contributes no measurement");
            break;
        case TelemetryEventKind::Pause: ++pauses; break;
        case TelemetryEventKind::Fault:
            ++faults;
            set.notes.push_back("fault at " + number(event.at_s, 1) + " s" +
                                (event.detail.empty() ? std::string {} : ": " + event.detail));
            break;
        default: break;
        }
    }

    // Anything still open never finished. It is reported, not estimated.
    for (const auto &entry : heat_open)
        set.notes.push_back("heating for T" + std::to_string(entry.first) +
                            " never reached temperature in this log");
    for (const auto &entry : change_open)
        set.notes.push_back("a toolchange for T" + std::to_string(entry.first) +
                            " never completed in this log");
    for (const auto &entry : purge_open)
        set.notes.push_back("a purge for T" + std::to_string(entry.first) +
                            " never completed in this log");

    for (const auto &entry : retries)
        accumulators[{ kMetricToolchangeRetries, entry.first }].add(double(entry.second));
    if (pauses > 0)
        accumulators[{ kMetricPauses, -1 }].add(double(pauses));
    if (faults > 0)
        accumulators[{ kMetricFaults, -1 }].add(double(faults));

    emit(set.observations, accumulators);
    return set;
}

// --- proposals -------------------------------------------------------------

TuningReport propose_tuning(const std::vector<ObservationSet> &sets,
                            const std::vector<CurrentSetting> &settings,
                            const TuningPolicy                &policy)
{
    TuningReport report;

    // Pool the usable evidence first, so a proposal is backed by everything
    // that legitimately bears on it rather than by whichever import came last.
    std::map<AccumulatorKey, Accumulator> pooled;
    size_t                                usable_sets = 0;

    for (const ObservationSet &set : sets) {
        if (set.completion != JobCompletion::Completed) {
            report.excluded.push_back(
                "a " + std::string(job_completion_name(set.completion)) +
                " job was excluded from the evidence; only completed jobs describe how the "
                "machine behaves when it runs to the end");
            continue;
        }
        ++usable_sets;
        for (const Observation &observation : set.observations) {
            Accumulator &accumulator = pooled[{ observation.metric, observation.tool }];
            // Re-pool from the mean weighted by its own sample count, which is
            // all the detail an ObservationSet keeps.
            for (size_t sample = 0; sample < observation.samples; ++sample)
                accumulator.add(observation.mean);
            if (observation.samples > 0) {
                accumulator.min = std::min(accumulator.min, observation.min);
                accumulator.max = std::max(accumulator.max, observation.max);
            }
        }
    }

    if (usable_sets == 0) {
        report.notes.push_back("no completed job was available, so nothing is proposed");
        return report;
    }

    for (const CurrentSetting &setting : settings) {
        auto found = pooled.find({ setting.setting, setting.tool });
        if (found == pooled.end()) {
            report.notes.push_back("nothing was measured for " + setting.setting + " on T" +
                                   std::to_string(setting.tool) + ", so it is left alone");
            continue;
        }

        const Accumulator &accumulator = found->second;
        const double       measured    = accumulator.sum / double(accumulator.count);

        if (accumulator.count < policy.min_samples) {
            // Too little evidence is a note, not a quiet low-confidence
            // number: a proposal on the page invites a click.
            report.notes.push_back(
                setting.setting + " on T" + std::to_string(setting.tool) + " has only " +
                std::to_string(accumulator.count) + " sample(s), below the " +
                std::to_string(policy.min_samples) +
                " required, so no change is proposed");
            continue;
        }

        const double reference = std::abs(setting.value) > EPSILON ? std::abs(setting.value) : 1.;
        const double relative  = std::abs(measured - setting.value) / reference;
        if (relative < policy.min_relative_change) {
            report.notes.push_back(setting.setting + " on T" + std::to_string(setting.tool) +
                                   " is already within " +
                                   number(policy.min_relative_change * 100., 0) +
                                   "% of what was measured");
            continue;
        }

        TuningProposal proposal;
        proposal.setting        = setting.setting;
        proposal.tool           = setting.tool;
        proposal.current_value  = setting.value;
        proposal.proposed_value = measured;
        proposal.samples        = accumulator.count;
        proposal.confidence     = accumulator.count >= policy.min_samples * 4
                                      ? Confidence::High
                                      : (accumulator.count >= policy.min_samples * 2
                                             ? Confidence::Medium
                                             : Confidence::Low);
        proposal.evidence.push_back("measured " + number(measured, 3) + " over " +
                                    std::to_string(accumulator.count) + " sample(s), range " +
                                    number(accumulator.min, 3) + " to " +
                                    number(accumulator.max, 3));
        proposal.evidence.push_back("the configured value is " + number(setting.value, 3) +
                                    ", a difference of " + number(relative * 100., 1) + "%");
        report.proposals.push_back(proposal);
    }

    return report;
}

bool apply_tuning_proposals(const std::vector<TuningProposal> &proposals,
                            const std::vector<bool>           &accepted,
                            std::vector<CurrentSetting>       *settings,
                            std::string                       *error)
{
    if (settings == nullptr)
        return fail(error, "no settings to apply the proposals to");
    if (accepted.size() != proposals.size())
        return fail(error, "there are " + std::to_string(proposals.size()) +
                               " proposal(s) but " + std::to_string(accepted.size()) +
                               " acceptance flag(s); nothing was applied");

    // Build the whole result before touching the caller's settings, so a
    // failure part way through cannot leave a half-tuned profile.
    std::vector<CurrentSetting> updated = *settings;
    for (size_t index = 0; index < proposals.size(); ++index) {
        if (!accepted[index])
            continue;
        const TuningProposal &proposal = proposals[index];
        auto found = std::find_if(updated.begin(), updated.end(),
                                  [&](const CurrentSetting &setting) {
                                      return setting.setting == proposal.setting &&
                                             setting.tool == proposal.tool;
                                  });
        if (found == updated.end())
            return fail(error, "there is no setting '" + proposal.setting + "' on T" +
                                   std::to_string(proposal.tool) + " to apply; nothing was "
                                                                   "applied");
        found->value = proposal.proposed_value;
    }

    *settings = updated;
    return true;
}

} // namespace Slic3r
