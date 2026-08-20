///|/ TigerSlicer Phase N: printer/profile capability matching.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterCapabilityMatch_hpp_
#define slic3r_PrinterCapabilityMatch_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"

namespace Slic3r {

// What a device actually told us about itself. Every field is optional
// because discovery must never invent a capability: an absent value means
// "not advertised", which is different from a value that disagrees.
//
// Named PrinterDeviceCapabilities rather than DeviceCapabilities because
// <wingdi.h> defines DeviceCapabilities as a macro expanding to
// DeviceCapabilitiesW under UNICODE. Any GUI translation unit that pulls in
// windows.h would silently rename the symbol and fail only at link time.
struct PrinterDeviceCapabilities
{
    std::string vendor;
    std::string model;
    std::string firmware;

    bool                technology_known { false };
    PrinterTechnology   technology { ptFFF };

    std::optional<int>    tool_count;
    std::optional<double> bed_width_mm;
    std::optional<double> bed_depth_mm;
    std::optional<double> max_height_mm;
    // Output format the device expects, when it says so (e.g. "gcode", "bgcode").
    std::string output_format;
};

// The subset of a TigerSlicer printer preset that matching needs. Keeping this
// a plain summary rather than a Preset reference keeps the rule set free of
// preset-collection and GUI dependencies, so it stays directly testable.
struct PrinterProfileSummary
{
    std::string name;
    std::string vendor;
    std::string model;

    PrinterTechnology technology { ptFFF };

    int    tool_count { 1 };
    double bed_width_mm { 0. };
    double bed_depth_mm { 0. };
    double max_height_mm { 0. };
    std::vector<std::string> output_formats;
};

enum class ProfileMatchLevel
{
    Exact,                   // every advertised fact agrees
    CompatibleWithWarnings,  // usable, but at least one fact needs attention
    Ambiguous,               // several profiles fit equally well
    NoCompatibleProfile,     // nothing fits; a blocking fact disagrees
};

// One checked fact, retained so the UI can show *why* a match was proposed
// rather than asking the user to trust a verdict.
struct MatchEvidence
{
    std::string field;
    std::string device_value;    // empty when the device did not advertise it
    std::string profile_value;
    bool        agrees { true };
    bool        blocking { false };  // disagreement rules the profile out
};

struct ProfileMatch
{
    ProfileMatchLevel level { ProfileMatchLevel::NoCompatibleProfile };
    // Best candidate, if any. Never applied automatically by this code.
    std::string best_profile;
    // Populated when several candidates tie, so the user can choose.
    std::vector<std::string> tied_profiles;
    std::vector<MatchEvidence> evidence;
    std::vector<std::string> warnings;
};

// Compare a discovered device against the installed printer profiles.
//
// This function only ever *reports*. It never selects, applies, or mutates a
// preset: silently adopting a slicing profile because a device appeared on the
// network is exactly the failure mode Phase N forbids.
ProfileMatch match_printer_profile(const PrinterDeviceCapabilities &device,
                                   const std::vector<PrinterProfileSummary> &profiles);

} // namespace Slic3r

#endif // slic3r_PrinterCapabilityMatch_hpp_
