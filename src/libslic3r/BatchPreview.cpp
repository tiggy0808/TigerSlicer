///|/ TigerSlicer Phase 25E: preview data and the first-use acknowledgement for
///|/ the experimental bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BatchPreview.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace Slic3r {

const char *acknowledgement_verdict_name(AcknowledgementVerdict verdict)
{
    switch (verdict) {
    case AcknowledgementVerdict::Granted:  return "granted";
    case AcknowledgementVerdict::Required: return "required";
    case AcknowledgementVerdict::Stale:    return "stale";
    case AcknowledgementVerdict::Declined: return "declined";
    }
    return "required";
}

const char *preview_marker_kind_name(PreviewMarkerKind kind)
{
    switch (kind) {
    case PreviewMarkerKind::BatchStart:  return "batch start";
    case PreviewMarkerKind::Toolchange:  return "toolchange";
    case PreviewMarkerKind::Return:      return "return";
    case PreviewMarkerKind::ZRegression: return "z regression";
    case PreviewMarkerKind::Descent:     return "descent";
    case PreviewMarkerKind::Finding:     return "finding";
    }
    return "finding";
}

const char *preview_banner_level_name(PreviewBannerLevel level)
{
    switch (level) {
    case PreviewBannerLevel::Experimental: return "experimental";
    case PreviewBannerLevel::Caution:      return "caution";
    case PreviewBannerLevel::Blocked:      return "blocked";
    }
    return "experimental";
}

bool BatchPreview::has_banner(PreviewBannerLevel level) const
{
    return std::any_of(banners.begin(), banners.end(),
                       [&](const PreviewBanner &banner) { return banner.level == level; });
}

namespace {

std::string number(double value, int precision = 1)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

std::vector<std::string> experimental_terms()
{
    return {
        "Bounded-Z batching prints several layers of one component before "
        "moving to another, so the toolhead goes back down to heights it has "
        "already passed.",
        "It has been checked in software against synthetic cases only. No "
        "print made with it has been verified on this machine, or on any "
        "machine.",
        "A schedule this mode produces is not offered for export unless every "
        "descent was proven clear and nothing about the reordering was found "
        "unsound or left unproven.",
        "It stays off until you turn it on, and turning it on applies to this "
        "user only.",
    };
}

} // namespace

AcknowledgementGate check_acknowledgement(const AcknowledgementRequest             &request,
                                          const std::vector<AcknowledgementRecord> &records)
{
    AcknowledgementGate gate;
    gate.terms = experimental_terms();

    const AcknowledgementRecord *mine = nullptr;
    for (const AcknowledgementRecord &record : records)
        if (record.user == request.user && record.feature == request.feature)
            if (mine == nullptr || record.version > mine->version)
                mine = &record;

    if (mine == nullptr) {
        gate.verdict = AcknowledgementVerdict::Required;
        gate.reasons.push_back(
            request.user.empty()
                ? std::string("no user is identified, so nobody can have acknowledged this "
                              "mode; it stays off")
                : "\"" + request.user + "\" has not been asked about " + request.feature +
                      " yet; it stays off until they are, and somebody else's acceptance "
                      "does not carry over");
        return gate;
    }

    if (! mine->explicitly_answered) {
        gate.verdict = AcknowledgementVerdict::Required;
        gate.reasons.push_back("a record exists for \"" + request.user +
                               "\" but nobody answered it; a setting that was never "
                               "answered is not an acknowledgement");
        return gate;
    }

    if (! mine->accepted) {
        gate.verdict = AcknowledgementVerdict::Declined;
        gate.reasons.push_back("\"" + request.user + "\" was asked about " + request.feature +
                               " and declined");
        return gate;
    }

    if (mine->version < request.version) {
        gate.verdict = AcknowledgementVerdict::Stale;
        gate.reasons.push_back("\"" + request.user + "\" accepted revision " +
                               std::to_string(mine->version) + " of these terms and they are "
                               "now at revision " + std::to_string(request.version) +
                               "; the acceptance does not cover terms they never saw");
        return gate;
    }

    gate.verdict = AcknowledgementVerdict::Granted;
    gate.granted = true;
    gate.reasons.push_back("\"" + request.user + "\" accepted revision " +
                           std::to_string(mine->version) + " of these terms");
    return gate;
}

