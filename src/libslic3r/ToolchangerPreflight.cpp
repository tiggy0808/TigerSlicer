///|/ TigerSlicer Phase 2: toolchanger preflight validation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ToolchangerPreflight.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace Slic3r {

bool PreflightReport::blocked() const
{
    return std::any_of(findings.begin(), findings.end(),
        [](const PreflightFinding &finding) {
            return finding.severity == PreflightSeverity::Error;
        });
}

std::vector<PreflightFinding> PreflightReport::errors() const
{
    std::vector<PreflightFinding> out;
    std::copy_if(findings.begin(), findings.end(), std::back_inserter(out),
        [](const PreflightFinding &finding) {
            return finding.severity == PreflightSeverity::Error;
        });
    return out;
}

std::vector<PreflightFinding> PreflightReport::warnings() const
{
    std::vector<PreflightFinding> out;
    std::copy_if(findings.begin(), findings.end(), std::back_inserter(out),
        [](const PreflightFinding &finding) {
            return finding.severity == PreflightSeverity::Warning;
        });
    return out;
}

std::string PreflightReport::to_text() const
{
    std::ostringstream out;
    out << "TigerSlicer toolchanger preflight\n";
    out << "================================\n\n";
    if (findings.empty()) {
        out << "No findings.\n";
        return out.str();
    }
    for (const PreflightFinding &finding : findings) {
        out << (finding.severity == PreflightSeverity::Error ? "ERROR  " : "WARNING")
            << "  [" << finding.code << "] " << finding.message << "\n";
        if (finding.tool.has_value())
            out << "         tool: T" << *finding.tool << "\n";
        if (!finding.object.empty())
            out << "         object: " << finding.object << "\n";
        if (!finding.region.empty())
            out << "         region: " << finding.region << "\n";
        if (!finding.feature.empty())
            out << "         feature: " << finding.feature << "\n";
        if (!finding.setting.empty())
            out << "         setting: " << finding.setting << "\n";
    }
    return out.str();
}

std::vector<double> find_invalid_feedrates(const std::string &gcode)
{
    std::vector<double> invalid;

    // Walk the text so we can honour comments and only read F-words that begin
    // a token; a naive search would trip over "...F0" inside a word.
    bool in_comment = false;
    for (size_t i = 0; i < gcode.size(); ++i) {
        const char c = gcode[i];
        if (c == '\n') { in_comment = false; continue; }
        if (in_comment) continue;
        if (c == ';') { in_comment = true; continue; }
        if (c == '(') {                     // parenthesised comment
            const size_t close = gcode.find(')', i);
            i = close == std::string::npos ? gcode.size() : close;
            continue;
        }
        if (c != 'F' && c != 'f')
            continue;
        // Must start a word, otherwise it is part of a parameter name.
        if (i > 0) {
            const char prev = gcode[i - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' ||
                prev == '.' || prev == '#' || prev == '{')
                continue;
        }

        size_t j = i + 1;
        std::string number;
        while (j < gcode.size() &&
               (std::isdigit(static_cast<unsigned char>(gcode[j])) || gcode[j] == '.' ||
                gcode[j] == '-' || gcode[j] == '+')) {
            number.push_back(gcode[j]);
            ++j;
        }
        if (number.empty())
            continue;   // e.g. a placeholder like F{speed}; not a literal

        try {
            const double value = std::stod(number);
            // Zero, negative, or non-finite feedrates either stall the motion
            // planner or fault the firmware.
            if (!std::isfinite(value) || value <= 0.)
                invalid.push_back(value);
        } catch (const std::exception &) {
            // Unparseable numeric literal - treat as invalid rather than
            // assuming it is fine.
            invalid.push_back(0.);
        }
        i = j - 1;
    }

    return invalid;
}

namespace {

const ToolPreflightState *find_tool(const PreflightInputs &inputs, int tool)
{
    const auto it = std::find_if(inputs.tools.begin(), inputs.tools.end(),
        [tool](const ToolPreflightState &state) { return state.tool == tool; });
    return it == inputs.tools.end() ? nullptr : &*it;
}

} // namespace

