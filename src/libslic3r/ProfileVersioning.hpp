///|/ TigerSlicer Phase 27: profile versioning and regression comparison.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ProfileVersioning_hpp_
#define slic3r_ProfileVersioning_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// The six kinds of profile a print depends on. A version of one kind is never
// comparable with a version of another, and the diff says so rather than
// producing a field list that looks meaningful.
enum class ProfileKind : uint8_t
{
    Printer,
    Print,
    Filament,
    Tool,
    Calibration,
    Transition,
};

const char *profile_kind_name(ProfileKind kind);

// Where a version came from. Unknown is not a default to be quietly accepted:
// a value nobody can attribute is reported when the version is validated.
enum class ProfileSource : uint8_t
{
    User,
    Vendor,
    Imported,
    Calibration,
    Migration,
    Unknown,
};

const char *profile_source_name(ProfileSource source);

// Exactly which version of exactly which profile. Projects reference this and
// nothing looser; see resolve_project_references.
struct ProfileVersionKey
{
    ProfileKind kind { ProfileKind::Print };
    std::string profile_id;
    // 1-based. 0 means "no revision given", which is a defect in a reference,
    // not a shorthand for the newest.
    int revision { 0 };

    bool complete() const { return ! profile_id.empty() && revision > 0; }
    std::string to_string() const;

    bool operator==(const ProfileVersionKey &o) const
    {
        return kind == o.kind && profile_id == o.profile_id && revision == o.revision;
    }
    bool operator!=(const ProfileVersionKey &o) const { return ! (*this == o); }
    bool operator<(const ProfileVersionKey &o) const;
};

// One field's difference between two versions. Both sides are optional so that
// "added", "removed" and "changed" stay distinguishable from one another.
struct ProfileFieldChange
{
    std::string                field;
    std::optional<std::string> before;
    std::optional<std::string> after;

    bool added() const { return ! before.has_value() && after.has_value(); }
    bool removed() const { return before.has_value() && ! after.has_value(); }
    bool changed() const { return before.has_value() && after.has_value(); }
};

// Whether a version can be used with something else. Unproven is its own
// verdict: nothing declared a requirement, so nothing was checked, and that is
// not the same as compatible.
enum class CompatibilityVerdict : uint8_t
{
    Compatible,
    Incompatible,
    Unproven,
};

const char *compatibility_verdict_name(CompatibilityVerdict verdict);

// A version that was never validated is not valid. It is unvalidated, and the
// difference is load-bearing when a project is about to be sliced with it.
enum class ValidationState : uint8_t
{
    Unvalidated,
    Valid,
    Invalid,
};

const char *validation_state_name(ValidationState state);

// A requirement one profile places on another, checked by name against the
// other profile's fields. A requirement naming a field the other profile does
// not have is Unproven, never satisfied.
struct CompatibilityRequirement
{
    ProfileKind target_kind { ProfileKind::Printer };
    std::string field;
    // The value the field must have. Absent means only presence is required.
    std::optional<std::string> equals;
    std::string                why;
};

struct ProfileVersionRecord
{
    ProfileVersionKey key;

    // The revision this one was edited from, within the same profile_id.
    // Absent only for revision 1.
    std::optional<int> parent_revision;

    // The version this profile was duplicated from, if it was. Points at
    // another profile_id, and possibly another kind.
    std::optional<ProfileVersionKey> derived_from;

    // Set when this version exists because someone rolled back to an earlier
    // one. History is never rewritten: a rollback is a new revision that
    // restores old field values and says where they came from.
    std::optional<int> restores_revision;

    std::map<std::string, std::string> fields;

    // Why this version exists. An empty reason is a validation finding: a
    // version nobody explained is one nobody can review.
    std::string   reason;
    ProfileSource source { ProfileSource::Unknown };

    // Seconds since the epoch, supplied by the caller. Nothing here reads the
    // clock, so a history is reproducible and orderable by construction.
    int64_t timestamp_s { 0 };

    std::vector<CompatibilityRequirement> requires_of_others;

    ValidationState validation { ValidationState::Unvalidated };
    // Why validation failed, when it did.
    std::vector<std::string> validation_messages;
};

// Why an attempt to change a history was refused. Every refusal names what is
// responsible rather than failing silently.
enum class HistoryError : uint8_t
{
    None,
    UnknownProfile,
    UnknownRevision,
    DuplicateRevision,
    NonMonotonicRevision,
    MissingParent,
    ParentMismatch,
    KindMismatch,
    Immutable,
    EmptyIdentifier,
};

const char *history_error_name(HistoryError error);

