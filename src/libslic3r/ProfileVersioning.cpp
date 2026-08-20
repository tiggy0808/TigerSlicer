///|/ TigerSlicer Phase 27: profile versioning and regression comparison.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ProfileVersioning.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r {

const char *profile_kind_name(ProfileKind kind)
{
    switch (kind) {
    case ProfileKind::Printer:     return "printer";
    case ProfileKind::Print:       return "print";
    case ProfileKind::Filament:    return "filament";
    case ProfileKind::Tool:        return "tool";
    case ProfileKind::Calibration: return "calibration";
    case ProfileKind::Transition:  return "transition";
    }
    return "unknown";
}

const char *profile_source_name(ProfileSource source)
{
    switch (source) {
    case ProfileSource::User:        return "user";
    case ProfileSource::Vendor:      return "vendor";
    case ProfileSource::Imported:    return "imported";
    case ProfileSource::Calibration: return "calibration";
    case ProfileSource::Migration:   return "migration";
    case ProfileSource::Unknown:     return "unknown";
    }
    return "unknown";
}

const char *compatibility_verdict_name(CompatibilityVerdict verdict)
{
    switch (verdict) {
    case CompatibilityVerdict::Compatible:   return "compatible";
    case CompatibilityVerdict::Incompatible: return "incompatible";
    case CompatibilityVerdict::Unproven:     return "unproven";
    }
    return "unproven";
}

const char *validation_state_name(ValidationState state)
{
    switch (state) {
    case ValidationState::Unvalidated: return "unvalidated";
    case ValidationState::Valid:       return "valid";
    case ValidationState::Invalid:     return "invalid";
    }
    return "unvalidated";
}

const char *history_error_name(HistoryError error)
{
    switch (error) {
    case HistoryError::None:                 return "none";
    case HistoryError::UnknownProfile:       return "unknown profile";
    case HistoryError::UnknownRevision:      return "unknown revision";
    case HistoryError::DuplicateRevision:    return "duplicate revision";
    case HistoryError::NonMonotonicRevision: return "non-monotonic revision";
    case HistoryError::MissingParent:        return "missing parent";
    case HistoryError::ParentMismatch:       return "parent mismatch";
    case HistoryError::KindMismatch:         return "kind mismatch";
    case HistoryError::Immutable:            return "immutable";
    case HistoryError::EmptyIdentifier:      return "empty identifier";
    }
    return "none";
}

const char *change_direction_name(ChangeDirection direction)
{
    switch (direction) {
    case ChangeDirection::Better:    return "better";
    case ChangeDirection::Worse:     return "worse";
    case ChangeDirection::Unchanged: return "unchanged";
    case ChangeDirection::Unknown:   return "unknown";
    }
    return "unknown";
}

const char *regression_verdict_name(RegressionVerdict verdict)
{
    switch (verdict) {
    case RegressionVerdict::Improvement: return "improvement";
    case RegressionVerdict::Regression:  return "regression";
    case RegressionVerdict::Mixed:       return "mixed";
    case RegressionVerdict::Neutral:     return "neutral";
    case RegressionVerdict::Unproven:    return "unproven";
    }
    return "unproven";
}

const char *reference_status_name(ReferenceStatus status)
{
    switch (status) {
    case ReferenceStatus::Resolved:        return "resolved";
    case ReferenceStatus::NameOnly:        return "name only";
    case ReferenceStatus::MissingProfile:  return "missing profile";
    case ReferenceStatus::MissingRevision: return "missing revision";
    case ReferenceStatus::Superseded:      return "superseded";
    case ReferenceStatus::Unvalidated:     return "unvalidated";
    case ReferenceStatus::Invalid:         return "invalid";
    }
    return "missing profile";
}

std::string ProfileVersionKey::to_string() const
{
    std::string out = profile_kind_name(kind);
    out += ':';
    out += profile_id.empty() ? "<unnamed>" : profile_id;
    out += '@';
    out += revision > 0 ? std::to_string(revision) : std::string("<none>");
    return out;
}

bool ProfileVersionKey::operator<(const ProfileVersionKey &o) const
{
    if (kind != o.kind)
        return uint8_t(kind) < uint8_t(o.kind);
    if (profile_id != o.profile_id)
        return profile_id < o.profile_id;
    return revision < o.revision;
}

// --- archive ---------------------------------------------------------------

