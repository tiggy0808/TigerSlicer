// Phase 14: telemetry-driven self-tuning. The tests are about what the import
// refuses to do: it never reads a document it does not fully understand, never
// claims an unidentified log describes this machine, never mixes an interrupted
// job into the evidence, never proposes on thin evidence, and never writes a
// setting the operator did not accept.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/TelemetryImport.hpp"

using namespace Slic3r;

namespace {

const char *const kDocument =
    "telemetry 1\n"
    "source klipper\n"
    "machine ratrig-vcore-tc\n"
    "manifest job-8891\n"
    "profile 4tool-abs-v3\n"
    "completion completed\n"
    "filament 41250.0\n"
    "event 0.000 job_start -1 -\n"
    "event 1.000 heat_start 0 240\n"
    "event 41.000 heat_reached 0 240\n"
    "event 100.000 toolchange_start 1 -\n"
    "event 112.000 toolchange_end 1 -\n"
    "event 112.500 purge_start 1 -\n"
    "event 118.000 purge_end 1 95.0\n"
    "event 200.000 toolchange_start 0 -\n"
    "event 214.000 toolchange_end 0 -\n"
    "event 300.000 toolchange_start 1 -\n"
    "event 302.000 toolchange_retry 1 -\n"
    "event 316.000 toolchange_end 1 -\n"
    "event 400.000 job_end -1 -\n";

TelemetryJob imported()
{
    TelemetryJob job;
    std::string  error;
    REQUIRE(parse_telemetry_document(kDocument, &job, &error));
    return job;
}

JobIdentity expected_identity()
{
    JobIdentity identity;
    identity.machine_id      = "ratrig-vcore-tc";
    identity.manifest_id     = "job-8891";
    identity.profile_version = "4tool-abs-v3";
    return identity;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("A telemetry document round-trips", "[telemetry-import]")
{
    const TelemetryJob job = imported();
    CHECK(job.source == TelemetrySource::KlipperLog);
    CHECK(job.identity.machine_id == "ratrig-vcore-tc");
    CHECK(job.completion == JobCompletion::Completed);
    REQUIRE(job.filament_used_mm3.has_value());
    CHECK(*job.filament_used_mm3 == Catch::Approx(41250.));
    CHECK(job.events.size() == 13);

    TelemetryJob again;
    std::string  error;
    REQUIRE(parse_telemetry_document(serialize_telemetry_document(job), &again, &error));
    CHECK(again.identity.machine_id == job.identity.machine_id);
    CHECK(again.events.size() == job.events.size());
    REQUIRE(again.identity.profile_version.has_value());
    CHECK(*again.identity.profile_version == "4tool-abs-v3");
}

TEST_CASE("An unrecorded field round-trips as unrecorded, not as a value",
          "[telemetry-import]")
{
    TelemetryJob job;
    std::string  error;
    REQUIRE(parse_telemetry_document("telemetry 1\nprofile -\nfilament -\n", &job, &error));
    CHECK_FALSE(job.identity.profile_version.has_value());
    CHECK_FALSE(job.filament_used_mm3.has_value());

    TelemetryJob again;
    REQUIRE(parse_telemetry_document(serialize_telemetry_document(job), &again, &error));
    CHECK_FALSE(again.identity.profile_version.has_value());
    CHECK_FALSE(again.filament_used_mm3.has_value());
}

TEST_CASE("A document this build does not understand is refused whole",
          "[telemetry-import]")
{
    TelemetryJob job;
    std::string  error;

    SECTION("a newer version") {
        CHECK_FALSE(parse_telemetry_document("telemetry 99\nsource klipper\n", &job, &error));
        CHECK(error.find("does not understand") != std::string::npos);
    }

    SECTION("an unknown event kind") {
        CHECK_FALSE(parse_telemetry_document(
            "telemetry 1\nevent 1.0 exploded 0 -\n", &job, &error));
        CHECK(error.find("unknown event kind") != std::string::npos);
    }

    SECTION("no version at all") {
        CHECK_FALSE(parse_telemetry_document("source klipper\n", &job, &error));
        CHECK(error.find("does not declare a telemetry version") != std::string::npos);
    }
}

TEST_CASE("Identity is matched, never assumed", "[telemetry-import]")
{
    const TelemetryJob job = imported();

    CHECK(match_identity(job.identity, expected_identity()) == IdentityMatch::Match);

    SECTION("a different machine") {
        JobIdentity other  = expected_identity();
        other.machine_id   = "voron-2.4";
        CHECK(match_identity(job.identity, other) == IdentityMatch::MachineMismatch);
    }

    SECTION("a different manifest") {
        JobIdentity other  = expected_identity();
        other.manifest_id  = "job-9000";
        CHECK(match_identity(job.identity, other) == IdentityMatch::ManifestMismatch);
    }

    SECTION("a different profile revision") {
        JobIdentity other      = expected_identity();
        other.profile_version  = "4tool-abs-v4";
        CHECK(match_identity(job.identity, other) == IdentityMatch::ProfileMismatch);
    }

    SECTION("a log that names no machine") {
        JobIdentity anonymous;
        anonymous.manifest_id = "job-8891";
        CHECK(match_identity(anonymous, expected_identity()) == IdentityMatch::Unidentified);
    }

    SECTION("a log that names no profile when one is expected") {
        JobIdentity partial   = expected_identity();
        partial.profile_version.reset();
        CHECK(match_identity(partial, expected_identity()) == IdentityMatch::Unidentified);
    }
}

TEST_CASE("Observations are extracted from what actually happened",
          "[telemetry-import]")
{
    const ObservationSet set = extract_observations(imported());

    const Observation *heat = set.find(kMetricHeatTimeS, 0);
    REQUIRE(heat != nullptr);
    CHECK(heat->mean == Catch::Approx(40.));
    CHECK(heat->samples == 1);

    const Observation *change = set.find(kMetricToolchangeTimeS, 1);
    REQUIRE(change != nullptr);
    // 12 s and 16 s.
    CHECK(change->mean == Catch::Approx(14.));
    CHECK(change->min == Catch::Approx(12.));
    CHECK(change->max == Catch::Approx(16.));
    CHECK(change->samples == 2);

    const Observation *purge = set.find(kMetricPurgeVolumeMm3, 1);
    REQUIRE(purge != nullptr);
    CHECK(purge->mean == Catch::Approx(95.));

    const Observation *retries = set.find(kMetricToolchangeRetries, 1);
    REQUIRE(retries != nullptr);
    CHECK(retries->mean == Catch::Approx(1.));

    // Nothing faulted or paused, so those metrics are absent rather than zero.
    CHECK(set.find(kMetricFaults, -1) == nullptr);
}

TEST_CASE("An operation that never finished contributes a note, not a timing",
          "[telemetry-import]")
{
    TelemetryJob job;
    std::string  error;
    REQUIRE(parse_telemetry_document("telemetry 1\n"
                                     "completion interrupted\n"
                                     "event 1.0 toolchange_start 2 -\n"
                                     "event 9.0 fault -1 - pickup timed out\n",
                                     &job, &error));

    const ObservationSet set = extract_observations(job);
    CHECK(set.find(kMetricToolchangeTimeS, 2) == nullptr);
    CHECK(mentions(set.notes, "never completed in this log"));
    CHECK(mentions(set.notes, "pickup timed out"));
    CHECK(mentions(set.notes, "describe a print that stopped"));
}

TEST_CASE("A purge with no recorded volume measures nothing", "[telemetry-import]")
{
    TelemetryJob job;
    std::string  error;
    REQUIRE(parse_telemetry_document("telemetry 1\n"
                                     "completion completed\n"
                                     "event 1.0 purge_start 0 -\n"
                                     "event 5.0 purge_end 0 -\n",
                                     &job, &error));

    const ObservationSet set = extract_observations(job);
    CHECK(set.find(kMetricPurgeVolumeMm3, 0) == nullptr);
    CHECK(mentions(set.notes, "recorded no volume"));
}

namespace {

ObservationSet measured(JobCompletion completion, double mean, size_t samples)
{
    ObservationSet set;
    set.completion = completion;
    Observation observation;
    observation.metric  = kMetricToolchangeTimeS;
    observation.tool    = 1;
    observation.mean    = mean;
    observation.min     = mean;
    observation.max     = mean;
    observation.samples = samples;
    set.observations.push_back(observation);
    return set;
}

std::vector<CurrentSetting> configured(double value)
{
    CurrentSetting setting;
    setting.setting = kMetricToolchangeTimeS;
    setting.tool    = 1;
    setting.value   = value;
    return { setting };
}

} // namespace

TEST_CASE("A change is proposed with the evidence behind it", "[telemetry-import]")
{
    const TuningReport report = propose_tuning(
        { measured(JobCompletion::Completed, 14., 12) }, configured(9.), TuningPolicy {});

    REQUIRE(report.proposals.size() == 1);
    const TuningProposal &proposal = report.proposals.front();
    CHECK(proposal.current_value == Catch::Approx(9.));
    CHECK(proposal.proposed_value == Catch::Approx(14.));
    CHECK(proposal.samples == 12);
    CHECK(proposal.confidence == Confidence::High);
    CHECK(mentions(proposal.evidence, "over 12 sample(s)"));
}

TEST_CASE("Confidence follows how much evidence there actually is",
          "[telemetry-import]")
{
    CHECK(propose_tuning({ measured(JobCompletion::Completed, 14., 3) }, configured(9.),
                         TuningPolicy {})
              .proposals.front()
              .confidence == Confidence::Low);
    CHECK(propose_tuning({ measured(JobCompletion::Completed, 14., 6) }, configured(9.),
                         TuningPolicy {})
              .proposals.front()
              .confidence == Confidence::Medium);
}

TEST_CASE("Thin evidence produces a note rather than a quiet proposal",
          "[telemetry-import]")
{
    const TuningReport report = propose_tuning(
        { measured(JobCompletion::Completed, 14., 2) }, configured(9.), TuningPolicy {});
    CHECK(report.proposals.empty());
    CHECK(mentions(report.notes, "below the 3 required"));
}

TEST_CASE("An interrupted job is never mixed into the evidence", "[telemetry-import]")
{
    const TuningReport report = propose_tuning(
        { measured(JobCompletion::Interrupted, 40., 20) }, configured(9.), TuningPolicy {});
    CHECK(report.proposals.empty());
    CHECK(mentions(report.excluded, "interrupted"));
    CHECK(mentions(report.notes, "no completed job was available"));
}

TEST_CASE("A value already close to what was measured is left alone",
          "[telemetry-import]")
{
    const TuningReport report = propose_tuning(
        { measured(JobCompletion::Completed, 14., 12) }, configured(13.5), TuningPolicy {});
    CHECK(report.proposals.empty());
    CHECK(mentions(report.notes, "already within"));
}

TEST_CASE("A setting nothing was measured for is left alone", "[telemetry-import]")
{
    CurrentSetting untouched;
    untouched.setting = kMetricPurgeVolumeMm3;
    untouched.tool    = 3;
    untouched.value   = 120.;

    const TuningReport report = propose_tuning(
        { measured(JobCompletion::Completed, 14., 12) }, { untouched }, TuningPolicy {});
    CHECK(report.proposals.empty());
    CHECK(mentions(report.notes, "nothing was measured"));
}

TEST_CASE("Only accepted proposals are applied", "[telemetry-import]")
{
    std::vector<CurrentSetting> settings = configured(9.);
    CurrentSetting              other;
    other.setting = kMetricPurgeVolumeMm3;
    other.tool    = 1;
    other.value   = 120.;
    settings.push_back(other);

    const TuningReport report = propose_tuning(
        { measured(JobCompletion::Completed, 14., 12) }, settings, TuningPolicy {});
    REQUIRE(report.proposals.size() == 1);

    std::string error;
    SECTION("declined") {
        REQUIRE(apply_tuning_proposals(report.proposals, { false }, &settings, &error));
        CHECK(settings[0].value == Catch::Approx(9.));
    }

    SECTION("accepted") {
        REQUIRE(apply_tuning_proposals(report.proposals, { true }, &settings, &error));
        CHECK(settings[0].value == Catch::Approx(14.));
        // Nothing else moved.
        CHECK(settings[1].value == Catch::Approx(120.));
    }
}

TEST_CASE("A mismatched acceptance list applies nothing at all", "[telemetry-import]")
{
    std::vector<CurrentSetting> settings = configured(9.);
    const TuningReport          report   = propose_tuning(
        { measured(JobCompletion::Completed, 14., 12) }, settings, TuningPolicy {});

    std::string error;
    CHECK_FALSE(apply_tuning_proposals(report.proposals, { true, true }, &settings, &error));
    CHECK(error.find("nothing was applied") != std::string::npos);
    CHECK(settings[0].value == Catch::Approx(9.));
}

TEST_CASE("A proposal with no matching setting applies nothing at all",
          "[telemetry-import]")
{
    TuningProposal orphan;
    orphan.setting        = "a_setting_that_does_not_exist";
    orphan.tool           = 7;
    orphan.proposed_value = 1.;

    std::vector<CurrentSetting> settings = configured(9.);
    std::string                 error;
    CHECK_FALSE(apply_tuning_proposals({ orphan }, { true }, &settings, &error));
    CHECK(error.find("nothing was ") != std::string::npos);
    CHECK(settings[0].value == Catch::Approx(9.));
}
