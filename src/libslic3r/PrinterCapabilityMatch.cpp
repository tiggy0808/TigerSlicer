///|/ TigerSlicer Phase N: printer/profile capability matching.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterCapabilityMatch.hpp"

#include <algorithm>
#include <cmath>

#include <boost/algorithm/string.hpp>

namespace Slic3r {

namespace {

std::string normalize(const std::string &value)
{
    std::string out = boost::algorithm::to_lower_copy(value);
    boost::algorithm::trim(out);
    // Model names differ cosmetically between advertisements and profiles
    // ("MK4 IS" vs "mk4is"); compare on alphanumerics only.
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](unsigned char c) { return !std::isalnum(c); }),
              out.end());
    return out;
}

std::string to_text(double value)
{
    std::string text = std::to_string(value);
    // Trim trailing zeros so evidence reads "250" rather than "250.000000".
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text;
}

// Scored comparison of one profile against the device.
struct Scored
{
    const PrinterProfileSummary *profile { nullptr };
    int  agreements { 0 };
    bool blocked { false };
    std::vector<MatchEvidence> evidence;
    std::vector<std::string> warnings;
};

Scored score_profile(const PrinterDeviceCapabilities &device, const PrinterProfileSummary &profile)
{
    Scored scored;
    scored.profile = &profile;

    auto add = [&scored](const std::string &field, const std::string &device_value,
                         const std::string &profile_value, bool agrees, bool blocking) {
        MatchEvidence item;
        item.field         = field;
        item.device_value  = device_value;
        item.profile_value = profile_value;
        item.agrees        = agrees;
        item.blocking      = blocking;
        scored.evidence.push_back(std::move(item));
        if (agrees)
            ++scored.agreements;
        if (!agrees && blocking)
            scored.blocked = true;
    };

    // Technology is blocking: an FDM profile can never drive a resin printer,
    // and offering one would be actively unsafe.
    if (device.technology_known) {
        const bool agrees = device.technology == profile.technology;
        add("technology",
            device.technology == ptSLA ? "Resin (MSLA)" : "FDM",
            profile.technology == ptSLA ? "Resin (MSLA)" : "FDM",
            agrees, true);
        if (!agrees)
            scored.warnings.push_back("This profile is for a different printer technology.");
    }

    if (!device.vendor.empty() && !profile.vendor.empty())
        add("manufacturer", device.vendor, profile.vendor,
            normalize(device.vendor) == normalize(profile.vendor), false);

    if (!device.model.empty() && !profile.model.empty()) {
        const std::string device_model  = normalize(device.model);
        const std::string profile_model = normalize(profile.model);
        // Accept a containment match so "MK4" matches an "Original Prusa MK4"
        // profile, but record it as evidence rather than treating it as exact.
        const bool agrees = device_model == profile_model ||
                            profile_model.find(device_model) != std::string::npos ||
                            device_model.find(profile_model) != std::string::npos;
        add("model", device.model, profile.model, agrees, false);
        if (!agrees)
            scored.warnings.push_back("The advertised model does not match this profile.");
    }

    if (device.tool_count.has_value()) {
        const bool agrees = *device.tool_count == profile.tool_count;
        add("tools", std::to_string(*device.tool_count),
            std::to_string(profile.tool_count), agrees, false);
        if (!agrees)
            scored.warnings.push_back(
                "The printer reports a different number of tools than this profile expects.");
    }

    // Build volume: a profile larger than the machine would let the user slice
    // something the printer physically cannot run, so it blocks.
    auto check_axis = [&](const char *field, const std::optional<double> &device_value,
                          double profile_value) {
        if (!device_value.has_value() || profile_value <= 0.)
            return;
        // Tolerate sub-millimetre reporting differences.
        const bool agrees = profile_value <= *device_value + 0.5;
        add(field, to_text(*device_value), to_text(profile_value), agrees, true);
        if (!agrees)
            scored.warnings.push_back(
                "This profile's build volume is larger than the printer reports.");
    };
    check_axis("bed width", device.bed_width_mm, profile.bed_width_mm);
    check_axis("bed depth", device.bed_depth_mm, profile.bed_depth_mm);
    check_axis("max height", device.max_height_mm, profile.max_height_mm);

    if (!device.output_format.empty() && !profile.output_formats.empty()) {
        const std::string wanted = normalize(device.output_format);
        const bool agrees = std::any_of(profile.output_formats.begin(), profile.output_formats.end(),
            [&wanted](const std::string &format) { return normalize(format) == wanted; });
        add("output format", device.output_format,
            boost::algorithm::join(profile.output_formats, ", "), agrees, true);
        if (!agrees)
            scored.warnings.push_back(
                "This profile does not produce the file format the printer expects.");
    }

    return scored;
}

} // namespace

ProfileMatch match_printer_profile(const PrinterDeviceCapabilities &device,
                                   const std::vector<PrinterProfileSummary> &profiles)
{
    ProfileMatch result;

    std::vector<Scored> viable;
    for (const PrinterProfileSummary &profile : profiles) {
        Scored scored = score_profile(device, profile);
        if (!scored.blocked)
            viable.push_back(std::move(scored));
    }

    if (viable.empty()) {
        result.level = ProfileMatchLevel::NoCompatibleProfile;
        // Report why the closest profile was rejected instead of saying
        // nothing; "no compatible profile" with no reason is not actionable.
        if (!profiles.empty()) {
            Scored closest = score_profile(device, profiles.front());
            for (const PrinterProfileSummary &profile : profiles) {
                Scored scored = score_profile(device, profile);
                if (scored.agreements > closest.agreements)
                    closest = std::move(scored);
            }
            result.evidence = std::move(closest.evidence);
            result.warnings = std::move(closest.warnings);
        }
        return result;
    }

    const int best_score = std::max_element(viable.begin(), viable.end(),
        [](const Scored &a, const Scored &b) { return a.agreements < b.agreements; })->agreements;

    std::vector<const Scored *> best;
    for (const Scored &scored : viable)
        if (scored.agreements == best_score)
            best.push_back(&scored);

    result.evidence = best.front()->evidence;
    result.warnings = best.front()->warnings;
    result.best_profile = best.front()->profile->name;

    if (best.size() > 1) {
        // Several profiles fit equally well; the user must choose. Never guess.
        result.level = ProfileMatchLevel::Ambiguous;
        for (const Scored *scored : best)
            result.tied_profiles.push_back(scored->profile->name);
        result.best_profile.clear();
        return result;
    }

    const bool every_check_agrees = std::all_of(result.evidence.begin(), result.evidence.end(),
        [](const MatchEvidence &item) { return item.agrees; });
    // An exact match additionally requires that the device told us something
    // identifying; agreeing on nothing is not agreement.
    result.level = (every_check_agrees && !result.evidence.empty())
                       ? ProfileMatchLevel::Exact
                       : ProfileMatchLevel::CompatibleWithWarnings;
    return result;
}

} // namespace Slic3r
