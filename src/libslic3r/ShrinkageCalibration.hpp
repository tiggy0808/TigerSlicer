#ifndef slic3r_ShrinkageCalibration_hpp_
#define slic3r_ShrinkageCalibration_hpp_

// TigerSlicer: shrinkage calibration (experimental). A printed part's size error has two parts: a proportional
// one (shrinkage, which grows with the size) and a constant one (bead width, over- or under-extrusion, the same at
// any size). One length cannot tell them apart; several lengths on the same axis can. A straight line fitted to
// measured against nominal lengths gives both: its slope is the scale, its intercept the constant offset.
//
// The test part has three lengths along X, three along Y and three heights in Z, two holes and a thin wall. Each
// length runs between faces that point opposite ways, so a constant offset of the walls shows up in the intercept.

#include "BoundingBox.hpp"
#include "TriangleMesh.hpp"

#include <array>
#include <optional>
#include <vector>

namespace Slic3r::shrinkage_calibration {

// Thresholds of the checks.
constexpr double kMaxDeviation        = 0.05; // a measurement further than 5 % from nominal is rejected
constexpr double kResidualLimitMm     = 0.05; // a fit further than this from a measurement: inconsistent
constexpr double kAnisotropyLimit     = 0.1;  // X and Y shrinkage differing by more percentage points: anisotropic
constexpr double kLargeOffsetMm       = 0.2;  // a constant offset (both sides together) larger than this: tune flow

// Least-squares straight line through (nominal, measured): measured = slope * nominal + intercept.
struct LinearFit
{
    bool   valid        = false; // at least two points with different nominals
    double slope        = 1.;
    double intercept    = 0.;
    double rms_residual = 0.;     // mm
    double max_residual = 0.;     // mm, largest distance of a measurement from the line
};
LinearFit fit_line(const std::vector<double> &nominal, const std::vector<double> &measured);

enum class Condition { AsPrinted, Annealed, MoistureConditioned };

// Nominal sizes of the test part, in mm. The arm lengths and heights shrink to fit a small printer.
struct Nominals
{
    std::array<double, 3> x{20., 60., 100.};
    std::array<double, 3> y{20., 60., 100.};
    std::array<double, 3> z{10., 20., 40.};
    double hole_small = 5.;
    double hole_large = 10.;
    double wall       = 2.;
};
// The nominals for a printer with this bed and height, scaled down when the full size does not fit.
// Empty when the bed is too small for the part.
std::optional<Nominals> nominals_for_bed(const BoundingBoxf &bed, double max_print_height);

struct Measurements
{
    std::array<double, 3> x{};
    std::array<double, 3> y{};
    std::array<double, 3> z{};
    double hole_small = 0.;
    double hole_large = 0.;
    double wall       = 0.;
};

// The measured values, in the order of Nominals: X 1-3, Y 1-3, Z 1-3, small hole, large hole, wall.
enum class Field { X1, X2, X3, Y1, Y2, Y3, Z1, Z2, Z3, HoleSmall, HoleLarge, Wall, Count };
double nominal(const Nominals &n, Field f);
double measured(const Measurements &m, Field f);
// Fields whose value is not positive or is further than kMaxDeviation from nominal.
std::vector<Field> out_of_range(const Nominals &n, const Measurements &m);

struct AxisResult
{
    LinearFit fit;
    double    percent      = 100.; // measured / nominal size from the slope, as filament shrinkage is entered
    bool      inconsistent = false; // max_residual > kResidualLimitMm
};

struct Result
{
    AxisResult x, y, z;
    double xy_percent  = 100.;  // mean of X and Y, the one XY value the filament has
    double anisotropy  = 0.;    // X minus Y, percentage points
    bool   anisotropic = false;
    double xy_offset   = 0.;    // mean constant offset of X and Y (both sides together), mm
    bool   large_offset = false;
    // Suggestions, not applied: per side, in the sense of the X-Y contour and hole compensation settings.
    double suggested_contour_compensation = 0.;
    double suggested_hole_compensation    = 0.;
    // Thin wall: measured minus what the X/Y fit predicts for it.
    double wall_expected = 0.;
    double wall_error    = 0.;
};
Result analyse(const Nominals &n, const Measurements &m);

// The filament shrinkage percentage to set, given the percentage the part was printed with (100 when
// compensation was off) and the measured percentage of the print.
double corrected_percent(double printed_with_percent, double measured_percent);

// The test part, its datum corner at the origin, standing on z = 0.
TriangleMesh make_test_part(const Nominals &n);

} // namespace Slic3r::shrinkage_calibration

#endif // slic3r_ShrinkageCalibration_hpp_
