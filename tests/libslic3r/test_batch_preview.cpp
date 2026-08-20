// Phase 25E: what the preview is allowed to show about a bounded-Z batch
// schedule, and what has to happen before the mode runs at all. The rules
// under test: the acknowledgement is explicit, per-user and per-revision and
// never defaults to on; the preview is built even when the schedule may not be
// exported, because seeing what it would do is how the decision gets made; and
// nothing the preview says presents an unverified schedule as production-safe.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/BatchPreview.hpp"

using namespace Slic3r;

namespace {

ScheduledOperation op(size_t component, size_t layer, int tool, double z_bottom,
                      double z_top, double duration, bool starts_batch = false)
{
    ScheduledOperation value;
    value.component    = component;
    value.layer        = layer;
    value.tool         = tool;
    value.z_bottom_mm  = z_bottom;
    value.z_top_mm     = z_top;
    value.duration_s   = duration;
    value.starts_batch = starts_batch;
    return value;
}

BatchPlan two_towers()
{
    BatchPlan plan;
    plan.mode    = ScheduleMode::BoundedBatch;
    plan.verdict = BatchVerdict::Batched;
    plan.operations = {
        op(0, 0, 0, 0.0, 0.2, 30., true), op(0, 1, 0, 0.2, 0.4, 30.),
        op(1, 0, 1, 0.0, 0.2, 30., true), op(1, 1, 1, 0.2, 0.4, 30.),
        op(0, 2, 0, 0.4, 0.6, 30., true), op(0, 3, 0, 0.6, 0.8, 30.),
        op(1, 2, 1, 0.4, 0.6, 30., true), op(1, 3, 1, 0.6, 0.8, 30.),
    };
    plan.operations[2].downward_move = true;
    plan.operations[6].downward_move = true;

    plan.metrics.total_time_s       = 315.;
    plan.metrics.toolchanges        = 3;
    plan.metrics.downward_moves     = 2;
    plan.legacy_metrics.toolchanges = 7;
    plan.toolchanges_saved          = 4;
    return plan;
}

std::vector<ComponentSemantics> semantics()
{
    ComponentSemantics a;
    a.id                 = 0;
    a.material           = "PLA";
    a.bond_window_s      = 600.;
    a.cancellable        = true;
    a.cancel_object_name = "left tower";

    ComponentSemantics b = a;
    b.id                 = 1;
    b.cancel_object_name = "right tower";
    return { a, b };
}

SemanticPolicy policy()
{
    SemanticPolicy value;
    value.ooze_window_s     = 300.;
    value.toolchange_cost_s = 25.;
    value.reprime_cost_s    = 8.;
    return value;
}

AcknowledgementGate granted_gate()
{
    AcknowledgementRecord record;
    record.user                = "tiger";
    record.feature             = "bounded-z-batching";
    record.version             = 2;
    record.explicitly_answered = true;
    record.accepted            = true;

    AcknowledgementRequest request;
    request.user    = "tiger";
    request.feature = "bounded-z-batching";
    request.version = 2;
    return check_acknowledgement(request, { record });
}

size_t markers_of(const BatchPreview &preview, PreviewMarkerKind kind)
{
    return size_t(std::count_if(preview.markers.begin(), preview.markers.end(),
                                [&](const PreviewMarker &marker) {
                                    return marker.kind == kind;
                                }));
}

bool banner_mentions(const BatchPreview &preview, const std::string &fragment)
{
    return std::any_of(preview.banners.begin(), preview.banners.end(),
                       [&](const PreviewBanner &banner) {
                           return banner.text.find(fragment) != std::string::npos;
                       });
}

} // namespace