BatchPreview build_batch_preview(const BatchPlan           &plan,
                                 const SemanticReview      &review,
                                 const SemanticPolicy      &policy,
                                 const AcknowledgementGate &gate)
{
    BatchPreview preview;
    preview.gate           = gate;
    preview.legacy_metrics = plan.legacy_metrics;
    preview.batch_metrics  = plan.metrics;

    // The banner that is never absent. It comes first so that anything reading
    // the list in order sees it before it sees a saving.
    preview.banners.push_back(
        { PreviewBannerLevel::Experimental,
          "Bounded-Z batching is experimental. It has been checked in software "
          "against synthetic cases only; nothing produced with it has been verified "
          "on a machine." });

    // --- the timeline -------------------------------------------------------
    std::map<size_t, bool>   seen;
    std::map<size_t, size_t> label_index;
    double                   clock = 0.;
    std::optional<int>       held_tool;
    double                   peak_z = 0.;
    bool                     first  = true;

    for (size_t i = 0; i < plan.operations.size(); ++i) {
        const ScheduledOperation &op = plan.operations[i];

        if (! held_tool.has_value() || *held_tool != op.tool) {
            if (held_tool.has_value()) {
                preview.markers.push_back({ PreviewMarkerKind::Toolchange, i, clock,
                                            op.z_bottom_mm,
                                            "T" + std::to_string(*held_tool) + " to T" +
                                                std::to_string(op.tool) });
                clock += policy.toolchange_cost_s;
            }
            held_tool = op.tool;
        }

        const bool returning = op.starts_batch && seen.count(op.component) > 0;
        if (returning) {
            preview.markers.push_back({ PreviewMarkerKind::Return, i, clock, op.z_bottom_mm,
                                        "back to component " +
                                            std::to_string(op.component) + " after leaving "
                                            "it" });
            clock += policy.reprime_cost_s;
        }
        if (op.starts_batch)
            preview.markers.push_back({ PreviewMarkerKind::BatchStart, i, clock,
                                        op.z_bottom_mm,
                                        "component " + std::to_string(op.component) +
                                            " on T" + std::to_string(op.tool) });

        if (op.downward_move)
            preview.markers.push_back({ PreviewMarkerKind::Descent, i, clock, op.z_bottom_mm,
                                        "descending to " + number(op.z_bottom_mm, 2) +
                                            " mm to pick component " +
                                            std::to_string(op.component) + " back up" });

        if (! first && op.z_top_mm < peak_z - policy.tolerance_mm)
            preview.markers.push_back({ PreviewMarkerKind::ZRegression, i, clock, op.z_top_mm,
                                        "global Z goes back below " + number(peak_z, 2) +
                                            " mm here" });

        PreviewSegment segment;
        segment.operation   = i;
        segment.component   = op.component;
        segment.tool        = op.tool;
        segment.z_bottom_mm = op.z_bottom_mm;
        segment.z_top_mm    = op.z_top_mm;
        segment.start_s     = clock;
        segment.end_s       = clock + op.duration_s;
        segment.starts_batch = op.starts_batch;
        segment.downward    = op.downward_move;
        segment.component_label = "component " + std::to_string(op.component) + " (T" +
                                  std::to_string(op.tool) + ")";
        preview.segments.push_back(segment);

        clock = segment.end_s;
        if (first || op.z_top_mm > peak_z + policy.tolerance_mm)
            peak_z = op.z_top_mm;
        first        = false;
        seen[op.component] = true;
    }

    // --- what the review found ----------------------------------------------
    for (const SemanticFinding &finding : review.findings) {
        if (finding.severity == SemanticSeverity::Info)
            continue;
        double at_s = 0.;
        double z_mm = 0.;
        if (finding.operation.has_value() && *finding.operation < preview.segments.size()) {
            at_s = preview.segments[*finding.operation].start_s;
            z_mm = preview.segments[*finding.operation].z_top_mm;
        }
        preview.markers.push_back({ PreviewMarkerKind::Finding, finding.operation, at_s, z_mm,
                                    finding.message });
    }

    // --- banners -------------------------------------------------------------
    switch (review.verdict) {
    case SemanticVerdict::Unsound:
        preview.banners.push_back(
            { PreviewBannerLevel::Blocked,
              "This schedule was checked and found unsound: " +
                  std::to_string(review.count(SemanticSeverity::Blocking)) +
                  " thing(s) it does would damage the print. It is not offered for export." });
        break;
    case SemanticVerdict::Unproven:
        preview.banners.push_back(
            { PreviewBannerLevel::Blocked,
              "This schedule could not be checked. Something it depends on was never "
              "measured, so it is not known to be wrong and it is not known to be right - "
              "and it is not offered for export on that basis." });
        break;
    case SemanticVerdict::Degraded:
        preview.banners.push_back(
            { PreviewBannerLevel::Caution,
              "This schedule is printable, but " +
                  std::to_string(review.count(SemanticSeverity::Warning)) +
                  " thing(s) about it are worse than the ordinary schedule. They are marked "
                  "on the timeline." });
        break;
    case SemanticVerdict::Sound:
        break;
    }

    if (! gate.granted) {
        std::string why = gate.reasons.empty() ? std::string("it has not been acknowledged")
                                               : gate.reasons.front();
        preview.banners.push_back(
            { PreviewBannerLevel::Blocked,
              "This mode is off: " + why +
                  ". What it would do is shown here so the decision can be made against "
                  "something real." });
    }

    // --- the comparison, never as a claim of safety --------------------------
    if (plan.toolchanges_saved.has_value())
        preview.banners.push_back(
            { PreviewBannerLevel::Caution,
              "Batching this print drops " + std::to_string(*plan.toolchanges_saved) +
                  " toolchange(s), from " + std::to_string(plan.legacy_metrics.toolchanges) +
                  " to " + std::to_string(plan.metrics.toolchanges) +
                  ". Fewer toolchanges is not the same as a better print, and none of this "
                  "has been verified on a machine." });

    preview.exportable = gate.granted && review.emittable() && plan.error.empty() &&
                         ! plan.operations.empty();

    if (preview.exportable && ! preview.has_banner(PreviewBannerLevel::Caution))
        preview.banners.push_back(
            { PreviewBannerLevel::Caution,
              "Nothing found wrong with this schedule is not the same as this schedule "
              "having been proven on a machine. Watch the first print it makes." });

    return preview;
}

} // namespace Slic3r
