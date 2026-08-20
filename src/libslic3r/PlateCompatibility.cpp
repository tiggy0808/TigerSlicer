///|/ TigerSlicer Phase N: live plate-to-printer compatibility reporting.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PlateCompatibility.hpp"

#include <algorithm>
#include <cmath>

#include <boost/algorithm/string.hpp>

namespace Slic3r {

namespace {

std::string normalize(const std::string &value)
{
    return boost::algorithm::to_lower_copy(boost::algorithm::trim_copy(value));
}

std::string mm(double value)
{
    std::string text = std::to_string(value);
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text + " mm";
}

} // namespace

CompatibilityReport check_plate_compatibility(const PlateRequirements &plate,
                                              const PrinterState &printer)
{
    CompatibilityReport report;

    auto note = [&report](CompatibilityState state, std::string area, std::string detail) {
        report.findings.push_back({ state, std::move(area), std::move(detail) });
        // Severity only ever escalates; a later pass cannot clear an earlier block.
        if (state == CompatibilityState::Blocked)
            report.state = CompatibilityState::Blocked;
        else if (state == CompatibilityState::Warning &&
                 report.state != CompatibilityState::Blocked)
            report.state = CompatibilityState::Warning;
    };

    // --- technology -----------------------------------------------------
    if (plate.technology != printer.profile.technology)
        note(CompatibilityState::Blocked, "Technology",
             "This plate is prepared for a different printing technology than the selected printer.");
    else
        note(CompatibilityState::Pass, "Technology", "Printer technology matches the plate.");

    // --- build volume ---------------------------------------------------
    auto check_axis = [&](const char *axis, const std::optional<double> &needed, double available) {
        if (!needed.has_value()) {
            note(CompatibilityState::Warning, "Build volume",
                 std::string("The plate's ") + axis + " extent is not known yet.");
            return;
        }
        if (available <= 0.) {
            note(CompatibilityState::Warning, "Build volume",
                 std::string("The printer does not report its ") + axis + " dimension.");
            return;
        }
        if (*needed > available + 1e-6)
            note(CompatibilityState::Blocked, "Build volume",
                 std::string("The plate needs ") + mm(*needed) + " in " + axis +
                     " but the printer offers " + mm(available) + ".");
    };
    check_axis("width", plate.width_mm, printer.profile.bed_width_mm);
    check_axis("depth", plate.depth_mm, printer.profile.bed_depth_mm);
    check_axis("height", plate.height_mm, printer.profile.max_height_mm);

    // --- tools and nozzles ----------------------------------------------
    const int highest_tool = plate.used_tools.empty()
        ? -1
        : *std::max_element(plate.used_tools.begin(), plate.used_tools.end());
    if (highest_tool >= printer.profile.tool_count)
        note(CompatibilityState::Blocked, "Tools",
             "The plate uses tool T" + std::to_string(highest_tool) + " but the printer has " +
                 std::to_string(printer.profile.tool_count) + " tool(s).");

    if (!plate.required_nozzle_mm.empty()) {
        if (!printer.installed_nozzles_mm.has_value()) {
            note(CompatibilityState::Warning, "Nozzles",
                 "The printer does not report its installed nozzles, so nozzle sizes could not be verified.");
        } else {
            const std::vector<double> &installed = *printer.installed_nozzles_mm;
            for (size_t i = 0; i < plate.used_tools.size() && i < plate.required_nozzle_mm.size(); ++i) {
                const int tool = plate.used_tools[i];
                if (tool < 0 || size_t(tool) >= installed.size()) {
                    note(CompatibilityState::Warning, "Nozzles",
                         "The printer did not report a nozzle for T" + std::to_string(tool) + ".");
                    continue;
                }
                if (std::abs(installed[tool] - plate.required_nozzle_mm[i]) > 1e-6)
                    note(CompatibilityState::Blocked, "Nozzles",
                         "T" + std::to_string(tool) + " needs a " + mm(plate.required_nozzle_mm[i]) +
                             " nozzle but a " + mm(installed[tool]) + " nozzle is installed.");
            }
        }
    }

    // --- materials -------------------------------------------------------
    if (!plate.required_materials.empty()) {
        if (!printer.loaded_materials.has_value()) {
            // Unknown is explicitly not "fine".
            note(CompatibilityState::Warning, "Materials",
                 "The printer does not report loaded materials, so they could not be verified.");
        } else {
            const std::vector<std::string> &loaded = *printer.loaded_materials;
            for (size_t i = 0; i < plate.required_materials.size(); ++i) {
                const std::string &needed = plate.required_materials[i];
                if (needed.empty())
                    continue;
                if (i >= loaded.size() || loaded[i].empty()) {
                    note(CompatibilityState::Warning, "Materials",
                         "No material reported for T" + std::to_string(int(i)) + ".");
                    continue;
                }
                if (normalize(loaded[i]) != normalize(needed))
                    note(CompatibilityState::Warning, "Materials",
                         "T" + std::to_string(int(i)) + " expects " + needed + " but " +
                             loaded[i] + " is loaded.");
            }
        }
    }

    // --- layer height ----------------------------------------------------
    if (plate.layer_height_mm.has_value()) {
        if (printer.min_layer_height_mm.has_value() &&
            *plate.layer_height_mm < *printer.min_layer_height_mm - 1e-9)
            note(CompatibilityState::Blocked, "Layer height",
                 "The layer height " + mm(*plate.layer_height_mm) +
                     " is below the printer minimum of " + mm(*printer.min_layer_height_mm) + ".");
        if (printer.max_layer_height_mm.has_value() &&
            *plate.layer_height_mm > *printer.max_layer_height_mm + 1e-9)
            note(CompatibilityState::Blocked, "Layer height",
                 "The layer height " + mm(*plate.layer_height_mm) +
                     " exceeds the printer maximum of " + mm(*printer.max_layer_height_mm) + ".");
    }

    // --- output format ----------------------------------------------------
    if (!plate.output_format.empty() && !printer.profile.output_formats.empty()) {
        const std::string wanted = normalize(plate.output_format);
        const bool supported = std::any_of(
            printer.profile.output_formats.begin(), printer.profile.output_formats.end(),
            [&wanted](const std::string &format) { return normalize(format) == wanted; });
        if (!supported)
            note(CompatibilityState::Blocked, "Output format",
                 "The printer does not accept " + plate.output_format + " files.");
    }

    // --- required firmware capabilities ------------------------------------
    for (const std::string &capability : plate.required_capabilities) {
        const bool present = std::any_of(
            printer.supported_capabilities.begin(), printer.supported_capabilities.end(),
            [&capability](const std::string &have) { return normalize(have) == normalize(capability); });
        if (!present)
            note(CompatibilityState::Blocked, "Firmware",
                 "The printer does not report support for '" + capability + "', which this plate requires.");
    }

    // --- readiness and storage ---------------------------------------------
    if (!printer.ready)
        note(CompatibilityState::Warning, "Printer state",
             "The printer is not currently reporting itself as ready.");

    if (printer.free_storage_bytes.has_value() && printer.job_size_bytes.has_value() &&
        *printer.job_size_bytes > *printer.free_storage_bytes)
        note(CompatibilityState::Blocked, "Storage",
             "The job is larger than the free space the printer reports.");

    return report;
}

} // namespace Slic3r
