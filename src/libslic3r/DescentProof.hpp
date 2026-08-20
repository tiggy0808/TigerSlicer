///|/ TigerSlicer Phase 25C: downward-Z and clearance proof for the experimental
///|/ bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_DescentProof_hpp_
#define slic3r_DescentProof_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SweptCollision.hpp"

namespace Slic3r {

// The same three outcomes as Phase 10, deliberately: "not proven safe" is its
// own answer here too and never collapses into "clear".
enum class DescentVerdict : uint8_t
{
    Clear,
    Refused,
    Unproven,
};

const char *descent_verdict_name(DescentVerdict verdict);

// A column of empty space the toolhead may drop through before it goes
// anywhere near the lagging component.
struct DescentCorridor
{
    std::string name;
    double      center_x { 0. }, center_y { 0. };
    double      half_width_mm { 0. };
};

struct DescentContext
{
    std::vector<ToolheadShape> shapes;
    std::vector<Obstacle>      obstacles;
    PrintedHeightField         printed;

    // Tried in the order given; the first one proven clear is used.
    std::vector<DescentCorridor> corridors;

    // Tool offset and calibration uncertainty. Absent means it was never
    // characterized, and nothing can then be proven clear.
    std::optional<double> calibration_uncertainty_mm;

    double clearance_mm { 1.0 };
    double sample_step_mm { 1.0 };
    // How far below the toolhead a corridor's printed geometry must stay.
    double corridor_headroom_mm { 0.5 };
};

// One return to a lagging component: where the toolhead is when a batch ends,
// and where the next batch has to start.
struct DescentRequest
{
    int    tool { 0 };
    double from_x { 0. }, from_y { 0. }, from_z { 0. };
    double to_x { 0. }, to_y { 0. }, to_z { 0. };
};

struct DescentPlan
{
    DescentVerdict verdict { DescentVerdict::Unproven };

    // The corridor the descent was planned in, when one was found.
    std::optional<std::string> corridor;

    // Travel across at height, then straight down inside the corridor, then
    // in at low z. In that order, always.
    std::vector<MotionSegment> segments;
    CollisionReport            report;

    std::vector<std::string> rejected_corridors;
    std::vector<std::string> reasons;
};

// Prove one descent.
//
// The descent is planned in an empty, verified corridor and completed before
// the toolhead goes anywhere over the lagging component; it never descends
// while horizontally overlapping printed geometry that reaches the height it
// is dropping to. The whole toolhead is swept, not the nozzle tip: a shape
// that describes only the nozzle leaves the descent unproven, because
// nozzle-point clearance says nothing about the carriage. Tool offset and
// calibration uncertainty widen the clearance, and an uncharacterized
// uncertainty leaves the descent unproven rather than assumed exact.
DescentPlan prove_descent(const DescentRequest &request, const DescentContext &context);

struct BatchDescentReview
{
    std::vector<DescentPlan> descents;

    size_t proven { 0 };
    size_t refused { 0 };
    size_t unproven { 0 };

    // True only when every descent was proven clear. Anything else means the
    // batch has to be shortened or the schedule blocked.
    bool all_proven { false };

    std::vector<std::string> notes;
};

// Prove every descent a batched schedule needs.
BatchDescentReview review_descents(const std::vector<DescentRequest> &descents,
                                   const DescentContext              &context);

// The largest number of leading batches whose descents were all proven. A
// batch schedule may only keep this many; the rest has to be given up.
size_t provable_batch_prefix(const BatchDescentReview &review);

} // namespace Slic3r

#endif // slic3r_DescentProof_hpp_
