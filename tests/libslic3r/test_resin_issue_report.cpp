///|/ TigerSlicer Phase UV4/UV5 groundwork: the resin issue model, export gate,
///|/ and repair verification.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/ResinIssueReport.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

ResinIssue issue(ResinIssueKind kind, uint32_t layer, const std::string &region = "")
{
    ResinIssue value;
    value.kind                 = kind;
    value.location.layer_index = layer;
    value.location.z_mm        = double(layer) * 0.05;
    value.location.region      = region;
    value.explanation          = resin_issue_kind_name(kind);
    return value;
}

bool contains(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("UV4: severity is TigerSlicer's decision, applied in one place", "[resin-issues]")
{
    // The detector's raw severity is ignored; classify_issue decides.
    CHECK(classify_issue(ResinIssueKind::Island) == ResinIssueSeverity::Blocking);
    CHECK(classify_issue(ResinIssueKind::ResinTrap) == ResinIssueSeverity::Blocking);
    CHECK(classify_issue(ResinIssueKind::InvalidLayerData) == ResinIssueSeverity::Blocking);
    CHECK(classify_issue(ResinIssueKind::EmptyLayer) == ResinIssueSeverity::Blocking);
    CHECK(classify_issue(ResinIssueKind::SuctionCup) == ResinIssueSeverity::Blocking);
    CHECK(classify_issue(ResinIssueKind::Overhang) == ResinIssueSeverity::Warning);
    CHECK(classify_issue(ResinIssueKind::TouchingBounds) == ResinIssueSeverity::Warning);
    CHECK(classify_issue(ResinIssueKind::ExcessiveHeight) == ResinIssueSeverity::Warning);

    // A detected issue arriving with the wrong severity is re-classified.
    std::vector<ResinIssue> detected = { issue(ResinIssueKind::Island, 10) };
    detected[0].severity             = ResinIssueSeverity::Info; // wrong on purpose
    const ResinAnalysis analysis = build_analysis(detected, AnalysisState::Complete, "hash-1");
    CHECK(analysis.issues[0].severity == ResinIssueSeverity::Blocking);
}

TEST_CASE("UV4: an analysis is sorted, hashed, and honest about being incomplete",
          "[resin-issues]")
{
    std::vector<ResinIssue> detected = {
        issue(ResinIssueKind::Overhang, 30),
        issue(ResinIssueKind::Island, 5),
        issue(ResinIssueKind::ResinTrap, 5, "cavity-a"),
    };

    SECTION("issues are sorted by layer then kind")
    {
        const ResinAnalysis analysis =
            build_analysis(detected, AnalysisState::Complete, "hash-1");
        REQUIRE(analysis.issues.size() == 3);
        CHECK(analysis.issues[0].location.layer_index.value() == 5);
        CHECK(analysis.issues[2].location.layer_index.value() == 30);
        CHECK(analysis.count(ResinIssueSeverity::Blocking) == 2);
        CHECK(analysis.has_blocking());
    }

    SECTION("a cancelled analysis keeps its findings but says they are not exhaustive")
    {
        const ResinAnalysis analysis =
            build_analysis(detected, AnalysisState::Cancelled, "hash-1");
        CHECK(analysis.state == AnalysisState::Cancelled);
        CHECK(analysis.issues.size() == 3);
        CHECK_THAT(analysis.message, Catch::Matchers::ContainsSubstring("not exhaustive"));
    }

    SECTION("an analysis is valid only for the content it was computed against")
    {
        const ResinAnalysis analysis =
            build_analysis(detected, AnalysisState::Complete, "hash-1");
        CHECK(analysis_valid_for(analysis, "hash-1"));
        CHECK_FALSE(analysis_valid_for(analysis, "hash-2"));
        // An untied analysis is never valid.
        const ResinAnalysis untied = build_analysis(detected, AnalysisState::Complete, "");
        CHECK_FALSE(analysis_valid_for(untied, "hash-1"));
    }
}

TEST_CASE("UV4: export is cleared only against a valid, complete, unblocked analysis",
          "[resin-issues]")
{
    SECTION("no blocking issues clears export")
    {
        const ResinAnalysis analysis = build_analysis(
            { issue(ResinIssueKind::Overhang, 20) }, AnalysisState::Complete, "hash-1");
        const ResinExportGate gate = evaluate_resin_export(analysis, "hash-1", {});
        CHECK(gate.may_export());
    }

    SECTION("a blocking issue blocks export and names it")
    {
        const ResinAnalysis analysis = build_analysis(
            { issue(ResinIssueKind::Island, 5) }, AnalysisState::Complete, "hash-1");
        const ResinExportGate gate = evaluate_resin_export(analysis, "hash-1", {});
        CHECK(gate.verdict == ResinExportVerdict::Blocked);
        CHECK(contains(gate.blocking, "island@5"));
    }

    SECTION("an accepted blocking issue is recorded and export is allowed")
    {
        const ResinAnalysis analysis = build_analysis(
            { issue(ResinIssueKind::Island, 5) }, AnalysisState::Complete, "hash-1");
        const ResinExportGate gate = evaluate_resin_export(analysis, "hash-1", { "island@5" });
        CHECK(gate.may_export());
        CHECK(contains(gate.accepted, "island@5"));
        CHECK_THAT(gate.message, Catch::Matchers::ContainsSubstring("accepted by override"));
    }

    SECTION("a stale analysis cannot clear export")
    {
        const ResinAnalysis analysis = build_analysis(
            { issue(ResinIssueKind::Overhang, 20) }, AnalysisState::Complete, "hash-1");
        const ResinExportGate gate = evaluate_resin_export(analysis, "hash-2", {});
        CHECK(gate.verdict == ResinExportVerdict::Unproven);
        CHECK_THAT(gate.message, Catch::Matchers::ContainsSubstring("re-run"));
    }

    SECTION("a capped analysis with no blocker found is Unproven, not Allowed")
    {
        // Not finding a blocker in an incomplete pass is not the same as there
        // being none.
        const ResinAnalysis analysis =
            build_analysis({}, AnalysisState::Capped, "hash-1");
        const ResinExportGate gate = evaluate_resin_export(analysis, "hash-1", {});
        CHECK(gate.verdict == ResinExportVerdict::Unproven);
    }

    SECTION("a cancelled analysis is likewise Unproven")
    {
        const ResinAnalysis analysis =
            build_analysis({}, AnalysisState::Cancelled, "hash-1");
        CHECK(evaluate_resin_export(analysis, "hash-1", {}).verdict ==
              ResinExportVerdict::Unproven);
    }
}

TEST_CASE("UV5: a repair is safe only if it introduces no new blocking issue", "[resin-issues]")
{
    const ResinAnalysis before = build_analysis(
        { issue(ResinIssueKind::Island, 5), issue(ResinIssueKind::Island, 6),
          issue(ResinIssueKind::Overhang, 20) },
        AnalysisState::Complete, "before");

    SECTION("removing blockers and introducing none is safe")
    {
        const ResinAnalysis after = build_analysis(
            { issue(ResinIssueKind::Overhang, 20) }, AnalysisState::Complete, "after");
        const RepairVerification result = verify_repair(before, after, "after");
        CHECK(result.safe_to_commit());
        CHECK(result.fixed.size() == 2); // both islands
        CHECK(contains(result.remaining, "overhang@20"));
        CHECK(result.introduced.empty());
    }

    SECTION("introducing a new blocking issue is a regression, however much it fixed")
    {
        // Fixes both islands but creates an empty layer.
        const ResinAnalysis after = build_analysis(
            { issue(ResinIssueKind::Overhang, 20), issue(ResinIssueKind::EmptyLayer, 6) },
            AnalysisState::Complete, "after");
        const RepairVerification result = verify_repair(before, after, "after");
        CHECK(result.verdict == RepairVerdict::Regressed);
        CHECK_FALSE(result.safe_to_commit());
        CHECK(contains(result.introduced, "empty layer@6"));
    }

    SECTION("a repair that removes nothing is ineffective, not safe")
    {
        const RepairVerification result = verify_repair(before, before, "before");
        CHECK(result.verdict == RepairVerdict::Ineffective);
        CHECK(result.fixed.empty());
    }

    SECTION("a repair verified against stale content cannot be called safe")
    {
        const ResinAnalysis after = build_analysis(
            { issue(ResinIssueKind::Overhang, 20) }, AnalysisState::Complete, "after");
        // The after-analysis is for "after", but the repaired content hashes to
        // something else - nothing can be concluded.
        const RepairVerification result = verify_repair(before, after, "different");
        CHECK(result.verdict == RepairVerdict::Ineffective);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("not valid"));
    }

    SECTION("a repair cannot be verified against an incomplete after-analysis")
    {
        const ResinAnalysis after = build_analysis(
            { issue(ResinIssueKind::Overhang, 20) }, AnalysisState::Capped, "after");
        const RepairVerification result = verify_repair(before, after, "after");
        CHECK(result.verdict == RepairVerdict::Ineffective);
    }

    SECTION("a new warning is not a regression; only new blocking issues are")
    {
        const ResinAnalysis after = build_analysis(
            { issue(ResinIssueKind::Overhang, 20), issue(ResinIssueKind::TouchingBounds, 7) },
            AnalysisState::Complete, "after");
        const RepairVerification result = verify_repair(before, after, "after");
        CHECK(result.safe_to_commit());
        // The new warning is not in the introduced (blocking) set.
        CHECK(result.introduced.empty());
    }
}

TEST_CASE("UV4/UV5: the same inputs report the same way every time", "[resin-issues]")
{
    std::vector<ResinIssue> detected = {
        issue(ResinIssueKind::ResinTrap, 12, "b"), issue(ResinIssueKind::ResinTrap, 12, "a"),
        issue(ResinIssueKind::Island, 3),
    };
    const ResinAnalysis a = build_analysis(detected, AnalysisState::Complete, "h");
    std::reverse(detected.begin(), detected.end());
    const ResinAnalysis b = build_analysis(detected, AnalysisState::Complete, "h");

    REQUIRE(a.issues.size() == b.issues.size());
    for (size_t i = 0; i < a.issues.size(); ++i)
        CHECK(a.issues[i].id() == b.issues[i].id());
}
