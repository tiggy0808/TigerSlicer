///|/ TigerSlicer Phase 14: telemetry-driven self-tuning.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_TelemetryImport_hpp_
#define slic3r_TelemetryImport_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Everything here is a local, offline import. No function in this header
// contacts a network, and nothing imported is transmitted anywhere.
enum class TelemetrySource : uint8_t
{
    KlipperLog,
    MoonrakerHistory,
    TigerSlicerManifest,
    Unknown,
};

const char *telemetry_source_name(TelemetrySource source);

// Whether the job that produced this telemetry actually finished. An
// interrupted job's timings describe a print that stopped, so they are kept
// but never mixed into the evidence for a proposal.
enum class JobCompletion : uint8_t
{
    Completed,
    Interrupted,
    Unknown,
};

const char *job_completion_name(JobCompletion completion);

struct JobIdentity
{
    std::string machine_id;
    std::string manifest_id;
    // Which profile revision produced the job. Absent means the log never
    // said, which is different from matching.
    std::optional<std::string> profile_version;
};

enum class TelemetryEventKind : uint8_t
{
    JobStart,
    JobEnd,
    HeatStart,
    HeatReached,
    ToolchangeStart,
    ToolchangeEnd,
    ToolchangeRetry,
    PurgeStart,
    PurgeEnd,
    Pause,
    Resume,
    Fault,
};

const char *telemetry_event_kind_name(TelemetryEventKind kind);
bool parse_telemetry_event_kind(const std::string &text, TelemetryEventKind *kind);

struct TelemetryEvent
{
    double             at_s { 0. };
    TelemetryEventKind kind { TelemetryEventKind::JobStart };
    // -1 when the event does not belong to a tool.
    int tool { -1 };
    // Numeric payload where the event carries one (a temperature, a purge
    // volume). Absent means the log did not record it.
    std::optional<double> value;
    std::string           detail;
};

struct TelemetryJob
{
    int             version { 0 };
    TelemetrySource source { TelemetrySource::Unknown };
    JobIdentity     identity;
    JobCompletion   completion { JobCompletion::Unknown };

    std::optional<double> filament_used_mm3;

    std::vector<TelemetryEvent> events;
};

// The document version this build understands. A document claiming a newer
// version is refused rather than partially read.
int telemetry_document_version();

// Parse one offline import. Returns false and sets `error` on anything it
// does not fully understand; a partially parsed job is never returned.
bool parse_telemetry_document(const std::string &text, TelemetryJob *job, std::string *error);

// Round-trips through parse_telemetry_document.
std::string serialize_telemetry_document(const TelemetryJob &job);

// --- identity --------------------------------------------------------------

enum class IdentityMatch : uint8_t
{
    Match,
    MachineMismatch,
    ManifestMismatch,
    ProfileMismatch,
    // The import did not carry enough identity to decide either way.
    Unidentified,
};

const char *identity_match_name(IdentityMatch match);

// Decide whether an import describes the machine and job we think it does.
// An import that cannot be identified is Unidentified, never an optimistic
// Match.
IdentityMatch match_identity(const JobIdentity &imported, const JobIdentity &expected);

// --- observations ----------------------------------------------------------

// One measured quantity, with how many samples stand behind it. `samples` is
// the honesty of the number: a mean over one event is reported as a mean over
// one event.
struct Observation
{
    std::string metric;
    int         tool { -1 };

    double mean { 0. };
    double min { 0. };
    double max { 0. };
    size_t samples { 0 };
};

struct ObservationSet
{
    int           version { 1 };
    JobIdentity   identity;
    JobCompletion completion { JobCompletion::Unknown };

    std::vector<Observation> observations;
    // Anything the extraction could not make sense of: an unpaired start, a
    // fault, a value the log never recorded.
    std::vector<std::string> notes;

    const Observation *find(const std::string &metric, int tool) const;
};

// Metric names produced by extract_observations.
extern const char *const kMetricHeatTimeS;
extern const char *const kMetricToolchangeTimeS;
extern const char *const kMetricToolchangeRetries;
extern const char *const kMetricPurgeVolumeMm3;
extern const char *const kMetricPauses;
extern const char *const kMetricFaults;

// Reduce a job's events to measured quantities. An operation that started and
// never ended contributes a note, not a timing.
ObservationSet extract_observations(const TelemetryJob &job);

// --- proposals -------------------------------------------------------------

struct CurrentSetting
{
    std::string setting;
    int         tool { -1 };
    double      value { 0. };
};

struct TuningPolicy
{
    // Below this many samples nothing is proposed, however far off the value
    // looks.
    size_t min_samples { 3 };
    // Ignore differences smaller than this fraction of the current value;
    // churning a profile for 1% is noise, not tuning.
    double min_relative_change { 0.10 };
};

enum class Confidence : uint8_t
{
    Insufficient,
    Low,
    Medium,
    High,
};

const char *confidence_name(Confidence confidence);

struct TuningProposal
{
    std::string setting;
    int         tool { -1 };

    double     current_value { 0. };
    double     proposed_value { 0. };
    size_t     samples { 0 };
    Confidence confidence { Confidence::Insufficient };

    // Why this change is being suggested, in terms of what was measured.
    std::vector<std::string> evidence;
};

struct TuningReport
{
    std::vector<TuningProposal> proposals;
    // Sets that were deliberately not used, and why.
    std::vector<std::string> excluded;
    std::vector<std::string> notes;
};

// Propose profile changes from imported evidence.
//
// Nothing here writes anything: proposals are returned for a human to accept.
// Interrupted jobs are excluded from the evidence, and a metric with too few
// samples produces a note rather than a low-confidence number.
TuningReport propose_tuning(const std::vector<ObservationSet> &sets,
                            const std::vector<CurrentSetting> &settings,
                            const TuningPolicy                &policy);

// Apply exactly the proposals the operator accepted.
//
// `accepted` must be the same length as `proposals`; a mismatch is an error
// rather than a partial application. Returns false and sets `error` without
// modifying anything.
bool apply_tuning_proposals(const std::vector<TuningProposal> &proposals,
                            const std::vector<bool>           &accepted,
                            std::vector<CurrentSetting>       *settings,
                            std::string                       *error);

} // namespace Slic3r

#endif // slic3r_TelemetryImport_hpp_
