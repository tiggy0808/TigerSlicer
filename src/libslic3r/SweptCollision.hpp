///|/ TigerSlicer Phase 10: swept-volume collision simulation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_SweptCollision_hpp_
#define slic3r_SweptCollision_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// An axis-aligned box. Default-constructed boxes are explicitly invalid so an
// unset shape can never be mistaken for a zero-sized one that collides with
// nothing.
struct Aabb
{
    bool   valid { false };
    double min_x { 0. }, min_y { 0. }, min_z { 0. };
    double max_x { 0. }, max_y { 0. }, max_z { 0. };

    static Aabb of(double min_x, double min_y, double min_z,
                   double max_x, double max_y, double max_z);

    bool intersects(const Aabb &other, double tolerance = 0.) const;
    Aabb translated(double dx, double dy, double dz) const;
    // The box covering this one at both ends of a motion.
    Aabb merged(const Aabb &other) const;
};

// The volume a tool occupies, relative to its own reference point.
struct ToolheadShape
{
    int tool { 0 };
    // The tip. Touching printed geometry with this is what printing is.
    Aabb nozzle;
    // Everything else: heater block, fan shroud, carriage. Touching printed
    // geometry with any of it is a crash.
    Aabb body;

    bool complete() const { return nozzle.valid && body.valid; }
};

enum class ObstacleKind : uint8_t
{
    Dock,
    ParkedTool,
    Brush,
    PurgeHardware,
    Fixture,
};

const char *obstacle_kind_name(ObstacleKind kind);

struct Obstacle
{
    std::string  name;
    ObstacleKind kind { ObstacleKind::Fixture };
    Aabb         box;
    // For docks and parked tools: which tool this belongs to. A tool is
    // allowed to touch its own dock, but only while docking.
    std::optional<int> owning_tool;
};

// Printed geometry as a column height field. A cell whose height was never
// determined is unknown, which is not the same as empty.
struct PrintedHeightField
{
    double origin_x { 0. };
    double origin_y { 0. };
    double cell_mm { 1. };
    size_t nx { 0 };
    size_t ny { 0 };
    // Row-major, ny rows of nx. Absent entries mean the column was never
    // determined.
    std::vector<std::optional<double>> height_mm;

    bool empty() const { return nx == 0 || ny == 0; }
    // Highest known column overlapping the footprint, and whether any column
    // in it was unknown.
    std::optional<double> highest_in(double min_x, double min_y,
                                     double max_x, double max_y,
                                     bool *any_unknown) const;
};

enum class MotionKind : uint8_t
{
    Print,
    Travel,
    DownwardZ,
    DockApproach,
    DockExit,
};

const char *motion_kind_name(MotionKind kind);

struct MotionSegment
{
    int        tool { 0 };
    MotionKind kind { MotionKind::Travel };
    double     x0 { 0. }, y0 { 0. }, z0 { 0. };
    double     x1 { 0. }, y1 { 0. }, z1 { 0. };
};

// Three outcomes, deliberately. "Not proven safe" is its own answer and never
// collapses into "clear".
enum class CollisionVerdict : uint8_t
{
    Clear,
    Collision,
    Unproven,
};

const char *collision_verdict_name(CollisionVerdict verdict);

struct CollisionFinding
{
    size_t           segment { 0 };
    CollisionVerdict verdict { CollisionVerdict::Clear };
    std::string      with;
    std::string      reason;
    // Where along the motion it was detected, when it was.
    std::optional<double> at_x, at_y, at_z;
};

struct CollisionReport
{
    CollisionVerdict              verdict { CollisionVerdict::Clear };
    std::vector<CollisionFinding> findings;

    // Broad-phase rejections versus precise checks actually performed, so the
    // acceleration can be shown to be doing something.
    size_t broad_phase_rejections { 0 };
    size_t precise_checks { 0 };

    std::string error;

    // Motion is only permitted when every segment was proven clear.
    bool motion_permitted() const { return error.empty() && verdict == CollisionVerdict::Clear; }
};

struct CollisionOptions
{
    // Clearance required between the toolhead body and anything else.
    double clearance_mm { 0. };
    // Longest step the precise phase may take along a segment.
    double sample_step_mm { 1. };
    // Stop after this many findings, reported rather than silently truncated.
    size_t max_findings { 64 };
};

// Simulate the swept volume of every segment against fixtures, parked tools,
// dock hardware and printed geometry.
//
// A segment whose inputs are incomplete - no shape for the moving tool, or an
// undetermined column beneath it - is reported Unproven. It is never reported
// clear.
CollisionReport simulate_collisions(const std::vector<MotionSegment> &segments,
                                    const std::vector<ToolheadShape> &shapes,
                                    const std::vector<Obstacle>      &obstacles,
                                    const PrintedHeightField         &printed,
                                    const CollisionOptions           &options);

} // namespace Slic3r

#endif // slic3r_SweptCollision_hpp_