HistoryResult ProfileArchive::add(const ProfileVersionRecord &record)
{
    HistoryResult result;

    if (record.key.profile_id.empty()) {
        result.error   = HistoryError::EmptyIdentifier;
        result.message = "a version was offered with no profile identifier";
        return result;
    }
    if (record.key.revision <= 0) {
        result.error   = HistoryError::UnknownRevision;
        result.message = "revision numbers start at 1; " + std::to_string(record.key.revision) +
                         " does not identify a version";
        return result;
    }

    if (m_records.count(record.key)) {
        // Deliberately refused even when the contents are identical. A caller
        // that writes the same revision twice has lost track of its history,
        // and letting it through hides that.
        result.error   = HistoryError::DuplicateRevision;
        result.message = record.key.to_string() + " already exists and versions are immutable";
        return result;
    }

    const ProfileVersionRecord *current_head = head(record.key.kind, record.key.profile_id);

    if (record.key.revision == 1) {
        if (record.parent_revision.has_value()) {
            result.error   = HistoryError::ParentMismatch;
            result.message = "revision 1 of " + record.key.profile_id +
                             " cannot have a parent revision within its own history";
            return result;
        }
    } else {
        if (current_head == nullptr) {
            result.error = HistoryError::MissingParent;
            result.message = record.key.to_string() +
                             " has no earlier revision; a history cannot start at " +
                             std::to_string(record.key.revision);
            return result;
        }
        if (record.key.revision <= current_head->key.revision) {
            result.error   = HistoryError::NonMonotonicRevision;
            result.message = record.key.to_string() + " is not after the current head " +
                             current_head->key.to_string();
            return result;
        }
        if (! record.parent_revision.has_value()) {
            result.error = HistoryError::MissingParent;
            result.message = record.key.to_string() +
                             " does not say which revision it was edited from";
            return result;
        }
        const ProfileVersionKey parent_key { record.key.kind, record.key.profile_id,
                                             *record.parent_revision };
        if (! m_records.count(parent_key)) {
            result.error   = HistoryError::MissingParent;
            result.message = record.key.to_string() + " names parent revision " +
                             std::to_string(*record.parent_revision) + ", which does not exist";
            return result;
        }
    }

    if (record.restores_revision.has_value()) {
        const ProfileVersionKey restored { record.key.kind, record.key.profile_id,
                                           *record.restores_revision };
        if (! m_records.count(restored)) {
            result.error   = HistoryError::UnknownRevision;
            result.message = record.key.to_string() + " claims to restore revision " +
                             std::to_string(*record.restores_revision) + ", which does not exist";
            return result;
        }
    }

    if (record.derived_from.has_value() && record.derived_from->complete() &&
        ! m_records.count(*record.derived_from)) {
        result.error   = HistoryError::UnknownRevision;
        result.message = record.key.to_string() + " says it was derived from " +
                         record.derived_from->to_string() + ", which is not in this archive";
        return result;
    }

    m_records.emplace(record.key, record);
    result.written = record.key;
    return result;
}

const ProfileVersionRecord *ProfileArchive::find(const ProfileVersionKey &key) const
{
    auto it = m_records.find(key);
    return it == m_records.end() ? nullptr : &it->second;
}

const ProfileVersionRecord *ProfileArchive::head(ProfileKind kind,
                                                 const std::string &profile_id) const
{
    const ProfileVersionRecord *best = nullptr;
    for (const auto &entry : m_records) {
        if (entry.first.kind != kind || entry.first.profile_id != profile_id)
            continue;
        if (best == nullptr || entry.first.revision > best->key.revision)
            best = &entry.second;
    }
    return best;
}

std::vector<const ProfileVersionRecord *> ProfileArchive::history(
    ProfileKind kind, const std::string &profile_id) const
{
    std::vector<const ProfileVersionRecord *> out;
    for (const auto &entry : m_records)
        if (entry.first.kind == kind && entry.first.profile_id == profile_id)
            out.push_back(&entry.second);
    // The map is already ordered by (kind, id, revision), so this is ascending
    // by revision; sorting again keeps that true if the container ever changes.
    std::sort(out.begin(), out.end(), [](const ProfileVersionRecord *a,
                                         const ProfileVersionRecord *b) {
        return a->key.revision < b->key.revision;
    });
    return out;
}

std::vector<std::string> ProfileArchive::profiles(ProfileKind kind) const
{
    std::set<std::string> seen;
    for (const auto &entry : m_records)
        if (entry.first.kind == kind)
            seen.insert(entry.first.profile_id);
    return std::vector<std::string>(seen.begin(), seen.end());
}

