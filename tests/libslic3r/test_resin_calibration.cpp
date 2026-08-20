///|/ TigerSlicer Phase UV6 groundwork: resin calibration generation and gating.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/ResinCalibration.hpp"

using namespace Slic3r;

namespace {

CapabilityClaim claim(const std::string &field, const std::string &value, EvidenceLevel evidence)
{
    CapabilityClaim c;
    c.field    = field;
    c.value    = value;
    c.source   = CapabilitySource::VendorSpec;
    c.evidence = evidence;
    return c;
}

ResinCalibrationRequest request(ResinCalibrationKind kind,
                                const std::vector<CapabilityClaim> &claims)
{
    ResinCalibrationRequest req;
    req.kind                = kind;
    req.machine             = build_registry(claims);
    req.machine_vendor      = "Elegoo";
    req.machine_model       = "Saturn";
    req.machine_firmware    = "1.0";
    req.mslicer_version     = "3.0";
    req.tigerslicer_version = "2.9.4";
    req.uvtools_version     = "5.0.4";
    req.generated_s         = 1700000000;
    return req;
}

const CalibrationRange *range_for(const ResinCalibrationPlan &plan, const std::string &param)
{
    for (const CalibrationRange &r : plan.ranges)
        if (r.parameter == param)
            return &r;
    return nullptr;
}

} // namespace

TEST_CASE("UV6: the catalog describes each test and what it measures", "[resin-calibration]")
{
    CHECK(resin_calibration_catalog().size() == 6);
    for (const ResinCalibrationTest &test : resin_calibration_catalog())
        CHECK_FALSE(test.measures.empty());
    REQUIRE(find_calibration_test(ResinCalibrationKind::Lift) != nullptr);
    // The lift test sweeps two parameters.
    CHECK(find_calibration_test(ResinCalibrationKind::Lift)->ranges.size() == 2);
}

TEST_CASE("UV6: a test with no special requirement is ready and carries the disclaimer",
          "[resin-calibration]")
{
    const ResinCalibrationPlan plan =
        plan_resin_calibration(request(ResinCalibrationKind::ElephantFoot, {}));
    CHECK(plan.ready());
    // The disclaimer is present and names the things a calibration is not safe
    // for; it is never omitted.
    CHECK_FALSE(plan.disclaimer.empty());
    CHECK_THAT(plan.disclaimer, Catch::Matchers::ContainsSubstring("resin"));
    CHECK_THAT(plan.disclaimer, Catch::Matchers::ContainsSubstring("firmware"));
    // Provenance is complete enough to reproduce the result.
    CHECK(plan.provenance.machine_model == "Saturn");
    CHECK(plan.provenance.uvtools_version == "5.0.4");
    CHECK_FALSE(plan.provenance.ranges.empty());
}

TEST_CASE("UV6: an advanced per-layer setting is gated on confirmed support, not a guess",
          "[resin-calibration]")
{
    SECTION("no per-layer exposure support at all is unsupported")
    {
        const ResinCalibrationPlan plan =
            plan_resin_calibration(request(ResinCalibrationKind::Exposure, {}));
        CHECK(plan.verdict == CalibrationVerdict::Unsupported);
        CHECK_FALSE(plan.unmet.empty());
    }

    SECTION("per-layer exposure known only by weak inference needs confirmed support")
    {
        const ResinCalibrationPlan plan = plan_resin_calibration(request(
            ResinCalibrationKind::Exposure,
            { claim("per_layer_exposure", "yes", EvidenceLevel::InferredFromFormatFamily) }));
        CHECK(plan.verdict == CalibrationVerdict::NeedsConfirmedSupport);
        CHECK_THAT(plan.unmet.front(),
                   Catch::Matchers::ContainsSubstring("not confirmed"));
    }

    SECTION("per-layer exposure confirmed by vendor spec is ready")
    {
        const ResinCalibrationPlan plan = plan_resin_calibration(request(
            ResinCalibrationKind::Exposure,
            { claim("per_layer_exposure", "yes", EvidenceLevel::ConfirmedByVendorSpec) }));
        CHECK(plan.ready());
    }

    SECTION("confirmed on hardware is likewise enough")
    {
        const ResinCalibrationPlan plan = plan_resin_calibration(request(
            ResinCalibrationKind::GrayscaleBlooming,
            { claim("light_pwm", "yes", EvidenceLevel::ConfirmedOnHardware) }));
        CHECK(plan.ready());
    }

    SECTION("a conflicted capability is not treated as present")
    {
        std::vector<CapabilityClaim> claims = {
            claim("per_layer_exposure", "yes", EvidenceLevel::ConfirmedByVendorSpec),
            claim("per_layer_exposure", "no", EvidenceLevel::ConfirmedByVendorSpec),
        };
        const ResinCalibrationPlan plan =
            plan_resin_calibration(request(ResinCalibrationKind::Exposure, claims));
        CHECK(plan.verdict == CalibrationVerdict::Unsupported);
    }
}

