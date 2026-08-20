///|/ TigerSlicer Phase 27: profile versioning and regression comparison.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/ProfileVersioning.hpp"

using namespace Slic3r;

namespace {

// A wall-clock second is never read by the library, so fixtures supply their
// own monotonic stamps.
constexpr int64_t T0 = 1'700'000'000;

ProfileVersionRecord make_version(ProfileKind kind, const std::string &id, int revision,
                                  std::map<std::string, std::string> fields)
{
    ProfileVersionRecord record;
    record.key    = { kind, id, revision };
    record.fields = std::move(fields);
    if (revision > 1)
        record.parent_revision = revision - 1;
    record.reason      = "test fixture";
    record.source      = ProfileSource::User;
    record.timestamp_s = T0 + revision;
    return record;
}

ProfileArchive two_revision_archive()
{
    ProfileArchive archive;
    REQUIRE(archive
                .add(make_version(ProfileKind::Print, "fine", 1,
                                  { { "layer_height", "0.20" }, { "perimeters", "3" } }))
                .ok());
    REQUIRE(archive
                .add(make_version(ProfileKind::Print, "fine", 2,
                                  { { "layer_height", "0.15" }, { "perimeters", "3" },
                                    { "top_layers", "5" } }))
                .ok());
    return archive;
}

OutcomeSample confident_sample(double time_s, double toolchanges, double waste)
{
    OutcomeSample sample;
    sample.time_s      = time_s;
    sample.toolchanges = toolchanges;
    sample.waste_mm3   = waste;
    sample.confidence  = 0.9;
    sample.provenance  = "sliced with the same model and machine";
    return sample;
}

} // namespace

TEST_CASE("27: a version key names exactly one version", "[profile-versioning]")
{
    ProfileVersionKey key { ProfileKind::Filament, "PLA-black", 4 };
    CHECK(key.complete());
    CHECK(key.to_string() == "filament:PLA-black@4");

    ProfileVersionKey nameless { ProfileKind::Filament, "PLA-black", 0 };
    CHECK_FALSE(nameless.complete());
    CHECK(nameless.to_string() == "filament:PLA-black@<none>");
}

TEST_CASE("27: a history is append-only and refuses to be rewritten", "[profile-versioning]")
{
    ProfileArchive archive = two_revision_archive();

    SECTION("the same revision cannot be written twice, even with the same contents")
    {
        HistoryResult again = archive.add(make_version(ProfileKind::Print, "fine", 2,
                                                       { { "layer_height", "0.15" } }));
        CHECK_FALSE(again.ok());
        CHECK(again.error == HistoryError::DuplicateRevision);
        CHECK_THAT(again.message, Catch::Matchers::ContainsSubstring("immutable"));
    }

    SECTION("a history cannot start in the middle")
    {
        HistoryResult orphan =
            archive.add(make_version(ProfileKind::Print, "coarse", 3, { { "a", "b" } }));
        CHECK_FALSE(orphan.ok());
        CHECK(orphan.error == HistoryError::MissingParent);
    }

    SECTION("a revision must come after the head")
    {
        ProfileVersionRecord backwards =
            make_version(ProfileKind::Print, "fine", 2, { { "a", "b" } });
        backwards.key.revision    = 2;
        backwards.parent_revision = 1;
        // Already covered by the duplicate check; use an explicitly lower one.
        ProfileVersionRecord lower = make_version(ProfileKind::Print, "fine", 1, { { "a", "b" } });
        lower.parent_revision.reset();
        HistoryResult result = archive.add(lower);
        CHECK_FALSE(result.ok());
        CHECK(result.error == HistoryError::DuplicateRevision);
    }

    SECTION("a later revision must say what it was edited from")
    {
        ProfileVersionRecord orphaned =
            make_version(ProfileKind::Print, "fine", 3, { { "a", "b" } });
        orphaned.parent_revision.reset();
        HistoryResult result = archive.add(orphaned);
        CHECK_FALSE(result.ok());
        CHECK(result.error == HistoryError::MissingParent);
        CHECK_THAT(result.message,
                   Catch::Matchers::ContainsSubstring("does not say which revision"));
    }

    SECTION("revision 1 cannot claim a parent inside its own history")
    {
        ProfileVersionRecord first = make_version(ProfileKind::Tool, "T0", 1, { { "a", "b" } });
        first.parent_revision      = 0;
        HistoryResult result       = archive.add(first);
        CHECK_FALSE(result.ok());
        CHECK(result.error == HistoryError::ParentMismatch);
    }

    SECTION("a revision number of zero identifies nothing")
    {
        ProfileVersionRecord zero = make_version(ProfileKind::Tool, "T0", 1, { { "a", "b" } });
        zero.key.revision         = 0;
        CHECK(archive.add(zero).error == HistoryError::UnknownRevision);
    }

    SECTION("a version with no identifier is refused before anything else is checked")
    {
        ProfileVersionRecord unnamed = make_version(ProfileKind::Tool, "", 1, { { "a", "b" } });
        CHECK(archive.add(unnamed).error == HistoryError::EmptyIdentifier);
    }
}

