///|/ TigerSlicer Phase 25E: preview data and the first-use acknowledgement for
///|/ the experimental bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BatchPreview_hpp_
#define slic3r_BatchPreview_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "BatchSemantics.hpp"

namespace Slic3r {

// --- the first-use acknowledgement -----------------------------------------

enum class AcknowledgementVerdict : uint8_t
{
    // This user has explicitly accepted the current terms.
    Granted,
    // Nobody has accepted them yet, or not this user.
    Required,
    // The terms changed since this user accepted them, so the acceptance no
    // longer covers what would be run.
    Stale,
    // This user was asked and said no.
    Declined,
};

const char *acknowledgement_verdict_name(AcknowledgementVerdict verdict);

// What one user did when they were asked. There is deliberately no way to
// express "on by default": a record only exists because somebody was asked,
// and `explicitly_answered` is what says they answered rather than had it
// answered for them.
struct AcknowledgementRecord
{
    std::string user;
    std::string feature;

    // The revision of the terms this user actually saw.
    int version { 0 };

    bool explicitly_answered { false };
    bool accepted { false };
};

struct AcknowledgementRequest
{
    std::string user;
    std::string feature;
    // The revision of the terms as they stand now.
    int version { 0 };
};

struct AcknowledgementGate
{
    AcknowledgementVerdict verdict { AcknowledgementVerdict::Required };

    // True only for Granted. Nothing else lets the mode run.
    bool granted { false };

    // What the user has to be told before being asked, when they have to be
    // asked. Empty when they do not.
    std::vector<std::string> terms;
    std::vector<std::string> reasons;
};

// Decide whether this user has accepted this feature's current terms.
//
// The default is always no. An acknowledgement belongs to one user and one
// revision of the terms: another user's acceptance does not carry over, and an
// acceptance of older terms goes stale rather than silently covering terms
// that user never saw. A record that nobody explicitly answered is not an
// acceptance at all.
AcknowledgementGate check_acknowledgement(const AcknowledgementRequest              &request,
                                          const std::vector<AcknowledgementRecord> &records);

// --- preview data -----------------------------------------------------------

enum class PreviewMarkerKind : uint8_t
{
    BatchStart,
    Toolchange,
    Return,
    // The point at which global Z goes back below a height already reached.
    ZRegression,
    Descent,
    Finding,
};

const char *preview_marker_kind_name(PreviewMarkerKind kind);

enum class PreviewBannerLevel : uint8_t
{
    // Always present. The mode never appears without it.
    Experimental,
    Caution,
    // Something that stops this schedule being exported.
    Blocked,
};

const char *preview_banner_level_name(PreviewBannerLevel level);

struct PreviewSegment
{
    size_t operation { 0 };
    size_t component { 0 };
    int    tool { 0 };

    std::string component_label;

    double z_bottom_mm { 0. };
    double z_top_mm { 0. };

    // On the schedule's own clock, in emission order.
    double start_s { 0. };
    double end_s { 0. };

    bool starts_batch { false };
    bool downward { false };
};

struct PreviewMarker
{
    PreviewMarkerKind     kind { PreviewMarkerKind::BatchStart };
    std::optional<size_t> operation;
    double                at_s { 0. };
    double                z_mm { 0. };
    std::string           label;
};

struct PreviewBanner
{
    PreviewBannerLevel level { PreviewBannerLevel::Experimental };
    std::string        text;
};

struct BatchPreview
{
    // The mode is experimental. This is true in every preview it ever
    // produces, including a perfectly sound one, and there is no input that
    // turns it off.
    bool experimental { true };

    // Whether this schedule may actually be exported. Sound or degraded, and
    // acknowledged, and nothing else.
    bool exportable { false };

    std::vector<PreviewSegment> segments;
    std::vector<PreviewMarker>  markers;
    std::vector<PreviewBanner>  banners;

    AcknowledgementGate gate;

    // What the ordinary schedule would have done, so the preview shows a
    // comparison rather than a claim.
    ScheduleMetrics legacy_metrics;
    ScheduleMetrics batch_metrics;

    bool has_banner(PreviewBannerLevel level) const;
};

// Build everything the preview needs to show what a batched schedule actually
// does.
//
// The preview is built whether or not the schedule may be exported: seeing
// what the mode would do is how somebody decides whether to acknowledge it.
// What the preview never does is present it as production-safe - the
// Experimental banner is unconditional, a schedule that was not proven says so
// in different words from one that was proven wrong, and an unacknowledged or
// unsound schedule is shown with `exportable` false.
BatchPreview build_batch_preview(const BatchPlan            &plan,
                                 const SemanticReview       &review,
                                 const SemanticPolicy       &policy,
                                 const AcknowledgementGate  &gate);

} // namespace Slic3r

#endif // slic3r_BatchPreview_hpp_
