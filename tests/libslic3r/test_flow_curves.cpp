// Phase 8: per-tool calibrated flow curves. The tests concentrate on the line
// between measurement and inference: the configured constant stays the
// default and the ceiling, interpolation happens only between measured
// points, and going outside the calibrated bounds either refuses or is
// flagged as extrapolated - it never quietly becomes an answer.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include "libslic3r/FlowCurves.hpp"

using namespace Slic3r;

namespace {

FlowCurve curve(int tool = 0)
{
    FlowCurve value;
    value.tool               = tool;
    value.nozzle_diameter_mm = 0.4;
    value.material           = "PLA";
    value.samples            = { { 200., 10. }, { 220., 14. }, { 240., 16. } };
    value.layer_height_mm    = { 0.1, 0.3 };
    value.extrusion_width_mm = { 0.35, 0.5 };
    return value;
}

FlowQuery query(double temperature = 220.)
{
    FlowQuery value;
    value.tool                 = 0;
    value.nozzle_diameter_mm   = 0.4;
    value.material             = "PLA";
    value.temperature_c        = temperature;
    value.layer_height_mm      = 0.2;
    value.extrusion_width_mm   = 0.42;
    value.feature              = MultiRateFeature::Walls;
    return value;
}

FlowOptions options(double constant = 25., bool extrapolate = false)
{
    FlowOptions value;
    value.constant_max_volumetric_speed_mm3_s = constant;
    value.allow_extrapolation                 = extrapolate;
    return value;
}

} // namespace

TEST_CASE("With no calibration the configured constant applies", "[flow-curves]")
{
    const FlowResult result = resolve_flow_limit(query(), {}, options(25.));

    CHECK(result.max_flow_mm3_s == Catch::Approx(25.));
    CHECK(result.source == FlowSource::ConstantMvs);
    CHECK(result.limit == FlowLimit::NoCalibrationForTool);
    CHECK(result.explanation.find("no calibration matches") != std::string::npos);
}

TEST_CASE("A measured point is used exactly, without interpolation",
          "[flow-curves]")
{
    const FlowResult result = resolve_flow_limit(query(220.), { curve() }, options(25.));

    CHECK(result.max_flow_mm3_s == Catch::Approx(14.));
    CHECK(result.source == FlowSource::CalibratedExact);
    CHECK(result.limit == FlowLimit::CalibratedCurve);
    CHECK_FALSE(result.extrapolated);
}

TEST_CASE("Between measured points the curve is interpolated", "[flow-curves]")
{
    // Halfway between 200 C / 10 and 220 C / 14.
    const FlowResult result = resolve_flow_limit(query(210.), { curve() }, options(25.));

    CHECK(result.max_flow_mm3_s == Catch::Approx(12.));
    CHECK(result.source == FlowSource::CalibratedInterpolated);
    CHECK(result.explanation.find("interpolated between") != std::string::npos);
    CHECK_FALSE(result.extrapolated);
}

TEST_CASE("Outside the calibrated temperatures the curve is refused by default",
          "[flow-curves]")
{
    const FlowResult result = resolve_flow_limit(query(260.), { curve() }, options(25.));

    // The constant applies unchanged; the curve contributes nothing.
    CHECK(result.max_flow_mm3_s == Catch::Approx(25.));
    CHECK(result.source == FlowSource::ConstantMvs);
    CHECK(result.limit == FlowLimit::OutsideCalibratedTemperature);
    CHECK(result.explanation.find("extrapolation is not enabled") != std::string::npos);
    CHECK_FALSE(result.extrapolated);
}

TEST_CASE("Extrapolation happens only when explicitly allowed, and is flagged",
          "[flow-curves]")
{
    const FlowResult result = resolve_flow_limit(query(260.), { curve() }, options(25., true));

    // 220 -> 240 rises 2 per 20 degrees, so 260 extrapolates to 18.
    CHECK(result.max_flow_mm3_s == Catch::Approx(18.));
    CHECK(result.source == FlowSource::CalibratedExtrapolated);
    CHECK(result.extrapolated);
    CHECK(result.explanation.find("extrapolated, not measured") != std::string::npos);
}

TEST_CASE("A single sample cannot be extrapolated from", "[flow-curves]")
{
    FlowCurve lonely = curve();
    lonely.samples   = { { 220., 14. } };

    SECTION("exactly on the sample it still works") {
        const FlowResult result = resolve_flow_limit(query(220.), { lonely }, options(25., true));
        CHECK(result.max_flow_mm3_s == Catch::Approx(14.));
        CHECK(result.source == FlowSource::CalibratedExact);
    }

    SECTION("anywhere else it refuses even with extrapolation allowed") {
        const FlowResult result = resolve_flow_limit(query(230.), { lonely }, options(25., true));
        CHECK(result.source == FlowSource::ConstantMvs);
        CHECK(result.explanation.find("single calibrated sample") != std::string::npos);
    }
}