TEST_CASE("27: the same profile name in two kinds is two histories", "[profile-versioning]")
{
    ProfileArchive archive;
    REQUIRE(archive.add(make_version(ProfileKind::Print, "draft", 1, { { "a", "1" } })).ok());
    REQUIRE(archive.add(make_version(ProfileKind::Filament, "draft", 1, { { "a", "2" } })).ok());

    CHECK(archive.size() == 2);
    REQUIRE(archive.head(ProfileKind::Print, "draft") != nullptr);
    CHECK(archive.head(ProfileKind::Print, "draft")->fields.at("a") == "1");
    CHECK(archive.head(ProfileKind::Filament, "draft")->fields.at("a") == "2");
    CHECK(archive.profiles(ProfileKind::Print) == std::vector<std::string> { "draft" });
    CHECK(archive.profiles(ProfileKind::Calibration).empty());
}

TEST_CASE("27: a field diff distinguishes added, removed and changed", "[profile-versioning]")
{
    ProfileVersionRecord before =
        make_version(ProfileKind::Print, "fine", 1, { { "layer_height", "0.20" },
                                                      { "gone", "yes" } });
    ProfileVersionRecord after =
        make_version(ProfileKind::Print, "fine", 2, { { "layer_height", "0.15" },
                                                      { "added", "new" } });

    ProfileDiff diff = diff_versions(before, after);
    REQUIRE(diff.comparable);
    REQUIRE(diff.changes.size() == 3);

    // Sorted by field name: added, gone, layer_height.
    CHECK(diff.changes[0].field == "added");
    CHECK(diff.changes[0].added());
    CHECK_FALSE(diff.changes[0].removed());

    CHECK(diff.changes[1].field == "gone");
    CHECK(diff.changes[1].removed());

    CHECK(diff.changes[2].field == "layer_height");
    CHECK(diff.changes[2].changed());
    CHECK(diff.changes[2].before.value() == "0.20");
    CHECK(diff.changes[2].after.value() == "0.15");
}

TEST_CASE("27: unrelated profiles are not diffed into a meaningless field list",
          "[profile-versioning]")
{
    ProfileVersionRecord print =
        make_version(ProfileKind::Print, "fine", 1, { { "layer_height", "0.20" } });
    ProfileVersionRecord filament =
        make_version(ProfileKind::Filament, "PLA", 1, { { "layer_height", "0.20" } });

    SECTION("across kinds")
    {
        ProfileDiff diff = diff_versions(print, filament);
        CHECK_FALSE(diff.comparable);
        CHECK(diff.changes.empty());
        CHECK_THAT(diff.incomparable_reason,
                   Catch::Matchers::ContainsSubstring("do not describe the same thing"));
    }

    SECTION("across unrelated profiles of the same kind")
    {
        ProfileVersionRecord other =
            make_version(ProfileKind::Print, "coarse", 1, { { "layer_height", "0.30" } });
        ProfileDiff diff = diff_versions(print, other);
        CHECK_FALSE(diff.comparable);
        CHECK_THAT(diff.incomparable_reason, Catch::Matchers::ContainsSubstring("unrelated"));
    }

    SECTION("but a duplicate can be diffed against what it was duplicated from")
    {
        ProfileVersionRecord copy =
            make_version(ProfileKind::Print, "fine-copy", 1, { { "layer_height", "0.10" } });
        copy.parent_revision.reset();
        copy.derived_from = print.key;

        ProfileDiff diff = diff_versions(print, copy);
        REQUIRE(diff.comparable);
        REQUIRE(diff.changes.size() == 1);
        CHECK(diff.changes[0].field == "layer_height");
    }
}