PreflightReport run_toolchanger_preflight(const PreflightInputs &inputs)
{
    PreflightReport report;

    auto add = [&report](PreflightSeverity severity, std::string code, std::string message,
                         std::optional<int> tool = std::nullopt, std::string setting = {}) {
        PreflightFinding finding;
        finding.severity = severity;
        finding.code     = std::move(code);
        finding.message  = std::move(message);
        finding.tool     = tool;
        finding.setting  = std::move(setting);
        report.findings.push_back(std::move(finding));
    };

    for (const int tool : inputs.used_tools) {
        const ToolPreflightState *state = find_tool(inputs, tool);
        if (state == nullptr) {
            add(PreflightSeverity::Error, "tool_missing",
                "The plate uses T" + std::to_string(tool) +
                    " but the printer profile does not define it.", tool);
            continue;
        }

        if (state->nozzle_diameter_mm <= 0.)
            add(PreflightSeverity::Error, "nozzle_undefined",
                "T" + std::to_string(tool) + " has no nozzle diameter.", tool, "nozzle_diameter");

        if (!state->filament_selected)
            add(PreflightSeverity::Error, "filament_unselected",
                "T" + std::to_string(tool) + " has no filament selected.", tool);

        // Layer height must sit inside the tool's own printable range.
        if (state->actual_layer_height_mm <= 0.)
            add(PreflightSeverity::Error, "layer_height_invalid",
                "T" + std::to_string(tool) + " has a non-positive layer height.", tool,
                "layer_height");
        else {
            if (state->min_layer_height_mm > 0. &&
                state->actual_layer_height_mm < state->min_layer_height_mm - 1e-9)
                add(PreflightSeverity::Error, "layer_height_below_min",
                    "T" + std::to_string(tool) + " prints below its minimum layer height.",
                    tool, "min_layer_height");
            if (state->max_layer_height_mm > 0. &&
                state->actual_layer_height_mm > state->max_layer_height_mm + 1e-9)
                add(PreflightSeverity::Error, "layer_height_above_max",
                    "T" + std::to_string(tool) + " prints above its maximum layer height.",
                    tool, "max_layer_height");
            // A layer taller than the nozzle cannot be laid down reliably.
            if (state->nozzle_diameter_mm > 0. &&
                state->actual_layer_height_mm > state->nozzle_diameter_mm + 1e-9)
                add(PreflightSeverity::Error, "layer_height_exceeds_nozzle",
                    "T" + std::to_string(tool) +
                        " prints a layer taller than its nozzle diameter.",
                    tool, "layer_height");
        }

        if (state->extrusion_width_mm <= 0.)
            add(PreflightSeverity::Error, "width_invalid",
                "T" + std::to_string(tool) + " has a non-positive extrusion width.", tool,
                "extrusion_width");

        if (state->volumetric_limit_mm3_s <= 0.)
            add(PreflightSeverity::Warning, "flow_limit_unset",
                "T" + std::to_string(tool) + " has no effective volumetric limit; "
                "flow will not be constrained for it.", tool, "max_volumetric_speed");

        if (state->temperature_c <= 0.)
            add(PreflightSeverity::Error, "temperature_invalid",
                "T" + std::to_string(tool) + " has no printing temperature.", tool,
                "temperature");

        if (!state->offset_declared)
            add(PreflightSeverity::Error, "offset_undeclared",
                "T" + std::to_string(tool) + " has no declared tool offset.", tool);
    }

    // Painted detail must be representable by the tool that owns it.
    for (const auto &[tool, detail_mm] : inputs.smallest_detail_mm) {
        const ToolPreflightState *state = find_tool(inputs, tool);
        if (state == nullptr || state->nozzle_diameter_mm <= 0. || detail_mm <= 0.)
            continue;
        if (detail_mm < state->nozzle_diameter_mm - 1e-9)
            add(PreflightSeverity::Warning, "detail_not_representable",
                "T" + std::to_string(tool) + " owns detail finer than its nozzle can print.",
                tool);
    }

    // Custom G-code contracts.
    for (const MacroContract &macro : inputs.macros) {
        for (const double bad : find_invalid_feedrates(macro.gcode)) {
            std::ostringstream message;
            message << "Custom G-code '" << macro.name << "' contains an invalid feedrate (F"
                    << bad << ").";
            add(PreflightSeverity::Error, "invalid_feedrate", message.str(), std::nullopt,
                macro.name);
        }

        for (const std::string &capability : macro.required_capabilities) {
            const bool declared = std::any_of(inputs.tools.begin(), inputs.tools.end(),
                [&capability](const ToolPreflightState &state) {
                    return std::find(state.capabilities.begin(), state.capabilities.end(),
                                     capability) != state.capabilities.end();
                });
            if (!declared)
                add(PreflightSeverity::Error, "capability_undeclared",
                    "Custom G-code '" + macro.name + "' requires '" + capability +
                        "', which no tool declares.", std::nullopt, macro.name);
        }
    }

    return report;
}

} // namespace Slic3r
