#include "AutoSlice.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace auto_slice {

namespace {

constexpr double default_nozzle_diameter = 0.4;

double target_layer_ratio(Purpose purpose)
{
    switch (purpose) {
    case Purpose::Quality: return 0.3;
    case Purpose::Structural: return 0.5;
    case Purpose::Fast: return 0.7;
    }
    return 0.5;
}

// Low / Medium / High, from the Supports workspace's Light / Medium / Heavy presets.
const ResinSupportSettings resin_levels[] = {
    {0.8, 0.30, 2.0, 70, 0.10},
    {1.0, 0.40, 3.0, 100, 0.20},
    {1.4, 0.60, 3.5, 150, 0.30},
};

// A face counts as facing down when its normal is within about 45 degrees of straight down.
constexpr double downward_normal_z = -0.7;
// Holes are kept this much further apart than touching.
constexpr float hole_spacing_margin = 2.f;

} // namespace

std::vector<ProcessCandidate> process_candidates(const PresetCollection &prints)
{
    std::vector<ProcessCandidate> system, all;
    for (const Preset &preset : prints) {
        if (preset.is_default || !preset.is_visible || !preset.is_compatible || !preset.config.has("layer_height"))
            continue;
        ProcessCandidate candidate{preset.name, preset.config.opt_float("layer_height")};
        if (candidate.layer_height <= 0.)
            continue;
        if (preset.is_system)
            system.push_back(candidate);
        all.push_back(std::move(candidate));
    }
    return system.empty() ? all : system;
}

int pick_process(const std::vector<ProcessCandidate> &candidates, double nozzle_diameter, Purpose purpose)
{
    if (candidates.empty())
        return -1;
    const double nozzle = nozzle_diameter > 0. ? nozzle_diameter : default_nozzle_diameter;
    const double target = target_layer_ratio(purpose) * nozzle;

    auto is_strength = [](const ProcessCandidate &c) { return boost::algorithm::icontains(c.name, "strength"); };
    const bool strength_only = purpose == Purpose::Structural &&
                               std::any_of(candidates.begin(), candidates.end(), is_strength);

    int    best      = -1;
    double best_diff = std::numeric_limits<double>::max();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (strength_only && !is_strength(candidates[i]))
            continue;
        const double diff = std::abs(candidates[i].layer_height - target);
        // Strictly smaller keeps the first of equally near profiles.
        if (diff < best_diff - EPSILON) {
            best_diff = diff;
            best      = int(i);
        }
    }
    return best;
}

const ResinSupportSettings &resin_support_settings(SupportLevel level)
{
    switch (level) {
    case SupportLevel::Low: return resin_levels[0];
    case SupportLevel::Medium: return resin_levels[1];
    case SupportLevel::High: return resin_levels[2];
    }
    return resin_levels[1];
}

void apply_resin_support_settings(DynamicPrintConfig &config, SupportLevel level)
{
    const ResinSupportSettings &s = resin_support_settings(level);
    config.set_key_value("support_pillar_diameter", new ConfigOptionFloat(s.pillar_diameter));
    config.set_key_value("support_head_front_diameter", new ConfigOptionFloat(s.head_front_diameter));
    config.set_key_value("support_head_width", new ConfigOptionFloat(s.head_width));
    config.set_key_value("support_points_density_relative", new ConfigOptionInt(s.points_density));
    config.set_key_value("msla_support_contact_penetration", new ConfigOptionFloat(s.contact_penetration));
}

bool can_hollow(const indexed_triangle_set &mesh, const DynamicPrintConfig &config)
{
    if (mesh.indices.empty())
        return false;
    sla::HollowingConfig hollowing;
    if (config.has("hollowing_min_thickness"))
        hollowing.min_thickness = config.opt_float("hollowing_min_thickness");
    if (config.has("hollowing_quality"))
        hollowing.quality = config.opt_float("hollowing_quality");
    if (config.has("hollowing_closing_distance"))
        hollowing.closing_distance = config.opt_float("hollowing_closing_distance");
    const sla::InteriorPtr interior = sla::generate_interior(TriangleMesh(mesh), hollowing);
    return interior && !sla::get_mesh(*interior).empty();
}