TEST_CASE("27: a history diff shows every step, not only the endpoints", "[profile-versioning]")
{
    ProfileArchive archive;
    REQUIRE(archive.add(make_version(ProfileKind::Print, "p", 1, { { "v", "1" } })).ok());
    REQUIRE(archive.add(make_version(ProfileKind::Print, "p", 2, { { "v", "2" } })).ok());
    REQUIRE(archive.add(make_version(ProfileKind::Print, "p", 3, { { "v", "3" } })).ok());

    std::vector<ProfileDiff> steps = diff_history(archive, ProfileKind::Print, "p", 1, 3);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].before.revision == 1);
    CHECK(steps[0].after.revision == 2);
    CHECK(steps[0].changes[0].after.value() == "2");
    CHECK(steps[1].after.revision == 3);

    CHECK(diff_history(archive, ProfileKind::Print, "p", 3, 1).empty());
    CHECK(diff_history(archive, ProfileKind::Print, "absent", 1, 3).empty());
}

TEST_CASE("27: a version nobody checked is unvalidated, not valid", "[profile-versioning]")
{
    ProfileVersionRecord record =
        make_version(ProfileKind::Filament, "PLA", 1, { { "temperature", "210" } });

    SECTION("declaring no requirements leaves compatibility unproven")
    {
        ValidationOutcome outcome = validate_version(record, {});
        CHECK(outcome.compatibility == CompatibilityVerdict::Unproven);
        CHECK(outcome.state == ValidationState::Unvalidated);
        REQUIRE(outcome.unchecked.size() == 1);
        CHECK_THAT(outcome.unchecked[0],
                   Catch::Matchers::ContainsSubstring("declares no requirements"));
    }

    SECTION("a requirement with nothing to check it against is unproven, not satisfied")
    {
        record.requires_of_others.push_back(
            { ProfileKind::Printer, "nozzle_mm", std::string("0.40"), "the flow model assumes it" });
        ValidationOutcome outcome = validate_version(record, {});
        CHECK(outcome.compatibility == CompatibilityVerdict::Unproven);
        CHECK(outcome.state == ValidationState::Unvalidated);
        CHECK_THAT(outcome.unchecked[0],
                   Catch::Matchers::ContainsSubstring("no printer profile was supplied"));
    }

    SECTION("a requirement naming a field the other profile does not have is unproven")
    {
        record.requires_of_others.push_back(
            { ProfileKind::Printer, "nozzle_mm", std::string("0.40"), "" });
        ProfileVersionRecord printer =
            make_version(ProfileKind::Printer, "tiger", 1, { { "bed_x", "250" } });
        ValidationOutcome outcome = validate_version(record, { &printer });
        CHECK(outcome.compatibility == CompatibilityVerdict::Unproven);
        CHECK_THAT(outcome.unchecked[0],
                   Catch::Matchers::ContainsSubstring("does not have that field"));
    }

    SECTION("a satisfied requirement is compatible and validates")
    {
        record.requires_of_others.push_back(
            { ProfileKind::Printer, "nozzle_mm", std::string("0.40"), "" });
        ProfileVersionRecord printer =
            make_version(ProfileKind::Printer, "tiger", 1, { { "nozzle_mm", "0.40" } });
        ValidationOutcome outcome = validate_version(record, { &printer });
        CHECK(outcome.compatibility == CompatibilityVerdict::Compatible);
        CHECK(outcome.state == ValidationState::Valid);
        CHECK(outcome.messages.empty());
    }

    SECTION("a violated requirement is incompatible and invalid, and says why")
    {
        record.requires_of_others.push_back({ ProfileKind::Printer, "nozzle_mm",
                                              std::string("0.40"),
                                              "the flow model was fitted at 0.40" });
        ProfileVersionRecord printer =
            make_version(ProfileKind::Printer, "tiger", 1, { { "nozzle_mm", "0.60" } });
        ValidationOutcome outcome = validate_version(record, { &printer });
        CHECK(outcome.compatibility == CompatibilityVerdict::Incompatible);
        CHECK(outcome.state == ValidationState::Invalid);
        REQUIRE(outcome.messages.size() == 1);
        CHECK_THAT(outcome.messages[0],
                   Catch::Matchers::ContainsSubstring("the flow model was fitted at 0.40"));
    }

    SECTION("one satisfied and one uncheckable requirement is still unproven")
    {
        record.requires_of_others.push_back({ ProfileKind::Printer, "nozzle_mm",
                                              std::string("0.40"), "" });
        record.requires_of_others.push_back({ ProfileKind::Calibration, "flow", std::nullopt, "" });
        ProfileVersionRecord printer =
            make_version(ProfileKind::Printer, "tiger", 1, { { "nozzle_mm", "0.40" } });
        ValidationOutcome outcome = validate_version(record, { &printer });
        CHECK(outcome.compatibility == CompatibilityVerdict::Unproven);
        CHECK(outcome.state == ValidationState::Unvalidated);
    }
}

