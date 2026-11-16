#include "ScrewHole.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <cmath>

namespace Slic3r::ModelTools {

const std::vector<ScrewSize> &screw_sizes()
{
    // Pilot: ISO tap drill. Clearance: ISO 273 normal (medium) series.
    static const std::vector<ScrewSize> sizes{
        {"M2", 1.6, 2.4},   {"M2.5", 2.05, 2.9}, {"M3", 2.5, 3.4}, {"M4", 3.3, 4.5},
        {"M5", 4.2, 5.5},   {"M6", 5.0, 6.6},    {"M8", 6.8, 9.0},
    };
    return sizes;
}

double hole_diameter(const ScrewSize &size, HoleFit fit) { return fit == HoleFit::ThreadIn ? size.pilot : size.clearance; }

std::string hole_name(const ScrewSize &size, HoleFit fit)
{
    return size.name + (fit == HoleFit::ThreadIn ? " thread-in hole" : " through hole");
}

indexed_triangle_set hole_mesh(const Vec3d &point, const Vec3d &normal, double diameter, double depth, double above)
{
    // Enough sides that a printed hole looks round at every screw size.
    constexpr int sides = 64;
    // Circumscribed polygon: its flats, not its corners, lie on the circle.
    const double radius = 0.5 * diameter / std::cos(M_PI / sides);
    // its_make_cylinder() takes ceil(2 pi / fa) sides: a step a little over 2 pi / sides keeps rounding from adding one.
    indexed_triangle_set its = its_make_cylinder(radius, depth + above, 2. * M_PI / (sides - 0.5));
    its_translate(its, Vec3f(0.f, 0.f, float(-depth)));

    const Vec3d axis = normal.normalized();
    Transform3d trafo = Transform3d::Identity();
    trafo.translate(point);
    trafo.rotate(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), axis));
    its_transform(its, trafo);
    return its;
}

} // namespace Slic3r::ModelTools