struct HistoryResult
{
    HistoryError error { HistoryError::None };
    std::string  message;
    // The version that was written, when one was.
    std::optional<ProfileVersionKey> written;

    bool ok() const { return error == HistoryError::None; }
};

// Every version of every profile. Versions are append-only: an existing
// revision is never rewritten, because a project that referenced it would
// silently become a project about something else.
class ProfileArchive
{
public:
    HistoryResult add(const ProfileVersionRecord &record);

    const ProfileVersionRecord *find(const ProfileVersionKey &key) const;
    // The highest revision recorded for a profile, or nullptr.
    const ProfileVersionRecord *head(ProfileKind kind, const std::string &profile_id) const;

    // Every revision of one profile, in ascending revision order.
    std::vector<const ProfileVersionRecord *> history(ProfileKind         kind,
                                                      const std::string &profile_id) const;

    // Every profile id of a kind, sorted, so callers iterate deterministically.
    std::vector<std::string> profiles(ProfileKind kind) const;

    size_t size() const { return m_records.size(); }

private:
    std::map<ProfileVersionKey, ProfileVersionRecord> m_records;
};

// The result of comparing two versions field by field.
struct ProfileDiff
{
    ProfileVersionKey before, after;

    // Sorted by field name, so a diff is the same diff every time it is taken.
    std::vector<ProfileFieldChange> changes;

    // False when the two versions are not the same kind, or not the same
    // profile and neither was derived from the other. The change list is then
    // empty rather than misleading.
    bool        comparable { true };
    std::string incomparable_reason;

    bool empty() const { return changes.empty(); }
};

ProfileDiff diff_versions(const ProfileVersionRecord &before, const ProfileVersionRecord &after);

// The chain of diffs from one revision to another of the same profile, so a
// reviewer sees the steps and not only the endpoints.
std::vector<ProfileDiff> diff_history(const ProfileArchive &archive, ProfileKind kind,
                                      const std::string &profile_id, int from_revision,
                                      int to_revision);

// Validate a version against the archive it lives in and the profiles it
// declares requirements on. Never mutates; returns what the validation would
// record.
struct ValidationOutcome
{
    ValidationState          state { ValidationState::Unvalidated };
    std::vector<std::string> messages;

    CompatibilityVerdict compatibility { CompatibilityVerdict::Unproven };
    // Requirements that could not be checked, with the reason.
    std::vector<std::string> unchecked;
};

// The versions the requirements are checked against, given explicitly so the
// caller decides what "the other profiles" means rather than the archive
// guessing at a head revision.
ValidationOutcome validate_version(const ProfileVersionRecord                &record,
                                   const std::vector<const ProfileVersionRecord *> &context);

// --- regression comparison -------------------------------------------------

// A metric can be better when it is smaller (time, waste) or when it is larger
// (flow utilization, surface quality, confidence). Nothing infers this.
enum class MetricDirection : uint8_t
{
    LowerIsBetter,
    HigherIsBetter,
};

enum class ChangeDirection : uint8_t
{
    Better,
    Worse,
    Unchanged,
    // One side or the other was never measured. Not neutral: unknown.
    Unknown,
};

const char *change_direction_name(ChangeDirection direction);

// Every field optional, so "never measured" is structurally distinct from
// "measured as zero".
struct OutcomeSample
{
    std::optional<double> time_s;
    std::optional<double> toolchanges;
    std::optional<double> waste_mm3;
    std::optional<double> flow_utilization;
    std::optional<double> surface_quality;
    std::optional<double> warnings;

    // How much the sample itself is to be trusted, 0..1. A comparison is never
    // more confident than the weaker of the two samples.
    std::optional<double> confidence;

    // Where the numbers came from, carried through to the report.
    std::string provenance;
};

struct MetricComparison
{
    std::string           metric;
    MetricDirection       direction { MetricDirection::LowerIsBetter };
    std::optional<double> before, after;
    std::optional<double> delta;
    // Relative change, absent when the baseline is zero or either side is.
    std::optional<double> relative;

    ChangeDirection change { ChangeDirection::Unknown };
    // True only when the change exceeds the policy's threshold for this
    // metric. A change under the threshold is reported, not hidden, but it is
    // not counted towards the verdict.
    bool significant { false };

    // Why the comparison could not be made, when it could not.
    std::string unmeasured_reason;
};

enum class RegressionVerdict : uint8_t
{
    Improvement,
    Regression,
    Mixed,
    Neutral,
    // Nothing could be compared, or the comparison rests on samples too weak
    // to support a claim.
    Unproven,
};

const char *regression_verdict_name(RegressionVerdict verdict);