TEST_CASE("27: validation reports an unexplained or unattributable version",
          "[profile-versioning]")
{
    ProfileVersionRecord record =
        make_version(ProfileKind::Print, "p", 1, { { "layer_height", "0.2" } });
    record.requires_of_others.push_back({ ProfileKind::Printer, "nozzle_mm", std::nullopt, "" });
    ProfileVersionRecord printer =
        make_version(ProfileKind::Printer, "tiger", 1, { { "nozzle_mm", "0.40" } });

    record.reason.clear();
    record.source      = ProfileSource::Unknown;
    record.timestamp_s = 0;

    ValidationOutcome outcome = validate_version(record, { &printer });
    // Everything checkable passed, but three things about the version itself
    // could not be defended, so it does not reach Valid.
    CHECK(outcome.compatibility == CompatibilityVerdict::Compatible);
    CHECK(outcome.state == ValidationState::Unvalidated);
    CHECK(outcome.messages.size() == 3);

    SECTION("a version with no fields at all is invalid outright")
    {
        record.fields.clear();
        CHECK(validate_version(record, { &printer }).state == ValidationState::Invalid);
    }
}

TEST_CASE("27: a regression comparison reports every metric it could not compare",
          "[profile-versioning]")
{
    ProfileVersionRecord before =
        make_version(ProfileKind::Print, "fine", 1, { { "layer_height", "0.20" } });
    ProfileVersionRecord after =
        make_version(ProfileKind::Print, "fine", 2, { { "layer_height", "0.15" } });

    RegressionPolicy policy;

    SECTION("a clear improvement in every measured metric")
    {
        RegressionReport report = compare_versions(before, confident_sample(600., 19., 1710.),
                                                   after, confident_sample(500., 3., 270.),
                                                   policy);
        CHECK(report.verdict == RegressionVerdict::Improvement);
        CHECK(report.improved == 3);
        CHECK(report.regressed == 0);
        // Flow, surface and warnings were never measured on either side.
        CHECK(report.unknown == 3);
        REQUIRE(report.confidence.has_value());
        CHECK(report.confidence.value() == 0.9);
        // The diff travels with the numbers.
        REQUIRE(report.diff.comparable);
        REQUIRE(report.diff.changes.size() == 1);
        CHECK(report.diff.changes[0].field == "layer_height");
    }

    SECTION("an unmeasured metric is unknown, not neutral, and says which side is missing")
    {
        OutcomeSample old_sample = confident_sample(600., 19., 1710.);
        OutcomeSample new_sample = confident_sample(500., 3., 270.);
        new_sample.waste_mm3.reset();

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        const MetricComparison *waste = nullptr;
        for (const MetricComparison &m : report.metrics)
            if (m.metric == "waste")
                waste = &m;
        REQUIRE(waste != nullptr);
        CHECK(waste->change == ChangeDirection::Unknown);
        CHECK(waste->unmeasured_reason == "the new version did not measure it");
        CHECK(report.improved == 2);
        CHECK(report.unknown == 4);
    }

    SECTION("a mixed result is mixed, and never averaged into a single number")
    {
        OutcomeSample old_sample = confident_sample(500., 3., 270.);
        OutcomeSample new_sample = confident_sample(400., 9., 810.);

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Mixed);
        CHECK(report.improved == 1);
        CHECK(report.regressed == 2);
    }

    SECTION("a change under the threshold is reported but does not move the verdict")
    {
        OutcomeSample old_sample = confident_sample(500., 3., 270.);
        OutcomeSample new_sample = confident_sample(500.4, 3., 270.);

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Neutral);
        CHECK(report.unchanged == 3);
        const MetricComparison &time = report.metrics[0];
        CHECK(time.metric == "time");
        CHECK(time.change == ChangeDirection::Unchanged);
        CHECK_FALSE(time.significant);
        // The measured delta is still there to look at.
        REQUIRE(time.delta.has_value());
        CHECK(time.delta.value() > 0.);
    }

    SECTION("higher is better for flow, surface and confidence")
    {
        OutcomeSample old_sample, new_sample;
        old_sample.flow_utilization = 0.60;
        old_sample.surface_quality  = 0.70;
        old_sample.confidence       = 0.8;
        new_sample.flow_utilization = 0.80;
        new_sample.surface_quality  = 0.55;
        new_sample.confidence       = 0.8;

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Mixed);
        for (const MetricComparison &m : report.metrics) {
            if (m.metric == "flow utilization") {
                CHECK(m.direction == MetricDirection::HigherIsBetter);
                CHECK(m.change == ChangeDirection::Better);
            }
            if (m.metric == "surface quality")
                CHECK(m.change == ChangeDirection::Worse);
        }
    }

    SECTION("more warnings is a regression")
    {
        OutcomeSample old_sample, new_sample;
        old_sample.warnings  = 1.;
        old_sample.confidence = 0.9;
        new_sample.warnings  = 4.;
        new_sample.confidence = 0.9;

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Regression);
        CHECK(report.has_regression());
    }
}

