///|/ TigerSlicer Phase 10: swept-volume collision simulation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SweptCollision.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

namespace {
constexpr double COLLISION_EPSILON = 1e-9;
}

Aabb Aabb::of(double min_x, double min_y, double min_z,
              double max_x, double max_y, double max_z)
{
    Aabb box;
    box.valid = true;
    box.min_x = std::min(min_x, max_x);
    box.min_y = std::min(min_y, max_y);
    box.min_z = std::min(min_z, max_z);
    box.max_x = std::max(min_x, max_x);
    box.max_y = std::max(min_y, max_y);
    box.max_z = std::max(min_z, max_z);
    return box;
}

bool Aabb::intersects(const Aabb &other, double tolerance) const
{
    if (!valid || !other.valid)
        return false;
    return min_x - tolerance < other.max_x && max_x + tolerance > other.min_x &&
           min_y - tolerance < other.max_y && max_y + tolerance > other.min_y &&
           min_z - tolerance < other.max_z && max_z + tolerance > other.min_z;
}

Aabb Aabb::translated(double dx, double dy, double dz) const
{
    if (!valid)
        return *this;
    return Aabb::of(min_x + dx, min_y + dy, min_z + dz,
                    max_x + dx, max_y + dy, max_z + dz);
}

Aabb Aabb::merged(const Aabb &other) const
{
    if (!valid)
        return other;
    if (!other.valid)
        return *this;
    return Aabb::of(std::min(min_x, other.min_x), std::min(min_y, other.min_y),
                    std::min(min_z, other.min_z), std::max(max_x, other.max_x),
                    std::max(max_y, other.max_y), std::max(max_z, other.max_z));
}

const char *obstacle_kind_name(ObstacleKind kind)
{
    switch (kind) {
    case ObstacleKind::Dock:          return "dock";
    case ObstacleKind::ParkedTool:    return "parked tool";
    case ObstacleKind::Brush:         return "brush";
    case ObstacleKind::PurgeHardware: return "purge hardware";
    case ObstacleKind::Fixture:       return "fixture";
    }
    return "unknown";
}

const char *motion_kind_name(MotionKind kind)
{
    switch (kind) {
    case MotionKind::Print:        return "print move";
    case MotionKind::Travel:       return "travel";
    case MotionKind::DownwardZ:    return "downward move";
    case MotionKind::DockApproach: return "dock approach";
    case MotionKind::DockExit:     return "dock exit";
    }
    return "unknown";
}

const char *collision_verdict_name(CollisionVerdict verdict)
{
    switch (verdict) {
    case CollisionVerdict::Clear:     return "clear";
    case CollisionVerdict::Collision: return "collision";
    case CollisionVerdict::Unproven:  return "unproven";
    }
    return "unknown";
}

std::optional<double> PrintedHeightField::highest_in(double min_x, double min_y,
                                                     double max_x, double max_y,
                                                     bool *any_unknown) const
{
    if (any_unknown != nullptr)
        *any_unknown = false;
    if (empty() || cell_mm <= 0.)
        return std::nullopt;

    const double span_x = origin_x + double(nx) * cell_mm;
    const double span_y = origin_y + double(ny) * cell_mm;
    // A footprint entirely outside the field touches no printed geometry.
    if (max_x < origin_x || min_x > span_x || max_y < origin_y || min_y > span_y)
        return std::nullopt;

    const long first_x = long(std::floor((std::max(min_x, origin_x) - origin_x) / cell_mm));
    const long first_y = long(std::floor((std::max(min_y, origin_y) - origin_y) / cell_mm));
    const long last_x  = long(std::floor((std::min(max_x, span_x) - origin_x) / cell_mm));
    const long last_y  = long(std::floor((std::min(max_y, span_y) - origin_y) / cell_mm));

    std::optional<double> highest;
    for (long iy = std::max(0L, first_y); iy <= last_y && size_t(iy) < ny; ++iy)
        for (long ix = std::max(0L, first_x); ix <= last_x && size_t(ix) < nx; ++ix) {
            const size_t index = size_t(iy) * nx + size_t(ix);
            if (index >= height_mm.size() || !height_mm[index].has_value()) {
                // An undetermined column cannot be cleared.
                if (any_unknown != nullptr)
                    *any_unknown = true;
                continue;
            }
            const double value = *height_mm[index];
            if (!highest.has_value() || value > *highest)
                highest = value;
        }
    return highest;
}

