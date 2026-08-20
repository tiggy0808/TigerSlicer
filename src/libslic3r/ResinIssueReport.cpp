///|/ TigerSlicer Phase UV4/UV5 groundwork: the resin issue model, export gate,
///|/ and repair verification.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ResinIssueReport.hpp"

#include <algorithm>
#include <set>

namespace Slic3r {

const char *resin_issue_kind_name(ResinIssueKind kind)
{
    switch (kind) {
    case ResinIssueKind::Island:                 return "island";
    case ResinIssueKind::Overhang:               return "overhang";
    case ResinIssueKind::ResinTrap:              return "resin trap";
    case ResinIssueKind::SuctionCup:             return "suction cup";
    case ResinIssueKind::TouchingBounds:         return "touching build bounds";
    case ResinIssueKind::ExcessiveHeight:        return "excessive print height";
    case ResinIssueKind::EmptyLayer:             return "empty layer";
    case ResinIssueKind::AnomalousLayer:         return "anomalous layer";
    case ResinIssueKind::InvalidLayerData:       return "invalid layer data";
    case ResinIssueKind::IncompatibleParameters: return "incompatible parameters";
    }
    return "island";
}

const char *resin_issue_severity_name(ResinIssueSeverity severity)
{
    switch (severity) {
    case ResinIssueSeverity::Info:     return "info";
    case ResinIssueSeverity::Warning:  return "warning";
    case ResinIssueSeverity::Blocking: return "blocking";
    }
    return "warning";
}

const char *analysis_state_name(AnalysisState state)
{
    switch (state) {
    case AnalysisState::Complete:  return "complete";
    case AnalysisState::Cancelled: return "cancelled";
    case AnalysisState::Capped:    return "capped";
    case AnalysisState::Failed:    return "failed";
    }
    return "failed";
}

const char *resin_export_verdict_name(ResinExportVerdict verdict)
{
    switch (verdict) {
    case ResinExportVerdict::Allowed:  return "allowed";
    case ResinExportVerdict::Blocked:  return "blocked";
    case ResinExportVerdict::Unproven: return "unproven";
    }
    return "unproven";
}

const char *repair_verdict_name(RepairVerdict verdict)
{
    switch (verdict) {
    case RepairVerdict::Safe:        return "safe";
    case RepairVerdict::Regressed:   return "regressed";
    case RepairVerdict::Ineffective: return "ineffective";
    }
    return "ineffective";
}

std::string ResinIssue::id() const
{
    // Kind plus location. Two issues of the same kind at the same place are the
    // same issue, so a repair can name exactly the one it removed.
    std::string out = resin_issue_kind_name(kind);
    out += '@';
    out += location.layer_index.has_value() ? std::to_string(*location.layer_index)
                                            : std::string("job");
    if (! location.region.empty()) {
        out += '#';
        out += location.region;
    }
    return out;
}

ResinIssueSeverity classify_issue(ResinIssueKind kind)
{
    switch (kind) {
    // Structurally unsafe or the print fails: these block by default.
    case ResinIssueKind::Island:                 return ResinIssueSeverity::Blocking;
    case ResinIssueKind::ResinTrap:              return ResinIssueSeverity::Blocking;
    case ResinIssueKind::EmptyLayer:             return ResinIssueSeverity::Blocking;
    case ResinIssueKind::AnomalousLayer:         return ResinIssueSeverity::Blocking;
    case ResinIssueKind::InvalidLayerData:       return ResinIssueSeverity::Blocking;
    case ResinIssueKind::IncompatibleParameters: return ResinIssueSeverity::Blocking;
    // A suction cup can lift a part off the plate; block, but it is a mechanics
    // problem the user may understand better than the detector.
    case ResinIssueKind::SuctionCup:             return ResinIssueSeverity::Blocking;
    // Quality or fit warnings the user may knowingly accept.
    case ResinIssueKind::Overhang:               return ResinIssueSeverity::Warning;
    case ResinIssueKind::TouchingBounds:         return ResinIssueSeverity::Warning;
    case ResinIssueKind::ExcessiveHeight:        return ResinIssueSeverity::Warning;
    }
    return ResinIssueSeverity::Warning;
}

namespace {

void sort_issues(std::vector<ResinIssue> &issues)
{
    std::sort(issues.begin(), issues.end(), [](const ResinIssue &a, const ResinIssue &b) {
        const uint32_t la = a.location.layer_index.value_or(UINT32_MAX);
        const uint32_t lb = b.location.layer_index.value_or(UINT32_MAX);
        if (la != lb)
            return la < lb;
        if (a.kind != b.kind)
            return uint8_t(a.kind) < uint8_t(b.kind);
        return a.location.region < b.location.region;
    });
}

} // namespace

ResinAnalysis build_analysis(const std::vector<ResinIssue> &detected, AnalysisState state,
                             const std::string &content_hash)
{
    ResinAnalysis analysis;
    analysis.state        = state;
    analysis.content_hash = content_hash;
    analysis.issues       = detected;

    // Assign TigerSlicer's severity to each, overriding whatever the detector
    // supplied so the policy lives in one place.
    for (ResinIssue &issue : analysis.issues)
        issue.severity = classify_issue(issue.kind);

    sort_issues(analysis.issues);

    switch (state) {
    case AnalysisState::Complete:
        analysis.message = std::to_string(analysis.issues.size()) + " issue(s) found";
        break;
    case AnalysisState::Cancelled:
        analysis.message = "analysis cancelled; the " + std::to_string(analysis.issues.size()) +
                           " issue(s) found so far are not exhaustive";
        break;
    case AnalysisState::Capped:
        analysis.message = "analysis capped; more issues may exist beyond the " +
                           std::to_string(analysis.issues.size()) + " reported";
        break;
    case AnalysisState::Failed:
        analysis.message = "analysis could not run";
        break;
    }
    return analysis;
}

size_t ResinAnalysis::count(ResinIssueSeverity severity) const
{
    size_t n = 0;
    for (const ResinIssue &issue : issues)
        if (issue.severity == severity)
            ++n;
    return n;
}

bool ResinAnalysis::has_blocking() const
{
    return count(ResinIssueSeverity::Blocking) > 0;
}

bool analysis_valid_for(const ResinAnalysis &analysis, const std::string &current_content_hash)
{
    // No hash on either side is never a match: an untied analysis is stale.
    if (analysis.content_hash.empty() || current_content_hash.empty())
        return false;
    return analysis.content_hash == current_content_hash;
}

// --- export gate ------------------------------------------------------------

ResinExportGate evaluate_resin_export(const ResinAnalysis            &analysis,
                                      const std::string              &current_content_hash,
                                      const std::vector<std::string> &acknowledged_blocking)
{
    ResinExportGate gate;

    if (! analysis_valid_for(analysis, current_content_hash)) {
        gate.verdict = ResinExportVerdict::Unproven;
        gate.message = "the analysis is not valid for the current content; re-run it before "
                       "export";
        return gate;
    }
    if (analysis.state != AnalysisState::Complete) {
        // A cancelled or capped analysis found what it found, but not finding a
        // blocker in an incomplete pass is not the same as there being none.
        gate.verdict = ResinExportVerdict::Unproven;
        gate.message = "the analysis is " + std::string(analysis_state_name(analysis.state)) +
                       ", so the absence of a blocking issue is not established";
        return gate;
    }

    std::set<std::string> acknowledged(acknowledged_blocking.begin(),
                                       acknowledged_blocking.end());
    size_t unacknowledged = 0;
    for (const ResinIssue &issue : analysis.issues) {
        if (issue.severity != ResinIssueSeverity::Blocking)
            continue;
        if (acknowledged.count(issue.id()))
            gate.accepted.push_back(issue.id());
        else {
            gate.blocking.push_back(issue.id());
            ++unacknowledged;
        }
    }

    if (unacknowledged > 0) {
        gate.verdict = ResinExportVerdict::Blocked;
        gate.message = std::to_string(unacknowledged) +
                       " blocking issue(s) must be resolved or explicitly accepted before "
                       "export";
    } else if (! gate.accepted.empty()) {
        gate.verdict = ResinExportVerdict::Allowed;
        gate.message = std::to_string(gate.accepted.size()) +
                       " blocking issue(s) were accepted by override and recorded; export is "
                       "allowed";
    } else {
        gate.verdict = ResinExportVerdict::Allowed;
        gate.message = "no blocking issues; export is allowed";
    }
    return gate;
}

// --- repair verification ----------------------------------------------------

namespace {

std::set<std::string> blocking_ids(const ResinAnalysis &analysis)
{
    std::set<std::string> ids;
    for (const ResinIssue &issue : analysis.issues)
        if (issue.severity == ResinIssueSeverity::Blocking)
            ids.insert(issue.id());
    return ids;
}

std::set<std::string> all_ids(const ResinAnalysis &analysis)
{
    std::set<std::string> ids;
    for (const ResinIssue &issue : analysis.issues)
        ids.insert(issue.id());
    return ids;
}

} // namespace

RepairVerification verify_repair(const ResinAnalysis &before, const ResinAnalysis &after,
                                 const std::string &repaired_content_hash)
{
    RepairVerification result;

    // Both analyses must be trustworthy, and the after-analysis must describe
    // the repaired content. Otherwise nothing can be concluded.
    if (before.state != AnalysisState::Complete || after.state != AnalysisState::Complete ||
        ! analysis_valid_for(after, repaired_content_hash)) {
        result.verdict = RepairVerdict::Ineffective;
        result.message = "the repair cannot be verified: an analysis is incomplete or the "
                         "after-analysis is not valid for the repaired content";
        return result;
    }

    const std::set<std::string> before_all      = all_ids(before);
    const std::set<std::string> after_all       = all_ids(after);
    const std::set<std::string> before_blocking = blocking_ids(before);
    const std::set<std::string> after_blocking  = blocking_ids(after);

    for (const std::string &id : before_all)
        if (! after_all.count(id))
            result.fixed.push_back(id);
    for (const std::string &id : after_all)
        if (before_all.count(id))
            result.remaining.push_back(id);

    // The decisive set: Blocking issues present after that were not present
    // before. A repair that adds one of these is unsafe whatever else it did.
    for (const std::string &id : after_blocking)
        if (! before_blocking.count(id))
            result.introduced.push_back(id);

    std::sort(result.fixed.begin(), result.fixed.end());
    std::sort(result.remaining.begin(), result.remaining.end());
    std::sort(result.introduced.begin(), result.introduced.end());

    if (! result.introduced.empty()) {
        result.verdict = RepairVerdict::Regressed;
        result.message = "the repair introduced " + std::to_string(result.introduced.size()) +
                         " new blocking issue(s); it must not be committed";
    } else if (result.fixed.empty()) {
        result.verdict = RepairVerdict::Ineffective;
        result.message = "the repair removed no issue";
    } else {
        result.verdict = RepairVerdict::Safe;
        result.message = "the repair removed " + std::to_string(result.fixed.size()) +
                         " issue(s) and introduced no blocking issue";
    }
    return result;
}

} // namespace Slic3r