TEST_CASE("27: a comparison is never stronger than its weaker sample", "[profile-versioning]")
{
    ProfileVersionRecord before =
        make_version(ProfileKind::Print, "fine", 1, { { "layer_height", "0.20" } });
    ProfileVersionRecord after =
        make_version(ProfileKind::Print, "fine", 2, { { "layer_height", "0.15" } });
    RegressionPolicy policy;

    SECTION("the weaker confidence is the report's confidence")
    {
        OutcomeSample old_sample = confident_sample(600., 19., 1710.);
        OutcomeSample new_sample = confident_sample(500., 3., 270.);
        new_sample.confidence    = 0.6;

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        REQUIRE(report.confidence.has_value());
        CHECK(report.confidence.value() == 0.6);
        CHECK(report.verdict == RegressionVerdict::Improvement);
    }

    SECTION("below the required confidence, the numbers do not support a verdict")
    {
        OutcomeSample old_sample = confident_sample(600., 19., 1710.);
        OutcomeSample new_sample = confident_sample(500., 3., 270.);
        new_sample.confidence    = 0.2;

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Unproven);
        CHECK_THAT(report.unproven_reason, Catch::Matchers::ContainsSubstring("confidence"));
        // The per-metric findings are still there; only the claim is withheld.
        CHECK(report.improved == 3);
    }

    SECTION("a sample that never stated a confidence is weak, not trusted")
    {
        OutcomeSample old_sample = confident_sample(600., 19., 1710.);
        OutcomeSample new_sample = confident_sample(500., 3., 270.);
        new_sample.confidence.reset();

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Unproven);
        CHECK_FALSE(report.confidence.has_value());
        CHECK_THAT(report.unproven_reason,
                   Catch::Matchers::ContainsSubstring("did not state how far it can be trusted"));

        RegressionPolicy lenient      = policy;
        lenient.unstated_confidence_is_weak = false;
        RegressionReport permitted =
            compare_versions(before, old_sample, after, new_sample, lenient);
        CHECK(permitted.verdict == RegressionVerdict::Improvement);
    }

    SECTION("nothing measured on both sides proves nothing")
    {
        OutcomeSample old_sample;
        old_sample.time_s     = 600.;
        old_sample.confidence = 0.9;
        OutcomeSample new_sample;
        new_sample.waste_mm3  = 270.;
        new_sample.confidence = 0.9;

        RegressionReport report = compare_versions(before, old_sample, after, new_sample, policy);
        CHECK(report.verdict == RegressionVerdict::Unproven);
        CHECK_THAT(report.unproven_reason,
                   Catch::Matchers::ContainsSubstring("nothing was actually compared"));
        CHECK(report.unknown == 6);
    }
}

