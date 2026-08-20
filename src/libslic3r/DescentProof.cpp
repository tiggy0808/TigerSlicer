///|/ TigerSlicer Phase 25C: downward-Z and clearance proof for the experimental
///|/ bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "DescentProof.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *descent_verdict_name(DescentVerdict verdict)
{
    switch (verdict) {
    case DescentVerdict::Clear:    return "clear";
    case DescentVerdict::Refused:  return "refused";
    case DescentVerdict::Unproven: return "unproven";
    }
    return "unproven";
}

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

const ToolheadShape *shape_of(const std::vector<ToolheadShape> &shapes, int tool)
{
    for (const ToolheadShape &shape : shapes)
        if (shape.tool == tool)
            return &shape;
    return nullptr;
}

} // namespace

DescentPlan prove_descent(const DescentRequest &request, const DescentContext &context)
{
    DescentPlan plan;

    // The whole toolhead has to be described. A shape with only a nozzle is
    // the classic way to prove the wrong thing.
    const ToolheadShape *shape = shape_of(context.shapes, request.tool);
    if (shape == nullptr) {
        plan.reasons.push_back("no toolhead shape is described for T" +
                               std::to_string(request.tool) +
                               ", so nothing about this descent can be proven");
        return plan;
    }
    if (!shape->body.valid) {
        plan.reasons.push_back(
            "only the nozzle tip is described for T" + std::to_string(request.tool) +
            "; clearing the nozzle point does not prove the carriage or gantry clears, so "
            "this descent is not proven");
        return plan;
    }
    if (!shape->nozzle.valid) {
        plan.reasons.push_back("the toolhead shape for T" + std::to_string(request.tool) +
                               " has no nozzle volume, so it is incomplete and nothing can "
                               "be proven from it");
        return plan;
    }

    if (!context.calibration_uncertainty_mm.has_value()) {
        plan.reasons.push_back(
            "tool offset and calibration uncertainty were never characterized for this "
            "machine, so no clearance figure can be shown to hold and the descent is not "
            "proven");
        return plan;
    }
    const double margin = context.clearance_mm + *context.calibration_uncertainty_mm;

    if (context.corridors.empty()) {
        plan.reasons.push_back("no descent corridor was described, and descending wherever "
                               "the toolhead happens to be is not a plan");
        return plan;
    }

    bool any_unproven = false;

    for (const DescentCorridor &corridor : context.corridors) {
        // The corridor has to be empty all the way down to the height being
        // descended to. This is what stops a descent while horizontally
        // overlapping a taller printed component.
        const double half = corridor.half_width_mm + margin;
        bool         unknown_column = false;
        const std::optional<double> highest = context.printed.highest_in(
            corridor.center_x - half, corridor.center_y - half, corridor.center_x + half,
            corridor.center_y + half, &unknown_column);

        if (unknown_column) {
            plan.rejected_corridors.push_back(
                corridor.name + ": the printed height under it was never determined, so it "
                "cannot be called empty - unknown is not the same as clear");
            any_unproven = true;
            continue;
        }
        if (highest.has_value() &&
            *highest > request.to_z - context.corridor_headroom_mm + EPSILON) {
            plan.rejected_corridors.push_back(
                corridor.name + ": printed geometry in it reaches " + number(*highest, 2) +
                " mm, which does not leave " + number(context.corridor_headroom_mm, 2) +
                " mm under a descent to " + number(request.to_z, 2) +
                " mm; the toolhead would be dropping onto the part");
            continue;
        }

        // Across at height, straight down inside the corridor, then in at low
        // z. The descent finishes before the toolhead is ever over the lagging
        // component.
        std::vector<MotionSegment> segments;

        MotionSegment across;
        across.tool = request.tool;
        across.kind = MotionKind::Travel;
        across.x0   = request.from_x;
        across.y0   = request.from_y;
        across.z0   = request.from_z;
        across.x1   = corridor.center_x;
        across.y1   = corridor.center_y;
        across.z1   = request.from_z;
        segments.push_back(across);

        MotionSegment down;
        down.tool = request.tool;
        down.kind = MotionKind::DownwardZ;
        down.x0   = corridor.center_x;
        down.y0   = corridor.center_y;
        down.z0   = request.from_z;
        down.x1   = corridor.center_x;
        down.y1   = corridor.center_y;
        down.z1   = request.to_z;
        segments.push_back(down);

        MotionSegment into;
        into.tool = request.tool;
        into.kind = MotionKind::Travel;
        into.x0   = corridor.center_x;
        into.y0   = corridor.center_y;
        into.z0   = request.to_z;
        into.x1   = request.to_x;
        into.y1   = request.to_y;
        into.z1   = request.to_z;
        segments.push_back(into);

        CollisionOptions options;
        options.clearance_mm   = margin;
        options.sample_step_mm = context.sample_step_mm;

        const CollisionReport report = simulate_collisions(segments, context.shapes,
                                                           context.obstacles,
                                                           context.printed, options);

        if (report.motion_permitted()) {
            plan.verdict  = DescentVerdict::Clear;
            plan.corridor = corridor.name;
            plan.segments = segments;
            plan.report   = report;
            plan.reasons.push_back(
                "the descent to " + number(request.to_z, 2) + " mm happens inside " +
                corridor.name + ", which was verified empty, and finishes before the "
                "toolhead is over the lagging component; every segment was swept with the "
                "whole toolhead at " + number(margin, 2) +
                " mm of clearance, including " +
                number(*context.calibration_uncertainty_mm, 2) +
                " mm of calibration uncertainty");
            return plan;
        }

        if (report.verdict == CollisionVerdict::Unproven)
            any_unproven = true;

        std::string detail = report.error;
        if (detail.empty() && !report.findings.empty())
            detail = report.findings.front().with + ": " + report.findings.front().reason;
        plan.rejected_corridors.push_back(corridor.name + ": " +
                                          (detail.empty() ? std::string("the swept motion "
                                                                       "was not clear")
                                                          : detail));
        // Keep the last report, so a refusal can point at something concrete.
        plan.report = report;
    }

    plan.verdict = any_unproven ? DescentVerdict::Unproven : DescentVerdict::Refused;
    plan.reasons.push_back(
        any_unproven
            ? "no corridor could be shown to be clear for this descent; at least one could "
              "not be checked at all, so the descent is unproven rather than known bad"
            : "every corridor was checked and none of them is clear, so this descent is "
              "refused");
    return plan;
}

