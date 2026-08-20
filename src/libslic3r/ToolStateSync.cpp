///|/ TigerSlicer Phase N: loaded tool and material synchronization.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ToolStateSync.hpp"

#include <algorithm>
#include <cmath>

#include <boost/algorithm/string.hpp>

namespace Slic3r {

const char *to_string(StateProvenance provenance)
{
    switch (provenance) {
    case StateProvenance::Sensed:     return "sensed";
    case StateProvenance::Configured: return "configured";
    case StateProvenance::Assumed:    return "assumed";
    case StateProvenance::Unknown:    return "unknown";
    }
    return "unknown";
}

bool ToolSyncReport::blocked() const
{
    return std::any_of(mismatches.begin(), mismatches.end(),
        [](const ToolMismatch &mismatch) { return mismatch.severity == MismatchSeverity::Blocking; });
}

bool ToolSyncReport::has_warnings() const
{
    return std::any_of(mismatches.begin(), mismatches.end(),
        [](const ToolMismatch &mismatch) { return mismatch.severity == MismatchSeverity::Warning; });
}

namespace {

std::string normalize(const std::string &value)
{
    return boost::algorithm::to_lower_copy(boost::algorithm::trim_copy(value));
}

const ToolState *find_tool(const std::vector<ToolState> &actual, int index)
{
    const auto it = std::find_if(actual.begin(), actual.end(),
        [index](const ToolState &state) { return state.index == index; });
    return it == actual.end() ? nullptr : &*it;
}

} // namespace

ToolSyncReport compare_tool_state(const std::vector<ToolRequirement> &required,
                                  const std::vector<ToolState> &actual)
{
    ToolSyncReport report;

    auto note = [&report](int tool, std::string field, MismatchSeverity severity,
                          StateProvenance provenance, std::string detail) {
        report.mismatches.push_back(
            { tool, std::move(field), severity, std::move(detail), provenance });
    };

    for (const ToolRequirement &need : required) {
        const ToolState *state = find_tool(actual, need.index);
        if (state == nullptr) {
            note(need.index, "tool", MismatchSeverity::Blocking, StateProvenance::Unknown,
                 "The printer does not report a tool T" + std::to_string(need.index) + ".");
            continue;
        }

        // A tool the printer says is unavailable cannot run the job at all.
        if (state->available.known() && state->available.value == false)
            note(need.index, "availability", MismatchSeverity::Blocking,
                 state->available.provenance,
                 "T" + std::to_string(need.index) + " is reported as unavailable.");

        // --- nozzle: wrong geometry produces a wrong print -----------------
        if (need.nozzle_mm.has_value()) {
            if (!state->nozzle_mm.known())
                note(need.index, "nozzle", MismatchSeverity::Warning, state->nozzle_mm.provenance,
                     "The nozzle fitted to T" + std::to_string(need.index) +
                         " could not be verified.");
            else if (std::abs(*state->nozzle_mm.value - *need.nozzle_mm) > 1e-6)
                note(need.index, "nozzle", MismatchSeverity::Blocking, state->nozzle_mm.provenance,
                     "T" + std::to_string(need.index) + " has a different nozzle than the plate needs.");
        }

        // --- material --------------------------------------------------------
        if (need.material.has_value()) {
            if (!state->material.known()) {
                note(need.index, "material", MismatchSeverity::Warning, state->material.provenance,
                     "The material loaded in T" + std::to_string(need.index) +
                         " could not be verified.");
            } else if (normalize(*state->material.value) != normalize(*need.material)) {
                // Even a sensed mismatch is a warning rather than a block: the
                // user may knowingly print PETG settings on a PLA spool.
                note(need.index, "material", MismatchSeverity::Warning, state->material.provenance,
                     "T" + std::to_string(need.index) + " expects " + *need.material +
                         " but " + *state->material.value + " is loaded.");
            }
        }

        // --- colour: cosmetic, and rarely actually sensed ---------------------
        if (need.color.has_value() && state->color.known() &&
            normalize(*state->color.value) != normalize(*need.color))
            note(need.index, "color", MismatchSeverity::Info, state->color.provenance,
                 "T" + std::to_string(need.index) + " colour differs from the plate's.");

        // --- remaining quantity ------------------------------------------------
        if (need.needed_g.has_value() && state->remaining_g.known() &&
            *state->remaining_g.value < *need.needed_g)
            note(need.index, "remaining", MismatchSeverity::Warning, state->remaining_g.provenance,
                 "T" + std::to_string(need.index) + " may not have enough material for this print.");

        // --- calibration identity ----------------------------------------------
        if (!need.calibration_id.empty() && !state->calibration_id.empty() &&
            need.calibration_id != state->calibration_id)
            note(need.index, "calibration", MismatchSeverity::Warning, StateProvenance::Configured,
                 "T" + std::to_string(need.index) +
                     " was calibrated under a different calibration set than this plate assumes.");
    }

    return report;
}

} // namespace Slic3r