// --- diffs -----------------------------------------------------------------

namespace {

std::vector<ProfileFieldChange> field_changes(const std::map<std::string, std::string> &before,
                                              const std::map<std::string, std::string> &after)
{
    std::set<std::string> names;
    for (const auto &f : before)
        names.insert(f.first);
    for (const auto &f : after)
        names.insert(f.first);

    std::vector<ProfileFieldChange> out;
    for (const std::string &name : names) {
        auto b = before.find(name);
        auto a = after.find(name);
        const bool has_b = b != before.end();
        const bool has_a = a != after.end();
        if (has_b && has_a && b->second == a->second)
            continue;

        ProfileFieldChange change;
        change.field = name;
        if (has_b)
            change.before = b->second;
        if (has_a)
            change.after = a->second;
        out.push_back(change);
    }
    // Sorted, because a std::set was walked in order; a diff must read the
    // same way every time it is taken.
    return out;
}

} // namespace

ProfileDiff diff_versions(const ProfileVersionRecord &before, const ProfileVersionRecord &after)
{
    ProfileDiff diff;
    diff.before = before.key;
    diff.after  = after.key;

    if (before.key.kind != after.key.kind) {
        diff.comparable = false;
        diff.incomparable_reason = std::string("a ") + profile_kind_name(before.key.kind) +
                                   " profile and a " + profile_kind_name(after.key.kind) +
                                   " profile do not describe the same thing";
        return diff;
    }

    if (before.key.profile_id != after.key.profile_id) {
        // Two different profiles are still comparable when one was duplicated
        // from the other: that is exactly the case a reviewer wants a diff
        // for. Otherwise the field lists happen to share names and mean
        // nothing to each other.
        const bool related =
            (after.derived_from.has_value() &&
             after.derived_from->profile_id == before.key.profile_id) ||
            (before.derived_from.has_value() &&
             before.derived_from->profile_id == after.key.profile_id);
        if (! related) {
            diff.comparable = false;
            diff.incomparable_reason = before.key.profile_id + " and " + after.key.profile_id +
                                       " are unrelated profiles; neither was derived from the "
                                       "other, so a field diff would not mean anything";
            return diff;
        }
    }

    diff.changes = field_changes(before.fields, after.fields);
    return diff;
}

std::vector<ProfileDiff> diff_history(const ProfileArchive &archive, ProfileKind kind,
                                      const std::string &profile_id, int from_revision,
                                      int to_revision)
{
    std::vector<ProfileDiff> out;
    if (from_revision >= to_revision)
        return out;

    const std::vector<const ProfileVersionRecord *> all = archive.history(kind, profile_id);
    const ProfileVersionRecord                     *previous = nullptr;
    for (const ProfileVersionRecord *record : all) {
        if (record->key.revision < from_revision || record->key.revision > to_revision)
            continue;
        if (previous != nullptr)
            out.push_back(diff_versions(*previous, *record));
        previous = record;
    }
    return out;
}

// --- validation ------------------------------------------------------------