BatchDescentReview review_descents(const std::vector<DescentRequest> &descents,
                                   const DescentContext              &context)
{
    BatchDescentReview review;

    for (const DescentRequest &request : descents) {
        DescentPlan plan = prove_descent(request, context);
        switch (plan.verdict) {
        case DescentVerdict::Clear:    ++review.proven; break;
        case DescentVerdict::Refused:  ++review.refused; break;
        case DescentVerdict::Unproven: ++review.unproven; break;
        }
        review.descents.push_back(std::move(plan));
    }

    review.all_proven = !descents.empty() && review.refused == 0 && review.unproven == 0;

    if (review.refused > 0)
        review.notes.push_back(std::to_string(review.refused) +
                               " descent(s) were shown to be unsafe and cannot be emitted");
    if (review.unproven > 0)
        review.notes.push_back(std::to_string(review.unproven) +
                               " descent(s) could not be checked; an unverified downward "
                               "move is never emitted, so the batch has to be shortened or "
                               "the schedule given up");
    if (descents.empty())
        review.notes.push_back("this schedule needs no downward moves at all");

    return review;
}

size_t provable_batch_prefix(const BatchDescentReview &review)
{
    size_t count = 0;
    for (const DescentPlan &plan : review.descents) {
        if (plan.verdict != DescentVerdict::Clear)
            break;
        ++count;
    }
    return count;
}

} // namespace Slic3r