TEST_CASE("UV6: a range override outside the safe bounds is clamped, not obeyed",
          "[resin-calibration]")
{
    ResinCalibrationRequest req = request(ResinCalibrationKind::ElephantFoot, {});
    CalibrationRange         reckless;
    reckless.parameter = "compensation";
    reckless.minimum   = -1.0;  // below safe 0.0
    reckless.maximum   = 5.0;   // above safe 0.3
    reckless.step      = 0.05;
    req.range_overrides["compensation"] = reckless;

    const ResinCalibrationPlan plan = plan_resin_calibration(req);
    const CalibrationRange *range = range_for(plan, "compensation");
    REQUIRE(range != nullptr);
    CHECK(range->minimum == 0.0);
    CHECK(range->maximum == 0.3);
    bool noted = false;
    for (const std::string &note : plan.unmet)
        if (note.find("clamped") != std::string::npos)
            noted = true;
    CHECK(noted);
}

TEST_CASE("UV6: a calibration never destructively replaces a user model", "[resin-calibration]")
{
    SECTION("a new path is allowed without confirmation")
    {
        CalibrationSaveTarget target;
        target.path   = "calib-exposure.tsp";
        target.exists = false;
        const CalibrationSaveDecision decision =
            evaluate_calibration_save(target, ProjectAction::CreateNew);
        CHECK(decision.may_write_without_confirmation());
    }

    SECTION("replacing an existing calibration project needs confirmation")
    {
        CalibrationSaveTarget target;
        target.path          = "calib-exposure.tsp";
        target.exists        = true;
        target.is_user_model = false;
        const CalibrationSaveDecision decision =
            evaluate_calibration_save(target, ProjectAction::Replace);
        CHECK(decision.verdict == SaveVerdict::NeedsConfirmation);
    }

    SECTION("overwriting a user model is refused outright, confirmation or not")
    {
        CalibrationSaveTarget target;
        target.path          = "my-dragon.tsp";
        target.exists        = true;
        target.is_user_model = true;
        // Even asked to replace, it is refused: a calibration cannot destroy a
        // user's work.
        const CalibrationSaveDecision decision =
            evaluate_calibration_save(target, ProjectAction::Replace);
        CHECK(decision.verdict == SaveVerdict::Refused);
        CHECK_THAT(decision.message, Catch::Matchers::ContainsSubstring("cannot overwrite"));
    }
}

TEST_CASE("UV6: the same request plans the same way every time", "[resin-calibration]")
{
    const ResinCalibrationRequest req = request(
        ResinCalibrationKind::Exposure,
        { claim("per_layer_exposure", "yes", EvidenceLevel::ConfirmedByVendorSpec) });
    const ResinCalibrationPlan a = plan_resin_calibration(req);
    const ResinCalibrationPlan b = plan_resin_calibration(req);
    CHECK(a.verdict == b.verdict);
    REQUIRE(a.ranges.size() == b.ranges.size());
    for (size_t i = 0; i < a.ranges.size(); ++i) {
        CHECK(a.ranges[i].parameter == b.ranges[i].parameter);
        CHECK(a.ranges[i].minimum == b.ranges[i].minimum);
    }
    CHECK(a.disclaimer == b.disclaimer);
}
