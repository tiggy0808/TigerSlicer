#include <catch2/catch_all.hpp>

#include "libslic3r/ShrinkageCalibration.hpp"

#include <cmath>

using namespace Slic3r;
using namespace Slic3r::shrinkage_calibration;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
const std::vector<double> kLengths{20., 60., 100.};

std::vector<double> measure(double scale, double offset, const std::vector<double> &noise = {0., 0., 0.})
{
    std::vector<double> out;
    for (size_t i = 0; i < kLengths.size(); ++i)
        out.push_back(scale * kLengths[i] + offset + noise[i]);
    return out;
}

// A print whose X shrinks by sx, Y by sy, Z by sz, with a constant offset (both sides together) on X and Y.
Measurements printed(const Nominals &n, double sx, double sy, double sz, double offset, double hole_error = 0., double wall_error = 0.)
{
    Measurements m;
    for (int i = 0; i < 3; ++i) {
        m.x[i] = sx * n.x[i] + offset;
        m.y[i] = sy * n.y[i] + offset;
        m.z[i] = sz * n.z[i];
    }
    const double s = (sx + sy) / 2.;
    m.hole_small = s * n.hole_small + hole_error;
    m.hole_large = s * n.hole_large + hole_error;
    m.wall       = s * n.wall + offset + wall_error;
    return m;
}
} // namespace

TEST_CASE("The fit separates a pure scale error", "[ShrinkageCalibration]")
{
    const LinearFit fit = fit_line(kLengths, measure(0.995, 0.));
    REQUIRE(fit.valid);
    CHECK_THAT(fit.slope, WithinAbs(0.995, 1e-12));
    CHECK_THAT(fit.intercept, WithinAbs(0., 1e-10));
    CHECK_THAT(fit.max_residual, WithinAbs(0., 1e-10));
}

TEST_CASE("The fit separates a pure constant offset", "[ShrinkageCalibration]")
{
    const LinearFit fit = fit_line(kLengths, measure(1., -0.12));
    CHECK_THAT(fit.slope, WithinAbs(1., 1e-12));
    CHECK_THAT(fit.intercept, WithinAbs(-0.12, 1e-10));
}

TEST_CASE("The fit separates scale and offset together", "[ShrinkageCalibration]")
{
    const LinearFit fit = fit_line(kLengths, measure(0.993, 0.08));
    CHECK_THAT(fit.slope, WithinAbs(0.993, 1e-12));
    CHECK_THAT(fit.intercept, WithinAbs(0.08, 1e-10));
}

TEST_CASE("Noisy measurements fit close to the truth and report their residual", "[ShrinkageCalibration]")
{
    // Caliper noise of 0.01 mm.
    const LinearFit fit = fit_line(kLengths, measure(0.994, 0.05, {0.01, -0.01, 0.01}));
    CHECK_THAT(fit.slope, WithinAbs(0.994, 0.0005));
    CHECK_THAT(fit.intercept, WithinAbs(0.05, 0.02));
    CHECK(fit.max_residual > 0.);
    CHECK(fit.max_residual <= kResidualLimitMm);

    // A misread middle step is inconsistent with the other two.
    Nominals         n;
    Measurements     m = printed(n, 0.994, 0.994, 0.998, 0.);
    m.x[1] += 0.3;
    const Result r = analyse(n, m);
    CHECK(r.x.inconsistent);
    CHECK_FALSE(r.y.inconsistent);
}

TEST_CASE("A fit needs two different nominal sizes", "[ShrinkageCalibration]")
{
    CHECK_FALSE(fit_line({20.}, {19.9}).valid);
    CHECK_FALSE(fit_line({20., 20.}, {19.9, 19.8}).valid);
}

TEST_CASE("Analysis reports shrinkage, offset and the XY mean per axis", "[ShrinkageCalibration]")
{
    const Nominals n;
    const Result   r = analyse(n, printed(n, 0.995, 0.993, 0.998, -0.1));
    CHECK_THAT(r.x.percent, WithinAbs(99.5, 1e-9));
    CHECK_THAT(r.y.percent, WithinAbs(99.3, 1e-9));
    CHECK_THAT(r.z.percent, WithinAbs(99.8, 1e-9));
    CHECK_THAT(r.xy_percent, WithinAbs(99.4, 1e-9));
    CHECK_THAT(r.x.fit.intercept, WithinAbs(-0.1, 1e-9));
    CHECK_THAT(r.xy_offset, WithinAbs(-0.1, 1e-9));
    // X and Y differ by 0.2 percentage points.
    CHECK(r.anisotropic);
    CHECK_THAT(r.anisotropy, WithinAbs(0.2, 1e-9));
    CHECK_FALSE(r.large_offset);
}