TEST_CASE("27: rolling back adds a revision instead of erasing one", "[profile-versioning]")
{
    ProfileArchive archive = two_revision_archive();
    REQUIRE(archive
                .add(make_version(ProfileKind::Print, "fine", 3,
                                  { { "layer_height", "0.05" }, { "perimeters", "3" } }))
                .ok());

    SECTION("the rollback is a new head restoring the target's fields")
    {
        RollbackResult plan =
            plan_rollback(archive, ProfileKind::Print, "fine", 1, "0.05 was unprintable", T0 + 99);
        REQUIRE(plan.ok());
        REQUIRE(plan.record.has_value());
        CHECK(plan.record->key.revision == 4);
        CHECK(plan.record->parent_revision.value() == 3);
        CHECK(plan.record->restores_revision.value() == 1);
        CHECK(plan.record->fields.at("layer_height") == "0.20");
        CHECK(plan.record->reason == "0.05 was unprintable");
        // Restored fields are not validated by having once been validated.
        CHECK(plan.record->validation == ValidationState::Unvalidated);
        // What it would change, relative to the current head.
        REQUIRE(plan.changes.size() == 1);
        CHECK(plan.changes[0].field == "layer_height");
        CHECK(plan.changes[0].before.value() == "0.05");
        CHECK(plan.changes[0].after.value() == "0.20");

        REQUIRE(archive.add(*plan.record).ok());
        // Every earlier revision is still there and still says what it said.
        CHECK(archive.history(ProfileKind::Print, "fine").size() == 4);
        CHECK(archive.find({ ProfileKind::Print, "fine", 3 })->fields.at("layer_height") == "0.05");
        CHECK(archive.head(ProfileKind::Print, "fine")->key.revision == 4);
    }

    SECTION("rolling back to a revision that does not exist is refused, and says so")
    {
        RollbackResult plan = plan_rollback(archive, ProfileKind::Print, "fine", 9, "", T0);
        CHECK_FALSE(plan.ok());
        CHECK(plan.error == HistoryError::UnknownRevision);
        CHECK_FALSE(plan.record.has_value());
    }

    SECTION("rolling back to the current revision is refused rather than made a no-op version")
    {
        RollbackResult plan = plan_rollback(archive, ProfileKind::Print, "fine", 3, "", T0);
        CHECK_FALSE(plan.ok());
        CHECK(plan.error == HistoryError::DuplicateRevision);
    }

    SECTION("rolling back a profile with no history names the profile")
    {
        RollbackResult plan = plan_rollback(archive, ProfileKind::Print, "absent", 1, "", T0);
        CHECK(plan.error == HistoryError::UnknownProfile);
        CHECK_THAT(plan.message, Catch::Matchers::ContainsSubstring("absent"));
    }
}

TEST_CASE("27: a duplicate is a new profile that records where it came from",
          "[profile-versioning]")
{
    ProfileArchive archive = two_revision_archive();

    SECTION("the copy starts its own history at revision 1")
    {
        DuplicateResult plan = plan_duplicate(archive, { ProfileKind::Print, "fine", 2 },
                                              "fine-experiment", "trying a taller top", T0 + 50);
        REQUIRE(plan.ok());
        REQUIRE(plan.record.has_value());
        CHECK(plan.record->key.revision == 1);
        CHECK(plan.record->key.profile_id == "fine-experiment");
        CHECK_FALSE(plan.record->parent_revision.has_value());
        REQUIRE(plan.record->derived_from.has_value());
        CHECK(plan.record->derived_from->to_string() == "print:fine@2");
        CHECK(plan.record->fields.at("layer_height") == "0.15");
        CHECK(plan.record->validation == ValidationState::Unvalidated);

        REQUIRE(archive.add(*plan.record).ok());
        // The two histories are independent from here.
        CHECK(archive.history(ProfileKind::Print, "fine").size() == 2);
        CHECK(archive.history(ProfileKind::Print, "fine-experiment").size() == 1);
    }

    SECTION("duplicating onto an existing profile would overwrite a history, so it is refused")
    {
        DuplicateResult plan =
            plan_duplicate(archive, { ProfileKind::Print, "fine", 1 }, "fine", "", T0);
        CHECK_FALSE(plan.ok());
        CHECK(plan.error == HistoryError::DuplicateRevision);
        CHECK_THAT(plan.message, Catch::Matchers::ContainsSubstring("overwrite a history"));
    }

    SECTION("duplicating a version that is not there is refused")
    {
        DuplicateResult plan =
            plan_duplicate(archive, { ProfileKind::Print, "fine", 7 }, "copy", "", T0);
        CHECK(plan.error == HistoryError::UnknownRevision);
    }

    SECTION("a duplicate needs a name of its own")
    {
        DuplicateResult plan =
            plan_duplicate(archive, { ProfileKind::Print, "fine", 1 }, "", "", T0);
        CHECK(plan.error == HistoryError::EmptyIdentifier);
    }

    SECTION("an archive refuses a version derived from something it has never seen")
    {
        ProfileVersionRecord stray =
            make_version(ProfileKind::Print, "stray", 1, { { "a", "b" } });
        stray.parent_revision.reset();
        stray.derived_from = ProfileVersionKey { ProfileKind::Print, "elsewhere", 3 };
        HistoryResult result = archive.add(stray);
        CHECK_FALSE(result.ok());
        CHECK(result.error == HistoryError::UnknownRevision);
    }
}

