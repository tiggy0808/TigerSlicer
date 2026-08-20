///|/ Copyright (c) Prusa Research 2019 - 2023 Tomáš Mészáros @tamasmeszaros, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
// Drain-hole model data retained for project compatibility and migration.
// Moved out of SLA/Hollowing.hpp so the legacy hollowing processor can leave
// the production build while stored project payloads keep loading.
#ifndef SLA_DRAINHOLE_HPP
#define SLA_DRAINHOLE_HPP

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "libslic3r/Point.hpp"

struct indexed_triangle_set;

namespace Slic3r {

class ModelObject;

namespace sla {

struct DrainHole
{
    Vec3f pos;
    Vec3f normal;
    float radius;
    float height;
    bool  failed = false;

    DrainHole()
        : pos(Vec3f::Zero()), normal(Vec3f::UnitZ()), radius(5.f), height(10.f)
    {}

    DrainHole(Vec3f p, Vec3f n, float r, float h, bool fl = false)
        : pos(p), normal(n), radius(r), height(h), failed(fl)
    {}

    DrainHole(const DrainHole& rhs) :
        DrainHole(rhs.pos, rhs.normal, rhs.radius, rhs.height, rhs.failed) {}

    bool operator==(const DrainHole &sp) const;

    bool operator!=(const DrainHole &sp) const { return !(sp == (*this)); }

    bool is_inside(const Vec3f& pt) const;

    bool get_intersections(const Vec3f& s, const Vec3f& dir,
                           std::array<std::pair<float, Vec3d>, 2>& out) const;

    indexed_triangle_set to_mesh() const;

    template<class Archive> inline void serialize(Archive &ar)
    {
        ar(pos, normal, radius, height, failed);
    }

    static constexpr size_t steps = 32;
};

using DrainHoles = std::vector<DrainHole>;

constexpr float HoleStickOutLength = 1.f;

sla::DrainHoles transformed_drainhole_points(const ModelObject &mo,
                                             const Transform3d &trafo);

} // namespace sla
} // namespace Slic3r

#endif // SLA_DRAINHOLE_HPP