TEST_CASE("The mode is off until this user is asked and says yes", "[batch-preview]")
{
    AcknowledgementRequest request;
    request.user    = "tiger";
    request.feature = "bounded-z-batching";
    request.version = 2;

    AcknowledgementRecord record;
    record.user                = "tiger";
    record.feature             = "bounded-z-batching";
    record.version             = 2;
    record.explicitly_answered = true;
    record.accepted            = true;

    SECTION("nobody asked yet") {
        const AcknowledgementGate gate = check_acknowledgement(request, {});
        CHECK(gate.verdict == AcknowledgementVerdict::Required);
        CHECK_FALSE(gate.granted);
        CHECK_FALSE(gate.terms.empty());
    }

    SECTION("asked and accepted") {
        const AcknowledgementGate gate = check_acknowledgement(request, { record });
        CHECK(gate.verdict == AcknowledgementVerdict::Granted);
        CHECK(gate.granted);
    }

    SECTION("asked and declined") {
        record.accepted                = false;
        const AcknowledgementGate gate = check_acknowledgement(request, { record });
        CHECK(gate.verdict == AcknowledgementVerdict::Declined);
        CHECK_FALSE(gate.granted);
    }

    SECTION("a setting nobody answered is not an acknowledgement") {
        record.explicitly_answered     = false;
        const AcknowledgementGate gate = check_acknowledgement(request, { record });
        CHECK(gate.verdict == AcknowledgementVerdict::Required);
        CHECK_FALSE(gate.granted);
    }

    SECTION("somebody else's acceptance does not carry over") {
        record.user                    = "someone else";
        const AcknowledgementGate gate = check_acknowledgement(request, { record });
        CHECK(gate.verdict == AcknowledgementVerdict::Required);
        CHECK_FALSE(gate.granted);
    }

    SECTION("an acceptance of older terms goes stale") {
        record.version                 = 1;
        const AcknowledgementGate gate = check_acknowledgement(request, { record });
        CHECK(gate.verdict == AcknowledgementVerdict::Stale);
        CHECK_FALSE(gate.granted);
    }

    SECTION("accepting a later revision still covers this one") {
        record.version                 = 3;
        const AcknowledgementGate gate = check_acknowledgement(request, { record });
        CHECK(gate.verdict == AcknowledgementVerdict::Granted);
    }
}

TEST_CASE("Every preview is marked experimental, including a sound one",
          "[batch-preview]")
{
    const BatchPlan      plan   = two_towers();
    const SemanticReview review = review_batch_semantics(plan, semantics(), {}, {}, policy());
    REQUIRE(review.verdict == SemanticVerdict::Sound);

    const BatchPreview preview = build_batch_preview(plan, review, policy(), granted_gate());

    CHECK(preview.experimental);
    CHECK(preview.exportable);
    CHECK(preview.banners.front().level == PreviewBannerLevel::Experimental);
    CHECK(banner_mentions(preview, "nothing produced with it has been verified"));
    // A schedule nothing was found wrong with is still not a proven schedule.
    CHECK(preview.has_banner(PreviewBannerLevel::Caution));
    CHECK_FALSE(preview.has_banner(PreviewBannerLevel::Blocked));
}

TEST_CASE("The preview shows what the schedule does on its own clock",
          "[batch-preview]")
{
    const BatchPlan      plan    = two_towers();
    const SemanticReview review  = review_batch_semantics(plan, semantics(), {}, {}, policy());
    const BatchPreview   preview = build_batch_preview(plan, review, policy(), granted_gate());

    REQUIRE(preview.segments.size() == 8);
    CHECK(preview.segments.front().start_s == 0.);
    CHECK(preview.segments.front().component_label == "component 0 (T0)");
    // The last segment ends where the semantic estimate says the print ends.
    CHECK(preview.segments.back().end_s == review.estimate.total_time_s);

    // Segments never overlap and never go backwards in time, however far the
    // global Z goes backwards.
    for (size_t i = 1; i < preview.segments.size(); ++i)
        CHECK(preview.segments[i].start_s >= preview.segments[i - 1].end_s);

    CHECK(markers_of(preview, PreviewMarkerKind::BatchStart) == 4);
    CHECK(markers_of(preview, PreviewMarkerKind::Toolchange) == 3);
    CHECK(markers_of(preview, PreviewMarkerKind::Return) == 2);
    CHECK(markers_of(preview, PreviewMarkerKind::Descent) == 2);
    // Where the global Z drops below a height already reached.
    CHECK(markers_of(preview, PreviewMarkerKind::ZRegression) == 2);
}