TEST_CASE("27: a project must reference exact versions, not names", "[profile-versioning]")
{
    ProfileArchive archive;
    ProfileVersionRecord printer =
        make_version(ProfileKind::Printer, "tiger", 1, { { "nozzle_mm", "0.40" } });
    printer.validation = ValidationState::Valid;
    REQUIRE(archive.add(printer).ok());

    ProfileVersionRecord print_v1 =
        make_version(ProfileKind::Print, "fine", 1, { { "layer_height", "0.20" } });
    print_v1.validation = ValidationState::Valid;
    REQUIRE(archive.add(print_v1).ok());
    ProfileVersionRecord print_v2 =
        make_version(ProfileKind::Print, "fine", 2, { { "layer_height", "0.15" } });
    print_v2.validation = ValidationState::Valid;
    REQUIRE(archive.add(print_v2).ok());

    SECTION("exact and current references resolve")
    {
        ProjectResolution resolution = resolve_project_references(
            archive, { { ProfileKind::Printer, "tiger", 1 }, { ProfileKind::Print, "fine", 2 } });
        CHECK(resolution.complete);
        CHECK(resolution.references[0].status == ReferenceStatus::Resolved);
        CHECK(resolution.references[1].status == ReferenceStatus::Resolved);
        CHECK(resolution.references[1].head_revision.value() == 2);
    }

    SECTION("a reference by name alone is the failure this phase exists to prevent")
    {
        ProjectResolution resolution =
            resolve_project_references(archive, { { ProfileKind::Print, "fine", 0 } });
        CHECK_FALSE(resolution.complete);
        CHECK(resolution.name_only == 1);
        CHECK(resolution.references[0].status == ReferenceStatus::NameOnly);
        CHECK_FALSE(resolution.references[0].usable());
        CHECK_THAT(resolution.references[0].message,
                   Catch::Matchers::ContainsSubstring("without a revision"));
        // The head is still reported, so a UI can offer it - but nothing here
        // substitutes it for what the project asked for.
        CHECK(resolution.references[0].head_revision.value() == 2);
    }

    SECTION("an older exact reference is superseded, which is not an error")
    {
        ProjectResolution resolution =
            resolve_project_references(archive, { { ProfileKind::Print, "fine", 1 } });
        CHECK(resolution.complete);
        CHECK(resolution.superseded == 1);
        CHECK(resolution.references[0].status == ReferenceStatus::Superseded);
        CHECK(resolution.references[0].usable());
        CHECK(resolution.references[0].record->fields.at("layer_height") == "0.20");
    }

    SECTION("a missing revision is never replaced by the head")
    {
        ProjectResolution resolution =
            resolve_project_references(archive, { { ProfileKind::Print, "fine", 5 } });
        CHECK_FALSE(resolution.complete);
        CHECK(resolution.missing == 1);
        CHECK(resolution.references[0].status == ReferenceStatus::MissingRevision);
        CHECK(resolution.references[0].record == nullptr);
        CHECK_THAT(resolution.references[0].message,
                   Catch::Matchers::ContainsSubstring("would be a different print"));
    }

    SECTION("a missing profile names the kind it was looking for")
    {
        ProjectResolution resolution =
            resolve_project_references(archive, { { ProfileKind::Filament, "PLA", 1 } });
        CHECK(resolution.references[0].status == ReferenceStatus::MissingProfile);
        CHECK_FALSE(resolution.references[0].head_revision.has_value());
        CHECK_THAT(resolution.references[0].message,
                   Catch::Matchers::ContainsSubstring("filament profile named PLA"));
    }

    SECTION("an unvalidated or invalid version resolves but does not complete the project")
    {
        ProfileVersionRecord unchecked =
            make_version(ProfileKind::Filament, "PLA", 1, { { "temperature", "210" } });
        REQUIRE(archive.add(unchecked).ok());
        ProfileVersionRecord bad =
            make_version(ProfileKind::Tool, "T0", 1, { { "nozzle_mm", "0.40" } });
        bad.validation = ValidationState::Invalid;
        REQUIRE(archive.add(bad).ok());

        ProjectResolution resolution = resolve_project_references(
            archive, { { ProfileKind::Filament, "PLA", 1 }, { ProfileKind::Tool, "T0", 1 } });
        CHECK_FALSE(resolution.complete);
        CHECK(resolution.unvalidated == 1);
        CHECK(resolution.invalid == 1);
        CHECK(resolution.references[0].status == ReferenceStatus::Unvalidated);
        CHECK(resolution.references[1].status == ReferenceStatus::Invalid);
    }

    SECTION("references are reported in the order the project listed them")
    {
        ProjectResolution resolution = resolve_project_references(
            archive, { { ProfileKind::Print, "fine", 2 }, { ProfileKind::Filament, "PLA", 1 },
                       { ProfileKind::Printer, "tiger", 1 } });
        REQUIRE(resolution.references.size() == 3);
        CHECK(resolution.references[0].requested.kind == ProfileKind::Print);
        CHECK(resolution.references[1].requested.kind == ProfileKind::Filament);
        CHECK(resolution.references[2].requested.kind == ProfileKind::Printer);
    }
}