TEST_CASE("A calibration outside its layer-height bounds does not apply",
          "[flow-curves]")
{
    FlowQuery thick = query(220.);
    thick.layer_height_mm = 0.55;   // the curve was measured from 0.1 to 0.3

    const FlowResult result = resolve_flow_limit(thick, { curve() }, options(25.));
    CHECK(result.max_flow_mm3_s == Catch::Approx(25.));
    CHECK(result.limit == FlowLimit::OutsideCalibratedLayerHeight);
    CHECK(result.explanation.find("but not at") != std::string::npos);
}

TEST_CASE("A calibration outside its width bounds does not apply", "[flow-curves]")
{
    FlowQuery wide = query(220.);
    wide.extrusion_width_mm = 0.9;

    const FlowResult result = resolve_flow_limit(wide, { curve() }, options(25.));
    CHECK(result.limit == FlowLimit::OutsideCalibratedWidth);
    CHECK(result.source == FlowSource::ConstantMvs);
}

TEST_CASE("The configured constant remains the ceiling", "[flow-curves]")
{
    // The tool measured 14 mm3/s, but the profile only permits 8.
    const FlowResult result = resolve_flow_limit(query(220.), { curve() }, options(8.));

    CHECK(result.max_flow_mm3_s == Catch::Approx(8.));
    CHECK(result.source == FlowSource::ConstantMvs);
    CHECK(result.explanation.find("remains the ceiling") != std::string::npos);
}

TEST_CASE("A curve for another tool, nozzle or material does not apply",
          "[flow-curves]")
{
    SECTION("another tool") {
        FlowQuery other = query(220.);
        other.tool = 1;
        CHECK(resolve_flow_limit(other, { curve(0) }, options(25.)).limit ==
              FlowLimit::NoCalibrationForTool);
    }

    SECTION("another nozzle") {
        FlowQuery other = query(220.);
        other.nozzle_diameter_mm = 0.6;
        CHECK(resolve_flow_limit(other, { curve() }, options(25.)).limit ==
              FlowLimit::NoCalibrationForTool);
    }

    SECTION("another material") {
        FlowQuery other = query(220.);
        other.material = "PETG";
        CHECK(resolve_flow_limit(other, { curve() }, options(25.)).limit ==
              FlowLimit::NoCalibrationForTool);
    }
}

TEST_CASE("A feature-restricted calibration applies only to that feature",
          "[flow-curves]")
{
    FlowCurve bridges = curve();
    bridges.features  = { MultiRateFeature::SolidInfill };

    SECTION("the named feature uses it") {
        FlowQuery solid = query(220.);
        solid.feature = MultiRateFeature::SolidInfill;
        CHECK(resolve_flow_limit(solid, { bridges }, options(25.)).limit ==
              FlowLimit::CalibratedCurve);
    }

    SECTION("any other feature does not") {
        CHECK(resolve_flow_limit(query(220.), { bridges }, options(25.)).limit ==
              FlowLimit::NoCalibrationForTool);
    }
}

TEST_CASE("The most restrictive applicable calibration wins", "[flow-curves]")
{
    FlowCurve strict = curve();
    strict.samples   = { { 200., 6. }, { 240., 9. } };

    const FlowResult result =
        resolve_flow_limit(query(220.), { curve(), strict }, options(25.));
    // 7.5 from the strict curve rather than 14 from the permissive one.
    CHECK(result.max_flow_mm3_s == Catch::Approx(7.5));
}

TEST_CASE("Utilization is reported and an over-limit extrusion is flagged",
          "[flow-curves]")
{
    FlowQuery busy = query(220.);
    busy.requested_flow_mm3_s = 7.;

    const FlowResult within = resolve_flow_limit(busy, { curve() }, options(25.));
    CHECK(within.utilization == Catch::Approx(0.5));   // 7 of the calibrated 14
    CHECK_FALSE(within.exceeds_limit);

    busy.requested_flow_mm3_s = 20.;
    const FlowResult over = resolve_flow_limit(busy, { curve() }, options(25.));
    CHECK(over.utilization == Catch::Approx(20. / 14.));
    CHECK(over.exceeds_limit);
}

TEST_CASE("Every result explains its limiting reason", "[flow-curves]")
{
    CHECK_FALSE(resolve_flow_limit(query(), {}, options()).explanation.empty());
    CHECK_FALSE(resolve_flow_limit(query(220.), { curve() }, options()).explanation.empty());
    CHECK_FALSE(resolve_flow_limit(query(260.), { curve() }, options()).explanation.empty());
}

TEST_CASE("Malformed calibrations are reported", "[flow-curves]")
{
    FlowCurve empty = curve();
    empty.samples.clear();

    FlowCurve duplicated = curve();
    duplicated.samples = { { 220., 14. }, { 220., 12. } };

    FlowCurve negative = curve();
    negative.samples = { { 200., -1. }, { 220., 14. } };

    FlowCurve inverted = curve();
    inverted.layer_height_mm = { 0.3, 0.1 };

    const std::vector<std::string> problems =
        validate_flow_curves({ empty, duplicated, negative, inverted });
    CHECK(problems.size() == 4);
    CHECK(validate_flow_curves({ curve() }).empty());
}