sla::DrainHoles place_drain_holes(const indexed_triangle_set &mesh, const Transform3d &instance,
                                  float radius, float depth, size_t count)
{
    sla::DrainHoles holes;
    if (count == 0 || mesh.indices.empty())
        return holes;

    // A mirroring instance reverses the winding, so world normals computed from the transformed
    // vertices point inwards and must be flipped back.
    const double winding = instance.linear().determinant() < 0. ? -1. : 1.;

    struct Spot
    {
        Vec3d world;   // face centroid on the bed
        Vec3f local;   // face centroid in mesh coordinates
        Vec3f inward;  // into the model, mesh coordinates
        double area;
    };
    std::vector<Spot> spots;
    double            z_min = std::numeric_limits<double>::max();
    for (const stl_triangle_vertex_indices &face : mesh.indices) {
        const Vec3f &a = mesh.vertices[face(0)];
        const Vec3f &b = mesh.vertices[face(1)];
        const Vec3f &c = mesh.vertices[face(2)];
        const Vec3d  wa = instance * a.cast<double>();
        const Vec3d  wb = instance * b.cast<double>();
        const Vec3d  wc = instance * c.cast<double>();
        Vec3d        normal     = (wb - wa).cross(wc - wa);
        const double twice_area = normal.norm();
        const Vec3f  local_normal = (b - a).cross(c - a);
        if (twice_area < EPSILON || local_normal.squaredNorm() <= 0.f)
            continue;
        normal *= winding / twice_area;
        if (normal.z() > downward_normal_z)
            continue;
        const Vec3d centroid = (wa + wb + wc) / 3.;
        spots.push_back({centroid, (a + b + c) / 3.f, -local_normal.normalized(), 0.5 * twice_area});
        z_min = std::min(z_min, centroid.z());
    }
    if (spots.empty())
        return holes;

    // The lowest surfaces: those within a hole radius (at least 1 mm) of the very bottom.
    const double band_top = z_min + std::max(1.0, double(radius));
    spots.erase(std::remove_if(spots.begin(), spots.end(), [band_top](const Spot &s) { return s.world.z() > band_top; }),
                spots.end());

    // Keep away from the band's outline so a hole does not break through a side wall, unless
    // that leaves nothing (a small or curved bottom).
    BoundingBoxf band_box;
    Vec2d        weighted_centre = Vec2d::Zero();
    double       total_area      = 0.;
    for (const Spot &s : spots) {
        band_box.merge(Vec2d(s.world.x(), s.world.y()));
        weighted_centre += s.area * Vec2d(s.world.x(), s.world.y());
        total_area += s.area;
    }
    // The centroids sit inside their faces; widen the box to the faces' vertices.
    for (const stl_triangle_vertex_indices &face : mesh.indices)
        for (int k = 0; k < 3; ++k) {
            const Vec3d v = instance * mesh.vertices[face(k)].cast<double>();
            if (v.z() <= band_top)
                band_box.merge(Vec2d(v.x(), v.y()));
        }
    weighted_centre /= total_area;
    const double edge_clearance = double(radius) + 0.5;
    auto clear_of_edges = [&band_box, edge_clearance](const Spot &s) {
        return s.world.x() - band_box.min.x() >= edge_clearance && band_box.max.x() - s.world.x() >= edge_clearance &&
               s.world.y() - band_box.min.y() >= edge_clearance && band_box.max.y() - s.world.y() >= edge_clearance;
    };
    if (std::any_of(spots.begin(), spots.end(), clear_of_edges))
        spots.erase(std::remove_if(spots.begin(), spots.end(), [&](const Spot &s) { return !clear_of_edges(s); }),
                    spots.end());

    auto xy_distance = [](const Vec3d &p, const Vec3d &q) { return (Vec2d(p.x(), p.y()) - Vec2d(q.x(), q.y())).norm(); };

    std::vector<const Spot *> chosen;
    // First: the spot nearest the middle of the bottom.
    const Spot *first = &*std::min_element(spots.begin(), spots.end(), [&](const Spot &l, const Spot &r) {
        return (Vec2d(l.world.x(), l.world.y()) - weighted_centre).squaredNorm() <
               (Vec2d(r.world.x(), r.world.y()) - weighted_centre).squaredNorm();
    });
    chosen.push_back(first);
    // Then: each next spot as far as possible from those already chosen.
    const double min_gap = 2. * double(radius) + double(hole_spacing_margin);
    while (chosen.size() < count) {
        const Spot *best     = nullptr;
        double      best_gap = min_gap;
        for (const Spot &s : spots) {
            double gap = std::numeric_limits<double>::max();
            for (const Spot *c : chosen)
                gap = std::min(gap, xy_distance(s.world, c->world));
            if (gap >= best_gap) {
                best_gap = gap;
                best     = &s;
            }
        }
        if (best == nullptr)
            break;
        chosen.push_back(best);
    }

    for (const Spot *s : chosen)
        holes.emplace_back(s->local - s->inward * sla::HoleStickOutLength, s->inward, radius,
                           depth + sla::HoleStickOutLength);
    return holes;
}

} // namespace auto_slice
} // namespace Slic3r
