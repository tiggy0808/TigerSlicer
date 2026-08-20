///|/ TigerSlicer Phase UV3 groundwork: output format and revision selection.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ResinFormatSelection.hpp"

#include <algorithm>
#include <set>

namespace Slic3r {

const char *format_support_evidence_name(FormatSupportEvidence evidence)
{
    switch (evidence) {
    case FormatSupportEvidence::Unknown:               return "unknown";
    case FormatSupportEvidence::StructurallySupported: return "structurally supported";
    case FormatSupportEvidence::ProfileDeclared:       return "profile declared";
    case FormatSupportEvidence::VerifiedOnHardware:    return "verified on hardware";
    }
    return "unknown";
}

const char *format_selection_verdict_name(FormatSelectionVerdict verdict)
{
    switch (verdict) {
    case FormatSelectionVerdict::Selected:           return "selected";
    case FormatSelectionVerdict::Pinned:             return "pinned";
    case FormatSelectionVerdict::Ambiguous:          return "ambiguous";
    case FormatSelectionVerdict::NoCompatibleFormat: return "no compatible format";
    case FormatSelectionVerdict::NoFormatsDeclared:  return "no formats declared";
    case FormatSelectionVerdict::PinnedUnavailable:  return "pinned unavailable";
    }
    return "no formats declared";
}

const char *revision_agreement_name(RevisionAgreement agreement)
{
    switch (agreement) {
    case RevisionAgreement::Agrees:          return "agrees";
    case RevisionAgreement::RevisionChanged: return "revision changed";
    case RevisionAgreement::FormatDropped:   return "format dropped";
    }
    return "format dropped";
}

bool ResinFormatOption::has(ResinCapability capability) const
{
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

std::vector<ResinCapability> job_required_capabilities(const ResinJobManifest &job)
{
    std::set<ResinCapability> needed;

    for (const ResinLayer &layer : job.layers) {
        if (! layer.override_exposure.has_value())
            continue;
        const ResinExposure &e = *layer.override_exposure;
        if (e.exposure_s.has_value() || e.light_off_delay_s.has_value())
            needed.insert(ResinCapability::PerLayerExposure);
        if (e.lift_distance_mm.has_value() || e.lift_speed_mm_min.has_value() ||
            e.retract_distance_mm.has_value() || e.retract_speed_mm_min.has_value())
            needed.insert(ResinCapability::PerLayerLift);
        if (e.light_pwm.has_value())
            needed.insert(ResinCapability::PerLayerLightPwm);
        if (e.tilt_deg.has_value())
            needed.insert(ResinCapability::Tilt);
    }
    if (job.normal.tilt_deg.has_value() || job.bottom.tilt_deg.has_value())
        needed.insert(ResinCapability::Tilt);
    if (job.normal.light_pwm.has_value() || job.bottom.light_pwm.has_value())
        needed.insert(ResinCapability::LightPwm);
    if (! job.thumbnails.empty())
        needed.insert(ResinCapability::Thumbnails);

    return std::vector<ResinCapability>(needed.begin(), needed.end());
}

namespace {

// How many of the job's required capabilities an option carries.
size_t carried(const ResinFormatOption &option, const std::vector<ResinCapability> &needed)
{
    size_t n = 0;
    for (ResinCapability capability : needed)
        if (option.has(capability))
            ++n;
    return n;
}

std::vector<ResinCapability> unmet(const ResinFormatOption            &option,
                                   const std::vector<ResinCapability> &needed)
{
    std::vector<ResinCapability> missing;
    for (ResinCapability capability : needed)
        if (! option.has(capability))
            missing.push_back(capability);
    return missing;
}

} // namespace

FormatSelection select_output_format(const PrinterFormatProfile &profile,
                                     const ResinJobManifest     &job)
{
    FormatSelection selection;

    if (profile.options.empty()) {
        selection.verdict = FormatSelectionVerdict::NoFormatsDeclared;
        selection.message = "the printer profile declares no output formats";
        return selection;
    }

    const std::vector<ResinCapability> needed = job_required_capabilities(job);

    // Eligible options: all, or only hardware-verified when required.
    std::vector<const ResinFormatOption *> eligible;
    for (const ResinFormatOption &option : profile.options) {
        if (profile.require_verified &&
            option.evidence != FormatSupportEvidence::VerifiedOnHardware)
            continue;
        eligible.push_back(&option);
    }

    // A pinned format is honoured when offered - never silently swapped.
    if (profile.pinned_format.has_value()) {
        const ResinFormatOption *pinned = nullptr;
        for (const ResinFormatOption *option : eligible)
            if (option->format == *profile.pinned_format)
                pinned = option;
        if (pinned == nullptr) {
            // It might exist but be excluded by require_verified; say so
            // distinctly from "not offered at all".
            bool exists = false;
            for (const ResinFormatOption &option : profile.options)
                if (option.format == *profile.pinned_format)
                    exists = true;
            selection.verdict = FormatSelectionVerdict::PinnedUnavailable;
            selection.message =
                exists ? "the pinned format \"" + *profile.pinned_format +
                             "\" is offered but not hardware-verified, and verification is "
                             "required"
                       : "the pinned format \"" + *profile.pinned_format +
                             "\" is not offered by this printer";
            return selection;
        }
        selection.verdict            = FormatSelectionVerdict::Pinned;
        selection.format             = pinned->format;
        selection.revision           = pinned->revision;
        selection.evidence           = pinned->evidence;
        selection.unmet_capabilities = unmet(*pinned, needed);
        selection.message            = "the user's pinned format \"" + pinned->format +
                            "\" is honoured";
        return selection;
    }

    if (eligible.empty()) {
        selection.verdict = FormatSelectionVerdict::NoCompatibleFormat;
        selection.message = "no offered format meets the verification requirement";
        return selection;
    }

    // Rank: strongest evidence first, then most of the job's capabilities
    // carried. A genuine tie on both is reported, not broken by order.
    auto better = [&needed](const ResinFormatOption *a, const ResinFormatOption *b) {
        if (a->evidence != b->evidence)
            return uint8_t(a->evidence) > uint8_t(b->evidence);
        return carried(*a, needed) > carried(*b, needed);
    };

    const ResinFormatOption *best = eligible.front();
    for (const ResinFormatOption *option : eligible)
        if (better(option, best))
            best = option;

    std::vector<const ResinFormatOption *> tied;
    for (const ResinFormatOption *option : eligible)
        if (option->evidence == best->evidence &&
            carried(*option, needed) == carried(*best, needed))
            tied.push_back(option);

    if (tied.size() > 1) {
        // Distinct formats tying is an ambiguity; the same format at different
        // revisions is not - the newest-declared revision of that one format
        // is taken. Detect by format name.
        std::set<std::string> formats;
        for (const ResinFormatOption *option : tied)
            formats.insert(option->format);
        if (formats.size() > 1) {
            selection.verdict = FormatSelectionVerdict::Ambiguous;
            for (const std::string &format : formats)
                selection.tied.push_back(format);
            std::sort(selection.tied.begin(), selection.tied.end());
            selection.message = "several formats are equally suitable; the user must choose";
            return selection;
        }

        // Same format at multiple revisions: take the newest revision, rather
        // than whichever option happened to be declared first. Compare
        // numerically when both revisions are plain integers (so "10" > "4");
        // otherwise fall back to declaration order (last-declared = newest).
        auto small_int = [](const std::string &s) {
            return ! s.empty() && s.size() <= 9 &&
                   std::all_of(s.begin(), s.end(),
                               [](char c) { return c >= '0' && c <= '9'; });
        };
        best = tied.front();
        for (const ResinFormatOption *option : tied) {
            const bool both_num = small_int(option->revision) && small_int(best->revision);
            const bool newer = both_num
                ? std::stol(option->revision) > std::stol(best->revision)
                : false;
            if (newer || ! both_num)
                best = option;
        }
    }

    selection.verdict            = FormatSelectionVerdict::Selected;
    selection.format             = best->format;
    selection.revision           = best->revision;
    selection.evidence           = best->evidence;
    selection.unmet_capabilities = unmet(*best, needed);
    selection.message = "selected \"" + best->format + "\" (" +
                        format_support_evidence_name(best->evidence) + ")";
    return selection;
}

RevisionCheck check_committed_revision(const PrinterFormatProfile &profile,
                                       const std::string          &committed_format,
                                       const std::string          &committed_revision)
{
    RevisionCheck check;

    const ResinFormatOption *same_format = nullptr;
    bool                     exact       = false;
    for (const ResinFormatOption &option : profile.options) {
        if (option.format != committed_format)
            continue;
        same_format = &option;
        if (option.revision == committed_revision) {
            exact = true;
            break;
        }
    }

    if (exact) {
        check.agreement     = RevisionAgreement::Agrees;
        check.blocks_export = false;
        check.message = "the committed format and revision are still offered by the printer";
    } else if (same_format != nullptr) {
        check.agreement     = RevisionAgreement::RevisionChanged;
        check.blocks_export = true;
        check.message = "the job was sliced for " + committed_format + " revision \"" +
                        committed_revision + "\" but the printer now offers revision \"" +
                        same_format->revision +
                        "\"; export is blocked rather than re-encoded silently";
    } else {
        check.agreement     = RevisionAgreement::FormatDropped;
        check.blocks_export = true;
        check.message = "the printer no longer offers " + committed_format +
                        "; export is blocked rather than routed to another format";
    }
    return check;
}

} // namespace Slic3r