namespace {

const ToolheadShape *shape_for(const std::vector<ToolheadShape> &shapes, int tool)
{
    for (const ToolheadShape &shape : shapes)
        if (shape.tool == tool)
            return &shape;
    return nullptr;
}

// A tool is allowed to touch its own dock, but only while it is docking.
bool ignored_for(const Obstacle &obstacle, const MotionSegment &segment)
{
    if (segment.kind != MotionKind::DockApproach && segment.kind != MotionKind::DockExit)
        return false;
    if (obstacle.kind != ObstacleKind::Dock && obstacle.kind != ObstacleKind::ParkedTool)
        return false;
    return obstacle.owning_tool.has_value() && *obstacle.owning_tool == segment.tool;
}

std::string coordinate(double x, double y, double z)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "(" << x << ", " << y << ", " << z << ")";
    return out.str();
}

} // namespace

CollisionReport simulate_collisions(const std::vector<MotionSegment> &segments,
                                    const std::vector<ToolheadShape> &shapes,
                                    const std::vector<Obstacle>      &obstacles,
                                    const PrintedHeightField         &printed,
                                    const CollisionOptions           &options)
{
    CollisionReport report;

    if (segments.empty()) {
        report.error = "no motion was supplied";
        return report;
    }
    if (options.sample_step_mm <= 0.) {
        report.error = "the precise sampling step must be positive";
        return report;
    }

    bool saw_collision = false;
    bool saw_unproven  = false;
    bool truncated     = false;

    auto add = [&](CollisionFinding finding) {
        if (finding.verdict == CollisionVerdict::Collision)
            saw_collision = true;
        else if (finding.verdict == CollisionVerdict::Unproven)
            saw_unproven = true;
        if (report.findings.size() >= options.max_findings) {
            truncated = true;
            return;
        }
        report.findings.push_back(std::move(finding));
    };

    for (size_t index = 0; index < segments.size(); ++index) {
        const MotionSegment &segment = segments[index];

        const ToolheadShape *shape = shape_for(shapes, segment.tool);
        if (shape == nullptr || !shape->complete()) {
            // Without a shape there is nothing to sweep. That is not clear.
            CollisionFinding finding;
            finding.segment = index;
            finding.verdict = CollisionVerdict::Unproven;
            finding.with    = "T" + std::to_string(segment.tool);
            finding.reason  = shape == nullptr
                                 ? "no toolhead shape is described for this tool, so its " +
                                       std::string(motion_kind_name(segment.kind)) +
                                       " could not be checked"
                                 : "the toolhead shape for this tool is incomplete, so its " +
                                       std::string(motion_kind_name(segment.kind)) +
                                       " could not be checked";
            add(std::move(finding));
            continue;
        }

        // --- broad phase ----------------------------------------------------
        //
        // Sweep the whole toolhead across the segment once and reject anything
        // that cannot possibly be touched, before doing per-step work.
        const Aabb whole = shape->nozzle.merged(shape->body);
        const Aabb swept = whole.translated(segment.x0, segment.y0, segment.z0)
                               .merged(whole.translated(segment.x1, segment.y1, segment.z1));

        std::vector<const Obstacle *> near;
        for (const Obstacle &obstacle : obstacles) {
            if (ignored_for(obstacle, segment))
                continue;
            if (!obstacle.box.valid) {
                CollisionFinding finding;
                finding.segment = index;
                finding.verdict = CollisionVerdict::Unproven;
                finding.with    = obstacle.name;
                finding.reason  = "this obstacle has no described volume, so it could not be "
                                  "cleared";
                add(std::move(finding));
                continue;
            }
            if (swept.intersects(obstacle.box, options.clearance_mm))
                near.push_back(&obstacle);
            else
                ++report.broad_phase_rejections;
        }

        // --- precise phase ---------------------------------------------------
        const double dx     = segment.x1 - segment.x0;
        const double dy     = segment.y1 - segment.y0;
        const double dz     = segment.z1 - segment.z0;
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        const size_t steps  = size_t(std::max(1., std::ceil(length / options.sample_step_mm)));

        bool segment_reported = false;
        for (size_t step = 0; step <= steps && !segment_reported; ++step) {
            const double t = steps == 0 ? 0. : double(step) / double(steps);
            const double x = segment.x0 + dx * t;
            const double y = segment.y0 + dy * t;
            const double z = segment.z0 + dz * t;

            const Aabb body   = shape->body.translated(x, y, z);
            const Aabb nozzle = shape->nozzle.translated(x, y, z);
            ++report.precise_checks;

            for (const Obstacle *obstacle : near) {
                const Aabb whole_here = body.merged(nozzle);
                if (!whole_here.intersects(obstacle->box, options.clearance_mm))
                    continue;
                CollisionFinding finding;
                finding.segment = index;
                finding.verdict = CollisionVerdict::Collision;
                finding.with    = obstacle->name.empty()
                                   ? std::string(obstacle_kind_name(obstacle->kind))
                                   : obstacle->name;
                finding.reason  = std::string(motion_kind_name(segment.kind)) +
                                 " brings the toolhead into the " +
                                 obstacle_kind_name(obstacle->kind) + " at " +
                                 coordinate(x, y, z);
                finding.at_x = x;
                finding.at_y = y;
                finding.at_z = z;
                add(std::move(finding));
                segment_reported = true;
                break;
            }
            if (segment_reported)
                break;

            // --- printed geometry ------------------------------------------
            //
            // The nozzle is meant to reach printed geometry while printing.
            // Everything else on the toolhead is not, ever.
            bool unknown_column = false;
            const std::optional<double> highest =
                printed.highest_in(body.min_x, body.min_y, body.max_x, body.max_y,
                                   &unknown_column);

            if (highest.has_value() && body.min_z + COLLISION_EPSILON < *highest) {
                CollisionFinding finding;
                finding.segment = index;
                finding.verdict = CollisionVerdict::Collision;
                finding.with    = "printed geometry";
                std::ostringstream reason;
                reason.setf(std::ios::fixed);
                reason.precision(2);
                reason << motion_kind_name(segment.kind) << " passes at Z " << body.min_z
                       << " over printed geometry standing " << *highest << " mm tall at "
                       << coordinate(x, y, z);
                finding.reason = reason.str();
                finding.at_x   = x;
                finding.at_y   = y;
                finding.at_z   = z;
                add(std::move(finding));
                segment_reported = true;
                break;
            }

            if (segment.kind != MotionKind::Print) {
                // A non-printing move must not drive the nozzle itself into
                // the part either.
                bool nozzle_unknown = false;
                const std::optional<double> under_nozzle =
                    printed.highest_in(nozzle.min_x, nozzle.min_y, nozzle.max_x, nozzle.max_y,
                                       &nozzle_unknown);
                if (under_nozzle.has_value() &&
                    nozzle.min_z + COLLISION_EPSILON < *under_nozzle) {
                    CollisionFinding finding;
                    finding.segment = index;
                    finding.verdict = CollisionVerdict::Collision;
                    finding.with    = "printed geometry";
                    finding.reason  = std::string(motion_kind_name(segment.kind)) +
                                     " drives the nozzle into printed geometry at " +
                                     coordinate(x, y, z);
                    finding.at_x = x;
                    finding.at_y = y;
                    finding.at_z = z;
                    add(std::move(finding));
                    segment_reported = true;
                    break;
                }
                unknown_column = unknown_column || nozzle_unknown;
            }

            if (unknown_column) {
                CollisionFinding finding;
                finding.segment = index;
                finding.verdict = CollisionVerdict::Unproven;
                finding.with    = "printed geometry";
                finding.reason  = "the height of at least one column beneath the toolhead at " +
                                 coordinate(x, y, z) +
                                 " was never determined, so this motion could not be cleared";
                finding.at_x = x;
                finding.at_y = y;
                finding.at_z = z;
                add(std::move(finding));
                segment_reported = true;
                break;
            }
        }
    }

    if (truncated)
        // A capped list must not read as a complete one.
        report.findings.push_back({ 0, CollisionVerdict::Unproven, "report",
                                    "more findings exist than the configured limit of " +
                                        std::to_string(options.max_findings) +
                                        "; this report is truncated",
                                    std::nullopt, std::nullopt, std::nullopt });

    // A collision is worse than an unproven motion, but neither permits motion.
    report.verdict = saw_collision ? CollisionVerdict::Collision
                                   : (saw_unproven ? CollisionVerdict::Unproven
                                                   : CollisionVerdict::Clear);
    return report;
}

} // namespace Slic3r
