///|/ Copyright (c) Prusa Research 2019 - 2023 Oleksandra Iushchenko @YuSanka, Tomáš Mészáros @tamasmeszaros, Filip Sykala @Jony01, Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
// Implementations moved verbatim from SLA/Hollowing.cpp; see DrainHole.hpp.
#include <libslic3r/SLA/DrainHole.hpp>

#include <cmath>

#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/libslic3r.h>

namespace Slic3r {
namespace sla {

indexed_triangle_set DrainHole::to_mesh() const
{
    auto r = double(radius);
    auto h = double(height);
    indexed_triangle_set hole = its_make_cylinder(r, h); //sla::cylinder(r, h, steps);
    Eigen::Quaternionf q;
    q.setFromTwoVectors(Vec3f::UnitZ(), normal);
    for(auto& p : hole.vertices) p = q * p + pos;

    return hole;
}

bool DrainHole::operator==(const DrainHole &sp) const
{
    return (pos == sp.pos) && (normal == sp.normal) &&
            is_approx(radius, sp.radius) &&
            is_approx(height, sp.height);
}

bool DrainHole::is_inside(const Vec3f& pt) const
{
    Eigen::Hyperplane<float, 3> plane(normal, pos);
    float dist = plane.signedDistance(pt);
    if (dist < float(EPSILON) || dist > height)
        return false;

    Eigen::ParametrizedLine<float, 3> axis(pos, normal);
    if ( axis.squaredDistance(pt) < pow(radius, 2.f))
        return true;

    return false;
}

// Given a line s+dir*t, find parameter t of intersections with the hole
// and the normal (points inside the hole). Outputs through out reference,
// returns true if two intersections were found.
bool DrainHole::get_intersections(const Vec3f& s, const Vec3f& dir,
                                  std::array<std::pair<float, Vec3d>, 2>& out)
                                  const
{
    assert(is_approx(normal.norm(), 1.f));
    const Eigen::ParametrizedLine<float, 3> ray(s, dir.normalized());

    for (size_t i=0; i<2; ++i)
        out[i] = std::make_pair(AABBMesh::hit_result::infty(), Vec3d::Zero());

    const float sqr_radius = pow(radius, 2.f);

    // first check a bounding sphere of the hole:
    Vec3f center = pos+normal*height/2.f;
    float sqr_dist_limit = pow(height/2.f, 2.f) + sqr_radius ;
    if (ray.squaredDistance(center) > sqr_dist_limit)
        return false;

    // The line intersects the bounding sphere, look for intersections with
    // bases of the cylinder.

    size_t found = 0; // counts how many intersections were found
    Eigen::Hyperplane<float, 3> base;
    if (! is_approx(ray.direction().dot(normal), 0.f)) {
        for (size_t i=1; i<=1; --i) {
            Vec3f cylinder_center = pos+i*height*normal;
            if (i == 0) {
                // The hole base can be identical to mesh surface if it is flat
                // let's better move the base outward a bit
                cylinder_center -= EPSILON*normal;
            }
            base = Eigen::Hyperplane<float, 3>(normal, cylinder_center);
            Vec3f intersection = ray.intersectionPoint(base);
            // Only accept the point if it is inside the cylinder base.
            if ((cylinder_center-intersection).squaredNorm() < sqr_radius) {
                out[found].first = ray.intersectionParameter(base);
                out[found].second = (i==0 ? 1. : -1.) * normal.cast<double>();
                ++found;
            }
        }
    }
    else
    {
        // In case the line was perpendicular to the cylinder axis, previous
        // block was skipped, but base will later be assumed to be valid.
        base = Eigen::Hyperplane<float, 3>(normal, pos-EPSILON*normal);
    }

    // In case there is still an intersection to be found, check the wall
    if (found != 2 && ! is_approx(std::abs(ray.direction().dot(normal)), 1.f)) {
        // Project the ray onto the base plane
        Vec3f proj_origin = base.projection(ray.origin());
        Vec3f proj_dir = base.projection(ray.origin()+ray.direction())-proj_origin;
        // save how the parameter scales and normalize the projected direction
        float par_scale = proj_dir.norm();
        proj_dir = proj_dir/par_scale;
        Eigen::ParametrizedLine<float, 3> projected_ray(proj_origin, proj_dir);
        // Calculate point on the secant that's closest to the center
        // and its distance to the circle along the projected line
        Vec3f closest = projected_ray.projection(pos);
        float dist = sqrt((sqr_radius - (closest-pos).squaredNorm()));
        // Unproject both intersections on the original line and check
        // they are on the cylinder and not past it:
        for (int i=-1; i<=1 && found !=2; i+=2) {
            Vec3f isect = closest + i*dist * projected_ray.direction();
            Vec3f to_isect = isect-proj_origin;
            float par = to_isect.norm() / par_scale;
            if (to_isect.normalized().dot(proj_dir.normalized()) < 0.f)
                par *= -1.f;
            Vec3d hit_normal = (pos-isect).normalized().cast<double>();
            isect = ray.pointAt(par);
            // check that the intersection is between the base planes:
            float vert_dist = base.signedDistance(isect);
            if (vert_dist > 0.f && vert_dist < height) {
                out[found].first = par;
                out[found].second = hit_normal;
                ++found;
            }
        }
    }

    // If only one intersection was found, it is some corner case,
    // no intersection will be returned:
    if (found != 2)
        return false;

    // Sort the intersections:
    if (out[0].first > out[1].first)
        std::swap(out[0], out[1]);

    return true;
}

DrainHoles transformed_drainhole_points(const ModelObject &mo,
                                        const Transform3d &trafo)
{
    auto pts = mo.sla_drain_holes;
//    const Transform3d& vol_trafo = mo.volumes.front()->get_transformation().get_matrix();
    const Geometry::Transformation trans(trafo /** vol_trafo*/);
    const Transform3d& tr = trans.get_matrix();
    for (sla::DrainHole &hl : pts) {
        Vec3d pos = hl.pos.cast<double>();
        Vec3d nrm = hl.normal.cast<double>();

        pos = tr * pos;
        nrm = tr * nrm - tr.translation();

        // Now shift the hole a bit above the object and make it deeper to
        // compensate for it. This is to avoid problems when the hole is placed
        // on (nearly) flat surface.
        pos -= nrm.normalized() * sla::HoleStickOutLength;

        hl.pos = pos.cast<float>();
        hl.normal = nrm.cast<float>();
        hl.height += sla::HoleStickOutLength;
    }

    return pts;
}

} // namespace sla
} // namespace Slic3r
