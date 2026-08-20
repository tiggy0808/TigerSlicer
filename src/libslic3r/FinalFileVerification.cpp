///|/ TigerSlicer Phase UV8 groundwork: final-file verification and the
///|/ deterministic, atomic export decision.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FinalFileVerification.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace Slic3r {

const char *check_outcome_name(CheckOutcome outcome)
{
    switch (outcome) {
    case CheckOutcome::Match:        return "match";
    case CheckOutcome::Mismatch:     return "mismatch";
    case CheckOutcome::AcceptedLoss: return "accepted loss";
    case CheckOutcome::Unchecked:    return "unchecked";
    }
    return "unchecked";
}

const char *verification_verdict_name(VerificationVerdict verdict)
{
    switch (verdict) {
    case VerificationVerdict::Verified: return "verified";
    case VerificationVerdict::Failed:   return "failed";
    case VerificationVerdict::Unproven: return "unproven";
    }
    return "unproven";
}

const char *publish_decision_name(PublishDecision decision)
{
    switch (decision) {
    case PublishDecision::Publish:     return "publish";
    case PublishDecision::KeepLastGood: return "keep last good";
    case PublishDecision::Discard:     return "discard";
    }
    return "discard";
}

namespace {

// The fields whose mismatch or absence matters most. A mismatch anywhere is a
// failure; an *unchecked* field only leaves the verdict Unproven when it is one
// of these.
const std::set<std::string> &critical_field_names()
{
    static const std::set<std::string> names = {
        "layer count", "resolution", "orientation", "normal exposure",
        "bottom exposure", "format revision", "raster",
    };
    return names;
}

std::string to_text(double value)
{
    std::ostringstream out;
    out.precision(6);
    out << value;
    return out.str();
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

VerificationField check(const std::string &name, bool has_expected, const std::string &expected,
                        bool has_actual, const std::string &actual, bool equal)
{
    VerificationField field;
    field.name     = name;
    field.expected = has_expected ? expected : std::string("<none>");
    field.actual   = has_actual ? actual : std::string("<not decoded>");
    if (! has_actual)
        field.outcome = CheckOutcome::Unchecked;
    else if (! has_expected)
        // The file has a value the manifest does not constrain; not a mismatch,
        // just unconstrained.
        field.outcome = CheckOutcome::Match;
    else
        field.outcome = equal ? CheckOutcome::Match : CheckOutcome::Mismatch;
    return field;
}

} // namespace

FileVerification verify_final_file(const ResinJobManifest     &manifest,
                                   const FinalFileFacts       &facts,
                                   const ConversionLossReport &loss)
{
    FileVerification result;

    // layer count
    result.fields.push_back(check(
        "layer count", true, std::to_string(manifest.layers.size()),
        facts.layer_count.has_value(),
        facts.layer_count.has_value() ? std::to_string(*facts.layer_count) : std::string(),
        facts.layer_count.has_value() && *facts.layer_count == manifest.layers.size()));

    // Z sequence: same count and each within tolerance.
    {
        VerificationField field;
        field.name = "z sequence";
        if (facts.layer_z_mm.empty()) {
            field.outcome = CheckOutcome::Unchecked;
            field.actual  = "<not decoded>";
        } else if (facts.layer_z_mm.size() != manifest.layers.size()) {
            field.outcome  = CheckOutcome::Mismatch;
            field.expected = std::to_string(manifest.layers.size()) + " layers";
            field.actual   = std::to_string(facts.layer_z_mm.size()) + " layers";
        } else {
            bool all_close = true;
            for (size_t i = 0; i < facts.layer_z_mm.size(); ++i) {
                const auto &z = manifest.layers[i].z_mm;
                if (! z.has_value() || std::abs(*z - facts.layer_z_mm[i]) > 1e-4) {
                    all_close = false;
                    field.note = "layer " + std::to_string(i) + " Z differs";
                    break;
                }
            }
            field.outcome = all_close ? CheckOutcome::Match : CheckOutcome::Mismatch;
        }
        result.fields.push_back(field);
    }

    // resolution
    {
        const bool has_expected =
            manifest.resolution_x.has_value() && manifest.resolution_y.has_value();
        const bool has_actual = facts.resolution_x.has_value() && facts.resolution_y.has_value();
        const std::string expected =
            has_expected ? std::to_string(*manifest.resolution_x) + "x" +
                               std::to_string(*manifest.resolution_y)
                         : std::string();
        const std::string actual =
            has_actual ? std::to_string(*facts.resolution_x) + "x" +
                             std::to_string(*facts.resolution_y)
                       : std::string();
        result.fields.push_back(check("resolution", has_expected, expected, has_actual, actual,
                                      has_expected && has_actual && expected == actual));
    }

    // orientation (origin + mirror + rotation, checked as a bundle)
    {
        VerificationField field;
        field.name = "orientation";
        if (! facts.origin.has_value() && ! facts.mirror_x.has_value() &&
            ! facts.mirror_y.has_value() && ! facts.rotation_deg.has_value()) {
            field.outcome = CheckOutcome::Unchecked;
            field.actual  = "<not decoded>";
        } else {
            const bool origin_ok =
                ! facts.origin.has_value() || *facts.origin == manifest.origin;
            const bool mirror_ok =
                (! facts.mirror_x.has_value() || *facts.mirror_x == manifest.mirror_x) &&
                (! facts.mirror_y.has_value() || *facts.mirror_y == manifest.mirror_y);
            const bool rotation_ok =
                ! facts.rotation_deg.has_value() || *facts.rotation_deg == manifest.rotation_deg;
            const bool ok = origin_ok && mirror_ok && rotation_ok;
            field.outcome  = ok ? CheckOutcome::Match : CheckOutcome::Mismatch;
            field.expected = std::string(pixel_origin_name(manifest.origin)) + " mx=" +
                             bool_text(manifest.mirror_x) + " rot=" +
                             std::to_string(manifest.rotation_deg);
        }
        result.fields.push_back(field);
    }

    // exposures
    result.fields.push_back(check(
        "normal exposure", manifest.normal.exposure_s.has_value(),
        manifest.normal.exposure_s.has_value() ? to_text(*manifest.normal.exposure_s)
                                               : std::string(),
        facts.normal_exposure_s.has_value(),
        facts.normal_exposure_s.has_value() ? to_text(*facts.normal_exposure_s) : std::string(),
        manifest.normal.exposure_s.has_value() && facts.normal_exposure_s.has_value() &&
            std::abs(*manifest.normal.exposure_s - *facts.normal_exposure_s) < 1e-3));
    result.fields.push_back(check(
        "bottom exposure", manifest.bottom.exposure_s.has_value(),
        manifest.bottom.exposure_s.has_value() ? to_text(*manifest.bottom.exposure_s)
                                               : std::string(),
        facts.bottom_exposure_s.has_value(),
        facts.bottom_exposure_s.has_value() ? to_text(*facts.bottom_exposure_s) : std::string(),
        manifest.bottom.exposure_s.has_value() && facts.bottom_exposure_s.has_value() &&
            std::abs(*manifest.bottom.exposure_s - *facts.bottom_exposure_s) < 1e-3));

    // format revision
    result.fields.push_back(check(
        "format revision", facts.format_revision.has_value(),
        facts.format_revision.value_or(""), facts.format_revision.has_value(),
        facts.format_revision.value_or(""), true));
    // (A manifest carries no format revision of its own yet; the decoded value
    // is recorded as present, and constrained by the printer profile elsewhere.
    // Left as Match/Unchecked, never a spurious mismatch.)
    if (! facts.format_revision.has_value())
        result.fields.back().outcome = CheckOutcome::Unchecked;

    // raster round-trip, with expected-loss accounting
    {
        VerificationField field;
        field.name = "raster";
        if (! facts.raster_checksum.has_value()) {
            field.outcome = CheckOutcome::Unchecked;
            field.actual  = "<not decoded>";
        } else if (! manifest.metadata.count("raster_checksum")) {
            // The manifest carries no reference checksum, so the decoded raster
            // cannot be compared to anything. Raster is print-critical: leave it
            // Unchecked (-> Unproven) rather than reporting a false Mismatch for a
            // lossless job or silently accepting an unverified raster on a lossy one.
            field.outcome = CheckOutcome::Unchecked;
            field.actual  = *facts.raster_checksum;
        } else {
            // A lossy raster encoding is expected not to round-trip exactly.
            // The UV2 loss report is the authority on whether that was
            // declared: if it classified "raster encoding" as a loss, a
            // checksum difference is an AcceptedLoss, not a Mismatch.
            bool raster_loss_declared = false;
            for (const LossRecord &record : loss.losses)
                if (record.field == "raster encoding")
                    raster_loss_declared = true;

            const bool exact = manifest.metadata.count("raster_checksum") &&
                               manifest.metadata.at("raster_checksum") == *facts.raster_checksum;
            if (exact)
                field.outcome = CheckOutcome::Match;
            else if (raster_loss_declared || ! manifest.encoding.lossless)
                field.outcome = CheckOutcome::AcceptedLoss;
            else
                field.outcome = CheckOutcome::Mismatch;
            field.note = raster_loss_declared
                             ? "a raster difference is accounted for by the declared loss"
                             : std::string();
        }
        result.fields.push_back(field);
    }

    // thumbnails: informational only.
    {
        VerificationField field;
        field.name    = "thumbnails";
        field.outcome = facts.thumbnails_present || manifest.thumbnails.empty()
                            ? CheckOutcome::Match
                            : CheckOutcome::Unchecked;
        result.fields.push_back(field);
    }

    // Roll up.
    bool any_mismatch          = false;
    bool any_critical_unchecked = false;
    for (const VerificationField &field : result.fields) {
        if (field.outcome == CheckOutcome::Mismatch) {
            any_mismatch = true;
            result.mismatches.push_back(field.name);
        } else if (field.outcome == CheckOutcome::Unchecked &&
                   critical_field_names().count(field.name)) {
            any_critical_unchecked = true;
            result.unchecked_critical.push_back(field.name);
        }
    }

    if (any_mismatch) {
        result.verdict = VerificationVerdict::Failed;
        result.summary = std::to_string(result.mismatches.size()) +
                         " field(s) do not match the manifest; the file is not the job";
    } else if (any_critical_unchecked) {
        result.verdict = VerificationVerdict::Unproven;
        result.summary = std::to_string(result.unchecked_critical.size()) +
                         " print-critical field(s) could not be decoded, so the file is not "
                         "proven to match the job";
    } else {
        result.verdict = VerificationVerdict::Verified;
        result.summary = "the decoded file matches the canonical job manifest";
    }
    return result;
}

// --- atomic publish ---------------------------------------------------------

PublishPlan plan_publish(const FileVerification &verification, const PublishState &state)
{
    PublishPlan plan;

    if (verification.verified()) {
        plan.decision = PublishDecision::Publish;
        plan.message  = "verification passed; publishing the temporary file to the final path";
        return plan;
    }

    // Verification did not pass. The last known-good is never overwritten with
    // an unverified result.
    if (state.has_last_good) {
        plan.decision = PublishDecision::KeepLastGood;
        plan.message  = "verification did not pass (" +
                       std::string(verification_verdict_name(verification.verdict)) +
                       "); the last known-good export is kept and the temporary file discarded";
    } else {
        plan.decision = PublishDecision::Discard;
        plan.message  = "verification did not pass and there is no last known-good; nothing is "
                       "published";
    }
    return plan;
}

} // namespace Slic3r