TEST_CASE("X and Y within 0.1 percentage points are not anisotropic", "[ShrinkageCalibration]")
{
    const Nominals n;
    CHECK_FALSE(analyse(n, printed(n, 0.9950, 0.9945, 1., 0.)).anisotropic);
    CHECK(analyse(n, printed(n, 0.9950, 0.9935, 1., 0.)).anisotropic);
}

TEST_CASE("Suggested compensation undoes the measured offsets", "[ShrinkageCalibration]")
{
    const Nominals n;
    // Parts 0.1 mm too wide (0.05 mm per side) and holes 0.2 mm too small.
    const Result r = analyse(n, printed(n, 1., 1., 1., 0.1, -0.2));
    CHECK_THAT(r.suggested_contour_compensation, WithinAbs(-0.05, 1e-9));
    CHECK_THAT(r.suggested_hole_compensation, WithinAbs(0.1, 1e-9));
    // The wall agrees with the X/Y fit.
    CHECK_THAT(r.wall_error, WithinAbs(0., 1e-9));
    CHECK_THAT(analyse(n, printed(n, 1., 1., 1., 0.1, 0., 0.15)).wall_error, WithinAbs(0.15, 1e-9));
}

TEST_CASE("A large constant offset is flagged for flow tuning", "[ShrinkageCalibration]")
{
    const Nominals n;
    CHECK(analyse(n, printed(n, 1., 1., 1., -0.3)).large_offset);
    CHECK_FALSE(analyse(n, printed(n, 1., 1., 1., -0.15)).large_offset);
}

TEST_CASE("Measurements more than 5 percent off nominal are rejected", "[ShrinkageCalibration]")
{
    const Nominals n;
    Measurements   m = printed(n, 0.995, 0.995, 0.998, 0.);
    CHECK(out_of_range(n, m).empty());
    m.x[2]       = 94.;  // 6 % short
    m.hole_large = 0.;   // not entered
    m.wall       = 2.09; // 4.5 % over: accepted
    const std::vector<Field> bad = out_of_range(n, m);
    REQUIRE(bad.size() == 2);
    CHECK(bad[0] == Field::X3);
    CHECK(bad[1] == Field::HoleLarge);
}

TEST_CASE("A verification print corrects the shrinkage it was printed with", "[ShrinkageCalibration]")
{
    // Printed with compensation off: the measured percentage is the material's.
    CHECK_THAT(corrected_percent(100., 99.4), WithinAbs(99.4, 1e-9));
    // Printed at 99.4 % and still 0.2 % small: the material shrinks a little more than that.
    CHECK_THAT(corrected_percent(99.4, 99.8), WithinAbs(99.2012, 1e-9));
    // Printed at the right value: unchanged.
    CHECK_THAT(corrected_percent(99.4, 100.), WithinAbs(99.4, 1e-9));
}

TEST_CASE("The test part keeps its full size on a large bed and shrinks to fit a small one", "[ShrinkageCalibration]")
{
    const auto big = nominals_for_bed(BoundingBoxf({0, 0}, {256, 256}), 256.);
    REQUIRE(big.has_value());
    CHECK_THAT(big->x[2], WithinAbs(100., 1e-9));
    CHECK_THAT(big->z[2], WithinAbs(40., 1e-9));

    const auto small = nominals_for_bed(BoundingBoxf({0, 0}, {90, 90}), 35.);
    REQUIRE(small.has_value());
    CHECK(small->x[2] <= 80.);
    CHECK(small->x[0] < small->x[1]);
    CHECK(small->z[2] <= 30.);
    // Holes and the wall keep their size.
    CHECK_THAT(small->hole_large, WithinAbs(10., 1e-9));

    CHECK_FALSE(nominals_for_bed(BoundingBoxf({0, 0}, {60, 60}), 100.).has_value());
}

TEST_CASE("The test part is a closed solid of the nominal size", "[ShrinkageCalibration]")
{
    const Nominals     n;
    const TriangleMesh part = make_test_part(n);
    REQUIRE(!part.empty());
    const BoundingBoxf3 bb = part.bounding_box();
    CHECK_THAT(bb.min.x(), WithinAbs(0., 1e-4));
    CHECK_THAT(bb.min.y(), WithinAbs(0., 1e-4));
    CHECK_THAT(bb.min.z(), WithinAbs(0., 1e-4));
    CHECK_THAT(bb.max.x(), WithinAbs(n.x[2], 1e-4));
    CHECK_THAT(bb.max.y(), WithinAbs(n.y[2], 1e-4));
    CHECK_THAT(bb.max.z(), WithinAbs(n.z[2], 1e-4));
    CHECK(part.stats().open_edges == 0);
    CHECK(TriangleMesh(part).volume() > 0.f);
}