ValidationOutcome validate_version(const ProfileVersionRecord &record,
                                   const std::vector<const ProfileVersionRecord *> &context)
{
    ValidationOutcome outcome;

    bool invalid = false;

    if (record.key.profile_id.empty()) {
        outcome.messages.push_back("the version has no profile identifier");
        invalid = true;
    }
    if (record.key.revision <= 0) {
        outcome.messages.push_back("the version has no revision number");
        invalid = true;
    }
    if (record.fields.empty()) {
        outcome.messages.push_back("the version carries no fields, so there is nothing to apply");
        invalid = true;
    }
    if (record.reason.empty())
        outcome.messages.push_back(
            "no reason was recorded for this version, so nobody can review why it exists");
    if (record.source == ProfileSource::Unknown)
        outcome.messages.push_back(
            "the source of this version is unknown, so its values cannot be attributed");
    if (record.timestamp_s <= 0)
        outcome.messages.push_back("the version has no timestamp, so it cannot be placed in time");

    if (record.requires_of_others.empty()) {
        outcome.compatibility = CompatibilityVerdict::Unproven;
        outcome.unchecked.push_back(
            "this version declares no requirements of other profiles, so nothing about its "
            "compatibility has been shown");
    } else {
        size_t satisfied = 0;
        bool   conflict  = false;
        for (const CompatibilityRequirement &requirement : record.requires_of_others) {
            const ProfileVersionRecord *target = nullptr;
            for (const ProfileVersionRecord *candidate : context)
                if (candidate != nullptr && candidate->key.kind == requirement.target_kind) {
                    target = candidate;
                    break;
                }

            const std::string label = std::string(profile_kind_name(requirement.target_kind)) +
                                      "." + requirement.field;

            if (target == nullptr) {
                outcome.unchecked.push_back(label + ": no " +
                                            profile_kind_name(requirement.target_kind) +
                                            " profile was supplied to check against");
                continue;
            }
            auto field = target->fields.find(requirement.field);
            if (field == target->fields.end()) {
                outcome.unchecked.push_back(label + ": " + target->key.to_string() +
                                            " does not have that field, so the requirement could "
                                            "not be checked");
                continue;
            }
            if (! requirement.equals.has_value()) {
                ++satisfied;
                continue;
            }
            if (field->second == *requirement.equals) {
                ++satisfied;
            } else {
                conflict = true;
                outcome.messages.push_back(label + " is \"" + field->second + "\" but " +
                                           record.key.to_string() + " requires \"" +
                                           *requirement.equals + "\"" +
                                           (requirement.why.empty() ? std::string()
                                                                    : ": " + requirement.why));
            }
        }

        if (conflict)
            outcome.compatibility = CompatibilityVerdict::Incompatible;
        else if (satisfied == record.requires_of_others.size())
            outcome.compatibility = CompatibilityVerdict::Compatible;
        else
            // Some requirement could not be checked. Partially checked is not
            // compatible, and saying so is the whole point of the verdict.
            outcome.compatibility = CompatibilityVerdict::Unproven;
    }

    if (outcome.compatibility == CompatibilityVerdict::Incompatible)
        invalid = true;

    if (invalid)
        outcome.state = ValidationState::Invalid;
    else if (outcome.compatibility == CompatibilityVerdict::Compatible && outcome.messages.empty())
        outcome.state = ValidationState::Valid;
    else if (outcome.messages.empty() && outcome.unchecked.empty())
        outcome.state = ValidationState::Valid;
    else
        // Everything that could be checked passed, but not everything could be
        // checked. That is not validated.
        outcome.state = ValidationState::Unvalidated;

    return outcome;
}

// --- regression comparison -------------------------------------------------

namespace {

struct MetricSpec
{
    const char *name;
    std::optional<double> OutcomeSample::*field;
    MetricDirection direction;
    double RegressionPolicy::*threshold;
};

// The fixed order every report is written in.
const MetricSpec METRICS[] = {
    { "time", &OutcomeSample::time_s, MetricDirection::LowerIsBetter,
      &RegressionPolicy::time_s_threshold },
    { "toolchanges", &OutcomeSample::toolchanges, MetricDirection::LowerIsBetter,
      &RegressionPolicy::toolchange_threshold },
    { "waste", &OutcomeSample::waste_mm3, MetricDirection::LowerIsBetter,
      &RegressionPolicy::waste_mm3_threshold },
    { "flow utilization", &OutcomeSample::flow_utilization, MetricDirection::HigherIsBetter,
      &RegressionPolicy::flow_threshold },
    { "surface quality", &OutcomeSample::surface_quality, MetricDirection::HigherIsBetter,
      &RegressionPolicy::surface_threshold },
    { "warnings", &OutcomeSample::warnings, MetricDirection::LowerIsBetter,
      &RegressionPolicy::warnings_threshold },
};

} // namespace