TEST_CASE("An unacknowledged schedule is shown but never exported",
          "[batch-preview]")
{
    AcknowledgementRequest request;
    request.user    = "tiger";
    request.feature = "bounded-z-batching";
    request.version = 2;

    const BatchPlan      plan    = two_towers();
    const SemanticReview review  = review_batch_semantics(plan, semantics(), {}, {}, policy());
    const BatchPreview   preview = build_batch_preview(plan, review, policy(),
                                                       check_acknowledgement(request, {}));

    CHECK_FALSE(preview.exportable);
    CHECK(preview.has_banner(PreviewBannerLevel::Blocked));
    CHECK(banner_mentions(preview, "so the decision can be made against something real"));
    // The timeline is still there: that is what the decision is made against.
    CHECK(preview.segments.size() == 8);
}

TEST_CASE("Unsound and unproven are shown in different words", "[batch-preview]")
{
    const BatchPlan plan = two_towers();

    SECTION("unsound says what it found") {
        BatchPlan broken = plan;
        std::swap(broken.operations[0], broken.operations[1]);
        const SemanticReview review =
            review_batch_semantics(broken, semantics(), {}, {}, policy());
        REQUIRE(review.verdict == SemanticVerdict::Unsound);

        const BatchPreview preview =
            build_batch_preview(broken, review, policy(), granted_gate());
        CHECK_FALSE(preview.exportable);
        CHECK(banner_mentions(preview, "checked and found unsound"));
        CHECK(markers_of(preview, PreviewMarkerKind::Finding) > 0);
    }

    SECTION("unproven says it could not be checked") {
        std::vector<ComponentSemantics> unmeasured = semantics();
        unmeasured[0].bond_window_s.reset();
        const SemanticReview review =
            review_batch_semantics(plan, unmeasured, {}, {}, policy());
        REQUIRE(review.verdict == SemanticVerdict::Unproven);

        const BatchPreview preview =
            build_batch_preview(plan, review, policy(), granted_gate());
        CHECK_FALSE(preview.exportable);
        CHECK(banner_mentions(preview, "not known to be wrong and it is not known to be "
                                       "right"));
    }
}

TEST_CASE("A degraded schedule may be exported, with what is worse marked",
          "[batch-preview]")
{
    std::vector<ComponentSemantics> cooling = semantics();
    cooling[0].bond_window_s                = 30.;

    const BatchPlan      plan   = two_towers();
    const SemanticReview review = review_batch_semantics(plan, cooling, {}, {}, policy());
    REQUIRE(review.verdict == SemanticVerdict::Degraded);

    const BatchPreview preview = build_batch_preview(plan, review, policy(), granted_gate());

    CHECK(preview.exportable);
    CHECK(preview.has_banner(PreviewBannerLevel::Caution));
    CHECK(banner_mentions(preview, "worse than the ordinary schedule"));
    CHECK(markers_of(preview, PreviewMarkerKind::Finding) > 0);
}

TEST_CASE("The saving is shown next to what it does not prove", "[batch-preview]")
{
    const BatchPlan      plan    = two_towers();
    const SemanticReview review  = review_batch_semantics(plan, semantics(), {}, {}, policy());
    const BatchPreview   preview = build_batch_preview(plan, review, policy(), granted_gate());

    CHECK(preview.legacy_metrics.toolchanges == 7);
    CHECK(preview.batch_metrics.toolchanges == 3);
    CHECK(banner_mentions(preview, "drops 4 toolchange(s)"));
    CHECK(banner_mentions(preview, "Fewer toolchanges is not the same as a better print"));
}

TEST_CASE("An empty schedule is not exportable however sound it is",
          "[batch-preview]")
{
    BatchPlan empty;
    empty.mode    = ScheduleMode::Legacy;
    empty.verdict = BatchVerdict::Disabled;

    const SemanticReview review = review_batch_semantics(empty, {}, {}, {}, policy());
    REQUIRE(review.verdict == SemanticVerdict::Sound);

    const BatchPreview preview = build_batch_preview(empty, review, policy(), granted_gate());
    CHECK_FALSE(preview.exportable);
    CHECK(preview.experimental);
    CHECK(preview.segments.empty());
}
