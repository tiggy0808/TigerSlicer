// Phase 15: the calibration-print generator. A calibration print that cannot
// measure what it claims to measure is worse than none, so most of what is
// tested here is refusal: a degenerate sweep, a relative calibration with one
// tool, a capability the machine never declared, and above all a known-good
// result being replaced without anyone saying so.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/CalibrationPrints.hpp"

using namespace Slic3r;

namespace {

CalibrationCapabilities four_tool()
{
    CalibrationCapabilities capabilities;
    capabilities.tool_count              = 4;
    capabilities.can_change_tools        = true;
    capabilities.can_set_temperature     = true;
    capabilities.can_set_pressure_advance = true;
    capabilities.can_control_fan         = true;
    return capabilities;
}

CalibrationRequest flow_sweep()
{
    CalibrationRequest request;
    request.kind                       = CalibrationKind::MaximumFlow;
    request.tools                      = { 0 };
    request.start                      = 5.;
    request.end                        = 25.;
    request.step                       = 5.;
    request.conditions.material        = "ABS";
    request.conditions.nozzle_diameter_mm = 0.4;
    request.conditions.temperature_c   = 250.;
    return request;
}

CalibrationResult manual_result(double value, Confidence confidence)
{
    CalibrationResult result;
    result.kind                = CalibrationKind::MaximumFlow;
    result.tools               = { 0 };
    result.value               = value;
    result.source              = ResultSource::ManualObservation;
    result.confidence          = confidence;
    result.conditions.material = "ABS";
    return result;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("A sweep produces one labelled specimen per step", "[calibration-prints]")
{
    const CalibrationProject project = generate_calibration_project(flow_sweep(), four_tool());

    REQUIRE(project.ok());
    REQUIRE(project.specimens.size() == 5);
    CHECK(project.specimens.front().value == Catch::Approx(5.));
    CHECK(project.specimens.back().value == Catch::Approx(25.));
    // The label carries the unit, so a specimen is never a bare number.
    CHECK(project.specimens.front().label == "5.000 mm3/s");
    CHECK_FALSE(project.instructions.empty());
    CHECK(mentions(project.notes, "ABS"));
}

TEST_CASE("Conditions that were not recorded say so", "[calibration-prints]")
{
    CalibrationRequest request = flow_sweep();
    request.conditions         = CalibrationConditions {};

    const CalibrationProject project = generate_calibration_project(request, four_tool());
    REQUIRE(project.ok());
    CHECK(mentions(project.notes, "unrecorded material"));
    CHECK(mentions(project.notes, "will not be comparable"));
}

TEST_CASE("A degenerate sweep is refused, not clamped", "[calibration-prints]")
{
    SECTION("a zero step") {
        CalibrationRequest request = flow_sweep();
        request.step               = 0.;
        const CalibrationProject project = generate_calibration_project(request, four_tool());
        CHECK_FALSE(project.ok());
        CHECK(project.error.find("step must be positive") != std::string::npos);
        CHECK(project.specimens.empty());
    }

    SECTION("an end below the start") {
        CalibrationRequest request = flow_sweep();
        request.end                = 1.;
        const CalibrationProject project = generate_calibration_project(request, four_tool());
        CHECK_FALSE(project.ok());
        CHECK(project.error.find("below where it starts") != std::string::npos);
    }
}

TEST_CASE("An enormous sweep is capped and says it was capped", "[calibration-prints]")
{
    CalibrationRequest request = flow_sweep();
    request.end                = 10000.;
    request.step               = 1.;

    const CalibrationProject project = generate_calibration_project(request, four_tool());
    REQUIRE(project.ok());
    CHECK(project.specimens.size() == 200);
    CHECK(mentions(project.notes, "capped at 200 specimens"));
}

TEST_CASE("A relative calibration needs two tools", "[calibration-prints]")
{
    CalibrationRequest request;
    request.kind  = CalibrationKind::XyOffset;
    request.tools = { 0 };
    request.start = -0.2;
    request.end   = 0.2;
    request.step  = 0.02;

    const CalibrationProject project = generate_calibration_project(request, four_tool());
    CHECK_FALSE(project.ok());
    CHECK(project.error.find("measured between two tools") != std::string::npos);
}

TEST_CASE("A tool cannot be calibrated against itself", "[calibration-prints]")
{
    CalibrationRequest request;
    request.kind  = CalibrationKind::XyOffset;
    request.tools = { 1, 1 };
    request.start = 0.;
    request.end   = 0.1;
    request.step  = 0.05;

    const CalibrationProject project = generate_calibration_project(request, four_tool());
    CHECK_FALSE(project.ok());
    CHECK(project.error.find("cannot be calibrated against itself") != std::string::npos);
}

TEST_CASE("A tool the machine does not have is refused", "[calibration-prints]")
{
    CalibrationRequest request = flow_sweep();
    request.tools              = { 7 };

    const CalibrationProject project = generate_calibration_project(request, four_tool());
    CHECK_FALSE(project.ok());
    CHECK(project.error.find("T7 does not exist") != std::string::npos);
}

TEST_CASE("A calibration the machine cannot vary would print identical specimens",
          "[calibration-prints]")
{
    CalibrationCapabilities limited = four_tool();
    limited.can_set_pressure_advance = false;
    limited.can_control_fan          = false;
    limited.can_change_tools         = false;

    CalibrationRequest request = flow_sweep();
    request.step               = 0.005;
    request.start              = 0.;
    request.end                = 0.05;

    SECTION("pressure advance") {
        request.kind = CalibrationKind::PressureAdvance;
        const CalibrationProject project = generate_calibration_project(request, limited);
        CHECK_FALSE(project.ok());
        CHECK(project.error.find("identical specimens") != std::string::npos);
    }

    SECTION("cooling") {
        request.kind = CalibrationKind::Cooling;
        const CalibrationProject project = generate_calibration_project(request, limited);
        CHECK_FALSE(project.ok());
        CHECK(project.error.find("identical specimens") != std::string::npos);
    }

    SECTION("a two-tool calibration on a single-tool machine") {
        request.kind  = CalibrationKind::Interface;
        request.tools = { 0, 1 };
        const CalibrationProject project = generate_calibration_project(request, limited);
        CHECK_FALSE(project.ok());
        CHECK(project.error.find("does not declare toolchanging") != std::string::npos);
    }
}

TEST_CASE("Instructions name the tools they are about", "[calibration-prints]")
{
    CalibrationRequest request;
    request.kind  = CalibrationKind::ToolchangeReliability;
    request.tools = { 0, 2 };
    request.start = 50.;
    request.end   = 200.;
    request.step  = 50.;

    const CalibrationProject project = generate_calibration_project(request, four_tool());
    REQUIRE(project.ok());
    CHECK(mentions(project.instructions, "T0"));
    CHECK(mentions(project.instructions, "T2"));
    // Retries matter as much as outright failures.
    CHECK(mentions(project.instructions, "every retry"));
}

TEST_CASE("A first result is simply recorded", "[calibration-prints]")
{
    std::vector<CalibrationResult> store;
    std::string                    message;
    CHECK(record_calibration_result(manual_result(18., Confidence::Medium), false, &store,
                                    &message) == RecordOutcome::Recorded);
    REQUIRE(store.size() == 1);
    CHECK(store.front().value == Catch::Approx(18.));
}

TEST_CASE("A result nobody has confidence in is not stored at all",
          "[calibration-prints]")
{
    std::vector<CalibrationResult> store;
    std::string                    message;
    CHECK(record_calibration_result(manual_result(18., Confidence::Insufficient), false, &store,
                                    &message) == RecordOutcome::Rejected);
    CHECK(store.empty());
    CHECK(message.find("without being any") != std::string::npos);
}

TEST_CASE("A known-good result is never replaced without confirmation",
          "[calibration-prints]")
{
    std::vector<CalibrationResult> store;
    CalibrationResult              trusted = manual_result(18., Confidence::High);
    trusted.known_good                     = true;
    store.push_back(trusted);

    std::string message;
    const CalibrationResult replacement = manual_result(11., Confidence::High);

    CHECK(record_calibration_result(replacement, false, &store, &message) ==
          RecordOutcome::NeedsConfirmation);
    CHECK(store.front().value == Catch::Approx(18.));
    CHECK(message.find("confirm to replace") != std::string::npos);

    CHECK(record_calibration_result(replacement, true, &store, &message) ==
          RecordOutcome::Superseded);
    CHECK(store.front().value == Catch::Approx(11.));
}

TEST_CASE("An older result never displaces a newer one", "[calibration-prints]")
{
    std::vector<CalibrationResult> store;
    CalibrationResult              current = manual_result(18., Confidence::High);
    current.version                        = 3;
    store.push_back(current);

    CalibrationResult stale = manual_result(11., Confidence::High);
    stale.version           = 2;

    std::string message;
    CHECK(record_calibration_result(stale, false, &store, &message) == RecordOutcome::Rejected);
    CHECK(store.front().value == Catch::Approx(18.));
    CHECK(message.find("newer result") != std::string::npos);
}

TEST_CASE("A less confident result does not quietly win on arrival order",
          "[calibration-prints]")
{
    std::vector<CalibrationResult> store;
    store.push_back(manual_result(18., Confidence::High));

    std::string message;
    CHECK(record_calibration_result(manual_result(11., Confidence::Low), false, &store,
                                    &message) == RecordOutcome::NeedsConfirmation);
    CHECK(store.front().value == Catch::Approx(18.));
    CHECK(message.find("confirm to replace it anyway") != std::string::npos);
}

TEST_CASE("A result for different conditions is a different result",
          "[calibration-prints]")
{
    std::vector<CalibrationResult> store;
    store.push_back(manual_result(18., Confidence::High));

    CalibrationResult other      = manual_result(30., Confidence::High);
    other.conditions.material    = "PLA";

    std::string message;
    CHECK(record_calibration_result(other, false, &store, &message) == RecordOutcome::Recorded);
    CHECK(store.size() == 2);
}

namespace {

ObservationSet telemetry(JobCompletion completion, size_t samples)
{
    ObservationSet set;
    set.completion = completion;
    Observation observation;
    observation.metric  = kMetricPurgeVolumeMm3;
    observation.tool    = 1;
    observation.mean    = 95.;
    observation.min     = 90.;
    observation.max     = 101.;
    observation.samples = samples;
    set.observations.push_back(observation);
    return set;
}

} // namespace

TEST_CASE("Telemetry can stand in for a manual reading, at its own confidence",
          "[calibration-prints]")
{
    std::string reason;
    const std::optional<CalibrationResult> result =
        result_from_observation(telemetry(JobCompletion::Completed, 12), kMetricPurgeVolumeMm3,
                                1, CalibrationKind::Priming, TuningPolicy {}, &reason);

    REQUIRE(result.has_value());
    CHECK(result->value == Catch::Approx(95.));
    CHECK(result->source == ResultSource::Telemetry);
    CHECK(result->confidence == Confidence::High);
    CHECK(mentions(result->notes, "12 telemetry sample(s)"));
}

TEST_CASE("Telemetry from an unfinished or thin job produces no result",
          "[calibration-prints]")
{
    std::string reason;

    SECTION("interrupted") {
        CHECK_FALSE(result_from_observation(telemetry(JobCompletion::Interrupted, 20),
                                            kMetricPurgeVolumeMm3, 1, CalibrationKind::Priming,
                                            TuningPolicy {}, &reason)
                        .has_value());
        CHECK(reason.find("did not complete") != std::string::npos);
    }

    SECTION("too few samples") {
        CHECK_FALSE(result_from_observation(telemetry(JobCompletion::Completed, 1),
                                            kMetricPurgeVolumeMm3, 1, CalibrationKind::Priming,
                                            TuningPolicy {}, &reason)
                        .has_value());
        CHECK(reason.find("below the 3 required") != std::string::npos);
    }

    SECTION("a metric that was never measured") {
        CHECK_FALSE(result_from_observation(telemetry(JobCompletion::Completed, 20),
                                            kMetricHeatTimeS, 1, CalibrationKind::Temperature,
                                            TuningPolicy {}, &reason)
                        .has_value());
        CHECK(reason.find("nothing was measured") != std::string::npos);
    }
}

TEST_CASE("Every calibration kind names itself and round-trips", "[calibration-prints]")
{
    const CalibrationKind kinds[] = {
        CalibrationKind::MaximumFlow, CalibrationKind::PressureAdvance,
        CalibrationKind::Retraction,  CalibrationKind::StandbyOoze,
        CalibrationKind::Priming,     CalibrationKind::Temperature,
        CalibrationKind::XyOffset,    CalibrationKind::ZOffset,
        CalibrationKind::FirstLayer,  CalibrationKind::Bridging,
        CalibrationKind::Cooling,     CalibrationKind::Interface,
        CalibrationKind::ToolchangeReliability,
    };
    for (CalibrationKind kind : kinds) {
        CalibrationKind parsed = CalibrationKind::MaximumFlow;
        REQUIRE(parse_calibration_kind(calibration_kind_name(kind), &parsed));
        CHECK(parsed == kind);
        CHECK(std::string(calibration_kind_unit(kind)).size() > 0);
    }
    CHECK_FALSE(parse_calibration_kind("vibes", nullptr));
}