RegressionReport compare_versions(const ProfileVersionRecord &before_version,
                                  const OutcomeSample        &before_sample,
                                  const ProfileVersionRecord &after_version,
                                  const OutcomeSample        &after_sample,
                                  const RegressionPolicy     &policy)
{
    RegressionReport report;
    report.before = before_version.key;
    report.after  = after_version.key;
    report.diff   = diff_versions(before_version, after_version);

    if (! report.diff.comparable)
        report.notes.push_back("the two versions could not be diffed: " +
                               report.diff.incomparable_reason);

    for (const MetricSpec &spec : METRICS) {
        MetricComparison comparison;
        comparison.metric    = spec.name;
        comparison.direction = spec.direction;
        comparison.before    = before_sample.*(spec.field);
        comparison.after     = after_sample.*(spec.field);

        if (! comparison.before.has_value() || ! comparison.after.has_value()) {
            comparison.change = ChangeDirection::Unknown;
            comparison.unmeasured_reason =
                ! comparison.before.has_value() && ! comparison.after.has_value()
                    ? "neither version measured it"
                    : (comparison.before.has_value() ? "the new version did not measure it"
                                                     : "the old version did not measure it");
            ++report.unknown;
            report.metrics.push_back(comparison);
            continue;
        }

        const double delta = *comparison.after - *comparison.before;
        comparison.delta   = delta;
        if (std::abs(*comparison.before) > 1e-12)
            comparison.relative = delta / *comparison.before;

        const double threshold = std::abs(policy.*(spec.threshold));
        if (std::abs(delta) < threshold) {
            comparison.change      = ChangeDirection::Unchanged;
            comparison.significant = false;
            ++report.unchanged;
        } else {
            comparison.significant = true;
            const bool better = spec.direction == MetricDirection::LowerIsBetter ? delta < 0.
                                                                                 : delta > 0.;
            comparison.change = better ? ChangeDirection::Better : ChangeDirection::Worse;
            if (better)
                ++report.improved;
            else
                ++report.regressed;
        }

        report.metrics.push_back(comparison);
    }

    // A comparison is only as strong as the weaker of the two samples. A
    // sample that never stated a confidence is the weakest of all, because
    // nobody said how far to trust it.
    const bool unstated =
        ! before_sample.confidence.has_value() || ! after_sample.confidence.has_value();
    if (! unstated)
        report.confidence = std::min(*before_sample.confidence, *after_sample.confidence);

    if (! before_sample.provenance.empty())
        report.notes.push_back("old measurements: " + before_sample.provenance);
    if (! after_sample.provenance.empty())
        report.notes.push_back("new measurements: " + after_sample.provenance);

    const size_t compared = report.improved + report.regressed + report.unchanged;

    if (compared == 0) {
        report.verdict = RegressionVerdict::Unproven;
        report.unproven_reason =
            "no metric was measured on both versions, so nothing was actually compared";
    } else if (unstated && policy.unstated_confidence_is_weak) {
        report.verdict = RegressionVerdict::Unproven;
        report.unproven_reason =
            "at least one sample did not state how far it can be trusted, so the numbers do not "
            "support a claim about the change";
    } else if (report.confidence.has_value() && *report.confidence < policy.minimum_confidence) {
        report.verdict = RegressionVerdict::Unproven;
        report.unproven_reason =
            "the weaker sample's confidence is below what this comparison requires";
    } else if (report.regressed > 0 && report.improved > 0) {
        report.verdict = RegressionVerdict::Mixed;
    } else if (report.regressed > 0) {
        report.verdict = RegressionVerdict::Regression;
    } else if (report.improved > 0) {
        report.verdict = RegressionVerdict::Improvement;
    } else {
        report.verdict = RegressionVerdict::Neutral;
    }

    if (report.unknown > 0)
        report.notes.push_back(std::to_string(report.unknown) +
                               " metric(s) could not be compared and are not counted in the "
                               "verdict");

    return report;
}

// --- rollback and duplication ---------------------------------------------

RollbackResult plan_rollback(const ProfileArchive &archive, ProfileKind kind,
                             const std::string &profile_id, int to_revision,
                             const std::string &reason, int64_t timestamp_s)
{
    RollbackResult result;

    const ProfileVersionRecord *current = archive.head(kind, profile_id);
    if (current == nullptr) {
        result.error   = HistoryError::UnknownProfile;
        result.message = std::string("no ") + profile_kind_name(kind) + " profile named " +
                         profile_id + " has any history to roll back";
        return result;
    }

    const ProfileVersionKey     target_key { kind, profile_id, to_revision };
    const ProfileVersionRecord *target = archive.find(target_key);
    if (target == nullptr) {
        result.error   = HistoryError::UnknownRevision;
        result.message = "cannot roll back to " + target_key.to_string() +
                         ": that revision does not exist";
        return result;
    }
    if (to_revision == current->key.revision) {
        result.error   = HistoryError::DuplicateRevision;
        result.message = target_key.to_string() +
                         " is already the current version; there is nothing to roll back";
        return result;
    }

    ProfileVersionRecord record;
    record.key               = { kind, profile_id, current->key.revision + 1 };
    record.parent_revision   = current->key.revision;
    record.restores_revision = to_revision;
    record.fields            = target->fields;
    record.requires_of_others = target->requires_of_others;
    record.reason = reason.empty() ? ("rolled back to revision " + std::to_string(to_revision))
                                   : reason;
    record.source       = ProfileSource::User;
    record.timestamp_s  = timestamp_s;
    // The restored fields have not been validated in their new context, and
    // the fact that they once were does not carry forward.
    record.validation = ValidationState::Unvalidated;

    result.changes = field_changes(current->fields, target->fields);
    result.record  = record;
    return result;
}

