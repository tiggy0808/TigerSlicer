///|/ TigerSlicer Phase UV4/UV5 groundwork: the resin issue model, export gate,
///|/ and repair verification.
///|/
///|/ The TigerSlicer-native decision layer over UVtools' analysis and repair
///|/ operations: how an issue is classified, whether it blocks export, when an
///|/ analysis is still valid, and whether a repair is safe to commit. This is
///|/ testable libslic3r logic; it runs no UVtools analysis and links no UVtools
///|/ code. The actual detection and repair execution live behind the UV1
///|/ bridge (see doc/TigerSlicerContinuationRoadmap.md).
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ResinIssueReport_hpp_
#define slic3r_ResinIssueReport_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// The audited UVtools checks meaningful before export. The kind is what the
// analysis found; the severity below is TigerSlicer's decision about it.
enum class ResinIssueKind : uint8_t
{
    Island,
    Overhang,
    ResinTrap,
    SuctionCup,
    TouchingBounds,
    ExcessiveHeight,
    EmptyLayer,
    AnomalousLayer,
    InvalidLayerData,
    IncompatibleParameters,
};

const char *resin_issue_kind_name(ResinIssueKind kind);

// TigerSlicer's severity decision. Blocking stops export by default; a
// Blocking issue that is structurally unsafe (invalid data, an island that
// will fall) is distinguished from a Warning the user may knowingly accept.
enum class ResinIssueSeverity : uint8_t
{
    Info,
    Warning,
    Blocking,
};

const char *resin_issue_severity_name(ResinIssueSeverity severity);

// Where an issue is, so selecting it can navigate the layer preview. Every
// field optional because a whole-job issue (excessive height) has no single
// layer, and an analysis that could not localize an issue says so rather than
// pointing at layer zero.
struct ResinIssueLocation
{
    std::optional<uint32_t> layer_index;
    std::optional<double>   z_mm;
    // A human description of the affected region, for the panel.
    std::string region;
};

struct ResinIssue
{
    ResinIssueKind     kind { ResinIssueKind::Island };
    ResinIssueSeverity severity { ResinIssueSeverity::Warning };
    ResinIssueLocation location;

    std::string explanation;
    std::string suggested_action;

    // A stable identity for this issue, so a repair can say it fixed *this*
    // one and an analysis can be diffed against a later one. Two issues of the
    // same kind at the same location are the same issue.
    std::string id() const;
};

// The severity TigerSlicer assigns a raw detected issue. This is the policy:
// invalid or corrupt data and anomalous/empty layers are structurally unsafe
// and Blocking; islands and resin traps are Blocking because they fail a
// print; overhangs and touching-bounds are Warnings the user may accept;
// incompatible parameters block because the file will not print correctly.
ResinIssueSeverity classify_issue(ResinIssueKind kind);

// The outcome of an analysis pass over a job.
enum class AnalysisState : uint8_t
{
    Complete,
    // The user cancelled a long analysis; partial results are kept but never
    // presented as a clean bill of health.
    Cancelled,
    // The analysis hit its cap; what it found stands, but it is not exhaustive.
    Capped,
    // The analysis could not run at all.
    Failed,
};

const char *analysis_state_name(AnalysisState state);

struct ResinAnalysis
{
    AnalysisState state { AnalysisState::Failed };

    // Sorted by (layer, kind), so the panel and any diff are stable.
    std::vector<ResinIssue> issues;

    // The content hash the analysis was computed against. An analysis is only
    // valid for the job whose content hashes to this. Empty means it is not
    // tied to any content and must be treated as stale.
    std::string content_hash;

    std::string message;

    size_t count(ResinIssueSeverity severity) const;
    bool   has_blocking() const;
};

// Build an analysis result from raw detected issues: assign each its severity,
// sort, and record the content hash it is valid for. Nothing here detects
// anything; it turns a detector's findings into TigerSlicer's report.
ResinAnalysis build_analysis(const std::vector<ResinIssue> &detected, AnalysisState state,
                             const std::string &content_hash);

// Whether an analysis may still be trusted for a job with this content hash.
// An analysis computed against different content, or against none, is stale -
// stale analysis presented as current is exactly what UV4 forbids.
bool analysis_valid_for(const ResinAnalysis &analysis, const std::string &current_content_hash);

// --- export gate ------------------------------------------------------------

enum class ResinExportVerdict : uint8_t
{
    Allowed,
    // Blocking issues remain and were not acknowledged.
    Blocked,
    // The analysis is stale or incomplete, so export cannot be cleared.
    Unproven,
};

const char *resin_export_verdict_name(ResinExportVerdict verdict);

struct ResinExportGate
{
    ResinExportVerdict verdict { ResinExportVerdict::Unproven };
    // Blocking issues that still stand.
    std::vector<std::string> blocking;
    // Blocking issues an expert explicitly accepted, recorded for the manifest.
    std::vector<std::string> accepted;
    std::string message;

    bool may_export() const { return verdict == ResinExportVerdict::Allowed; }
};

// Decide whether export may proceed. Export is cleared only when the analysis
// is valid for the current content, complete (not cancelled/capped/failed),
// and every Blocking issue is either absent or explicitly acknowledged by id.
// A capped or cancelled analysis is Unproven, never Allowed: not finding a
// blocker is not the same as there being none.
ResinExportGate evaluate_resin_export(const ResinAnalysis            &analysis,
                                      const std::string              &current_content_hash,
                                      const std::vector<std::string> &acknowledged_blocking);

// --- repair verification (UV5) ---------------------------------------------

enum class RepairVerdict : uint8_t
{
    // The repair fixed issues and introduced no new Blocking one.
    Safe,
    // The repair introduced a new Blocking issue; it must not be committed.
    Regressed,
    // The repair changed nothing, or its after-analysis is stale.
    Ineffective,
};

const char *repair_verdict_name(RepairVerdict verdict);

struct RepairVerification
{
    RepairVerdict verdict { RepairVerdict::Ineffective };

    // Issue ids the repair removed, that it left, and - the decisive set - the
    // Blocking ones it introduced.
    std::vector<std::string> fixed;
    std::vector<std::string> remaining;
    std::vector<std::string> introduced;

    std::string message;

    bool safe_to_commit() const { return verdict == RepairVerdict::Safe; }
};

// Verify a repair by comparing the analysis before it against the analysis
// after. A repair is Safe only when it introduces no new Blocking issue and
// removes at least one; a repair that adds any Blocking issue is Regressed and
// must not be committed, however many others it fixed. Both analyses must be
// complete and the after-analysis must be valid for the repaired content, or
// the verdict is Ineffective rather than an unverified Safe.
RepairVerification verify_repair(const ResinAnalysis &before, const ResinAnalysis &after,
                                 const std::string &repaired_content_hash);

} // namespace Slic3r

#endif // slic3r_ResinIssueReport_hpp_