TEST_CASE("27: all six profile kinds are versioned the same way", "[profile-versioning]")
{
    const ProfileKind kinds[] = { ProfileKind::Printer,     ProfileKind::Print,
                                  ProfileKind::Filament,    ProfileKind::Tool,
                                  ProfileKind::Calibration, ProfileKind::Transition };

    ProfileArchive archive;
    for (ProfileKind kind : kinds) {
        REQUIRE(archive.add(make_version(kind, "shared-name", 1, { { "v", "1" } })).ok());
        REQUIRE(archive.add(make_version(kind, "shared-name", 2, { { "v", "2" } })).ok());
    }
    CHECK(archive.size() == 12);

    for (ProfileKind kind : kinds) {
        REQUIRE(archive.head(kind, "shared-name") != nullptr);
        CHECK(archive.head(kind, "shared-name")->key.revision == 2);

        RollbackResult plan = plan_rollback(archive, kind, "shared-name", 1, "back", T0 + 5);
        REQUIRE(plan.ok());
        CHECK(plan.record->fields.at("v") == "1");

        DuplicateResult copy =
            plan_duplicate(archive, { kind, "shared-name", 2 }, "copy", "", T0 + 6);
        REQUIRE(copy.ok());
        CHECK(copy.record->key.kind == kind);
    }
}

TEST_CASE("27: the same inputs give the same history, diff and report every time",
          "[profile-versioning]")
{
    ProfileArchive archive = two_revision_archive();

    const std::vector<const ProfileVersionRecord *> first =
        archive.history(ProfileKind::Print, "fine");
    const std::vector<const ProfileVersionRecord *> second =
        archive.history(ProfileKind::Print, "fine");
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i)
        CHECK(first[i]->key == second[i]->key);

    ProfileDiff a = diff_versions(*first[0], *first[1]);
    ProfileDiff b = diff_versions(*first[0], *first[1]);
    REQUIRE(a.changes.size() == b.changes.size());
    for (size_t i = 0; i < a.changes.size(); ++i)
        CHECK(a.changes[i].field == b.changes[i].field);

    RegressionPolicy policy;
    RegressionReport r1 = compare_versions(*first[0], confident_sample(600., 19., 1710.),
                                           *first[1], confident_sample(500., 3., 270.), policy);
    RegressionReport r2 = compare_versions(*first[0], confident_sample(600., 19., 1710.),
                                           *first[1], confident_sample(500., 3., 270.), policy);
    REQUIRE(r1.metrics.size() == r2.metrics.size());
    for (size_t i = 0; i < r1.metrics.size(); ++i) {
        CHECK(r1.metrics[i].metric == r2.metrics[i].metric);
        CHECK(r1.metrics[i].change == r2.metrics[i].change);
    }
    CHECK(r1.verdict == r2.verdict);
}