struct RegressionPolicy
{
    // Absolute thresholds below which a change is not called significant.
    double time_s_threshold { 1.0 };
    double toolchange_threshold { 0.5 };
    double waste_mm3_threshold { 1.0 };
    double flow_threshold { 0.01 };
    double surface_threshold { 0.01 };
    double warnings_threshold { 0.5 };

    // Below this, a comparison is Unproven no matter what the numbers say.
    double minimum_confidence { 0.5 };

    // When true, a sample with no confidence at all is treated as too weak
    // rather than as fully trusted.
    bool unstated_confidence_is_weak { true };
};

struct RegressionReport
{
    ProfileVersionKey before, after;

    // In a fixed order, so two reports of the same comparison read alike.
    std::vector<MetricComparison> metrics;

    RegressionVerdict verdict { RegressionVerdict::Unproven };

    size_t improved { 0 };
    size_t regressed { 0 };
    size_t unchanged { 0 };
    size_t unknown { 0 };

    // The confidence the report itself carries: the weaker of the two samples.
    std::optional<double> confidence;
    // Set when the verdict is Unproven, saying which of the two reasons it is.
    std::string unproven_reason;

    // Field changes between the two versions, so a regression can be read
    // against what actually changed.
    ProfileDiff diff;

    std::vector<std::string> notes;

    bool has_regression() const { return regressed > 0; }
};

RegressionReport compare_versions(const ProfileVersionRecord &before_version,
                                  const OutcomeSample        &before_sample,
                                  const ProfileVersionRecord &after_version,
                                  const OutcomeSample        &after_sample,
                                  const RegressionPolicy     &policy);

// --- rollback and duplication ---------------------------------------------

// Build the version that rolling back to `to_revision` would produce: a new
// revision at the head of the same profile, restoring the target's fields and
// recording where they came from. Does not modify the archive; the caller adds
// the result, so the decision to write stays with the caller.
struct RollbackResult
{
    HistoryError                        error { HistoryError::None };
    std::string                         message;
    std::optional<ProfileVersionRecord> record;
    // The fields the rollback would change, relative to the current head.
    std::vector<ProfileFieldChange> changes;

    bool ok() const { return error == HistoryError::None; }
};

RollbackResult plan_rollback(const ProfileArchive &archive, ProfileKind kind,
                             const std::string &profile_id, int to_revision,
                             const std::string &reason, int64_t timestamp_s);

// Build revision 1 of a new profile from an existing version. The copy records
// what it was derived from; it does not inherit the source's parent chain,
// because it is a new profile and not a continuation of an old one.
struct DuplicateResult
{
    HistoryError                        error { HistoryError::None };
    std::string                         message;
    std::optional<ProfileVersionRecord> record;

    bool ok() const { return error == HistoryError::None; }
};

DuplicateResult plan_duplicate(const ProfileArchive &archive, const ProfileVersionKey &source,
                               const std::string &new_profile_id, const std::string &reason,
                               int64_t timestamp_s);

// --- project references ----------------------------------------------------

enum class ReferenceStatus : uint8_t
{
    Resolved,
    // The reference names a profile but no revision. This is the failure the
    // phase exists to make impossible: a project that means "whatever that
    // profile is now" does not describe a print.
    NameOnly,
    MissingProfile,
    MissingRevision,
    // Resolved, but a newer revision exists. Informational, never an error:
    // the project meant the revision it names.
    Superseded,
    // Resolved, but the version it names never passed validation.
    Unvalidated,
    Invalid,
};

const char *reference_status_name(ReferenceStatus status);

struct ReferenceResolution
{
    ProfileVersionKey requested;
    ReferenceStatus   status { ReferenceStatus::MissingProfile };
    std::string       message;

    const ProfileVersionRecord *record { nullptr };
    // The head revision at resolution time, when the profile exists.
    std::optional<int> head_revision;

    bool usable() const
    {
        return status == ReferenceStatus::Resolved || status == ReferenceStatus::Superseded;
    }
};

struct ProjectResolution
{
    // In the order the project listed them.
    std::vector<ReferenceResolution> references;

    // True only when every reference is usable. A project that cannot name
    // exactly what it was sliced with is not reproducible, and this says so
    // rather than falling back to a head revision.
    bool complete { false };

    size_t name_only { 0 };
    size_t missing { 0 };
    size_t superseded { 0 };
    size_t unvalidated { 0 };
    size_t invalid { 0 };
};

ProjectResolution resolve_project_references(const ProfileArchive                   &archive,
                                             const std::vector<ProfileVersionKey> &references);

} // namespace Slic3r

#endif // slic3r_ProfileVersioning_hpp_