DuplicateResult plan_duplicate(const ProfileArchive &archive, const ProfileVersionKey &source,
                               const std::string &new_profile_id, const std::string &reason,
                               int64_t timestamp_s)
{
    DuplicateResult result;

    if (new_profile_id.empty()) {
        result.error   = HistoryError::EmptyIdentifier;
        result.message = "a duplicate needs a name of its own";
        return result;
    }

    const ProfileVersionRecord *original = archive.find(source);
    if (original == nullptr) {
        result.error   = HistoryError::UnknownRevision;
        result.message = "cannot duplicate " + source.to_string() + ": it is not in this archive";
        return result;
    }
    if (archive.head(source.kind, new_profile_id) != nullptr) {
        result.error   = HistoryError::DuplicateRevision;
        result.message = std::string("a ") + profile_kind_name(source.kind) + " profile named " +
                         new_profile_id + " already exists; duplicating onto it would overwrite "
                         "a history";
        return result;
    }

    ProfileVersionRecord record;
    record.key          = { source.kind, new_profile_id, 1 };
    record.derived_from = source;
    record.fields       = original->fields;
    record.requires_of_others = original->requires_of_others;
    record.reason = reason.empty() ? ("duplicated from " + source.to_string()) : reason;
    record.source       = ProfileSource::User;
    record.timestamp_s  = timestamp_s;
    record.validation   = ValidationState::Unvalidated;

    result.record = record;
    return result;
}

// --- project references ----------------------------------------------------

ProjectResolution resolve_project_references(const ProfileArchive                 &archive,
                                             const std::vector<ProfileVersionKey> &references)
{
    ProjectResolution resolution;
    resolution.complete = true;

    for (const ProfileVersionKey &requested : references) {
        ReferenceResolution one;
        one.requested = requested;

        if (requested.profile_id.empty()) {
            one.status  = ReferenceStatus::MissingProfile;
            one.message = "the project references a profile with no name at all";
            ++resolution.missing;
            resolution.complete = false;
            resolution.references.push_back(one);
            continue;
        }

        const ProfileVersionRecord *head = archive.head(requested.kind, requested.profile_id);
        if (head != nullptr)
            one.head_revision = head->key.revision;

        if (requested.revision <= 0) {
            one.status  = ReferenceStatus::NameOnly;
            one.message = "the project references " + requested.profile_id +
                          " by name without a revision; that does not describe which settings it "
                          "was sliced with";
            ++resolution.name_only;
            resolution.complete = false;
            resolution.references.push_back(one);
            continue;
        }

        if (head == nullptr) {
            one.status  = ReferenceStatus::MissingProfile;
            one.message = std::string("no ") + profile_kind_name(requested.kind) +
                          " profile named " + requested.profile_id + " is present";
            ++resolution.missing;
            resolution.complete = false;
            resolution.references.push_back(one);
            continue;
        }

        const ProfileVersionRecord *record = archive.find(requested);
        if (record == nullptr) {
            one.status  = ReferenceStatus::MissingRevision;
            one.message = requested.to_string() + " is not present; the newest revision here is " +
                          std::to_string(head->key.revision) +
                          ", and using it instead would be a different print";
            ++resolution.missing;
            resolution.complete = false;
            resolution.references.push_back(one);
            continue;
        }

        one.record = record;

        if (record->validation == ValidationState::Invalid) {
            one.status  = ReferenceStatus::Invalid;
            one.message = requested.to_string() + " failed validation";
            ++resolution.invalid;
            resolution.complete = false;
        } else if (record->validation == ValidationState::Unvalidated) {
            one.status  = ReferenceStatus::Unvalidated;
            one.message = requested.to_string() + " has never been validated";
            ++resolution.unvalidated;
            resolution.complete = false;
        } else if (record->key.revision < head->key.revision) {
            // Not a failure. The project meant this revision, and a newer one
            // existing does not change what the project describes.
            one.status  = ReferenceStatus::Superseded;
            one.message = requested.to_string() + " is still exact, but revision " +
                          std::to_string(head->key.revision) + " is now the newest";
            ++resolution.superseded;
        } else {
            one.status = ReferenceStatus::Resolved;
        }

        resolution.references.push_back(one);
    }

    return resolution;
}

} // namespace Slic3r
