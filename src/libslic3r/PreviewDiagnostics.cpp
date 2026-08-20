///|/ TigerSlicer Phase 12: preview filtering, diagnostics and estimates.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PreviewDiagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

const char *thermal_state_name(ThermalState state)
{
    switch (state) {
    case ThermalState::Active:  return "active";
    case ThermalState::Standby: return "standby";
    case ThermalState::Idle:    return "idle";
    case ThermalState::Unknown: return "unknown";
    }
    return "unknown";
}

const char *prep_action_name(PrepAction action)
{
    switch (action) {
    case PrepAction::None:  return "none";
    case PrepAction::Purge: return "purge";
    case PrepAction::Brush: return "brush";
    case PrepAction::Prime: return "prime";
    case PrepAction::Cut:   return "cut";
    }
    return "none";
}

const char *comparison_status_name(ComparisonStatus status)
{
    switch (status) {
    case ComparisonStatus::Comparable:           return "comparable";
    case ComparisonStatus::SingleToolImpossible: return "single tool impossible";
    case ComparisonStatus::Incomparable:         return "incomparable";
    }
    return "incomparable";
}

bool PreviewFilter::empty() const
{
    return tools.empty() && display_colors.empty() && features.empty() &&
           thermal_states.empty() && prep_actions.empty() && collision_verdicts.empty() &&
           change_reasons.empty() && !min_print_z && !max_print_z && !min_layer_height_mm &&
           !max_layer_height_mm && !min_extrusion_width_mm && !max_extrusion_width_mm &&
           !min_flow_mm3_s && !max_flow_mm3_s && !tool_grid_height_mm &&
           !on_synchronization_plane && !from_chronological_index && !to_chronological_index;
}

namespace {

template<typename T>
bool allowed(const std::vector<T> &wanted, const T &value)
{
    // An empty list is "no opinion", not "nothing matches".
    return wanted.empty() || std::find(wanted.begin(), wanted.end(), value) != wanted.end();
}

bool within(const std::optional<double> &low, const std::optional<double> &high, double value)
{
    if (low.has_value() && value + EPSILON < *low)
        return false;
    if (high.has_value() && value > *high + EPSILON)
        return false;
    return true;
}

std::string number(double value, int precision = 2)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

} // namespace

std::vector<size_t> filter_preview(const std::vector<PreviewRecord> &records,
                                   const PreviewFilter              &filter)
{
    std::vector<size_t> selection;
    for (size_t index = 0; index < records.size(); ++index) {
        const PreviewRecord &record = records[index];

        if (!allowed(filter.tools, record.tool))
            continue;
        if (!allowed(filter.display_colors, record.display_color))
            continue;
        if (!allowed(filter.features, record.feature))
            continue;
        if (!allowed(filter.thermal_states, record.thermal_state))
            continue;
        if (!allowed(filter.prep_actions, record.prep_action))
            continue;
        if (!allowed(filter.collision_verdicts, record.collision))
            continue;

        if (!filter.change_reasons.empty()) {
            // An operation with no toolchange cannot match a change-reason
            // filter; it is not an unknown match, it simply has no reason.
            if (!record.change_reason.has_value() ||
                !allowed(filter.change_reasons, *record.change_reason))
                continue;
        }

        if (!within(filter.min_print_z, filter.max_print_z, record.print_z))
            continue;
        if (!within(filter.min_layer_height_mm, filter.max_layer_height_mm,
                    record.layer_height_mm))
            continue;
        if (!within(filter.min_extrusion_width_mm, filter.max_extrusion_width_mm,
                    record.extrusion_width_mm))
            continue;
        if (!within(filter.min_flow_mm3_s, filter.max_flow_mm3_s, record.flow_mm3_s))
            continue;

        if (filter.tool_grid_height_mm.has_value() &&
            std::abs(record.tool_grid_height_mm - *filter.tool_grid_height_mm) > EPSILON)
            continue;
        if (filter.on_synchronization_plane.has_value() &&
            record.on_synchronization_plane != *filter.on_synchronization_plane)
            continue;

        if (filter.from_chronological_index.has_value() &&
            record.chronological_index < *filter.from_chronological_index)
            continue;
        if (filter.to_chronological_index.has_value() &&
            record.chronological_index > *filter.to_chronological_index)
            continue;

        selection.push_back(index);
    }

    std::sort(selection.begin(), selection.end(), [&](size_t a, size_t b) {
        return records[a].chronological_index < records[b].chronological_index;
    });
    return selection;
}

PreviewTotals summarize_preview(const std::vector<PreviewRecord> &records,
                                const std::vector<size_t>        &selection)
{
    PreviewTotals totals;
    for (size_t index : selection) {
        if (index >= records.size())
            continue;
        const PreviewRecord &record = records[index];
        ++totals.operations;
        totals.time_s    += record.time_s;
        totals.waste_mm3 += record.waste_mm3;
        if (record.change_reason.has_value())
            ++totals.toolchanges;
        // An unproven operation is counted separately from a colliding one:
        // "we did not check" must not read as "we checked and it is fine".
        if (record.collision == CollisionVerdict::Unproven)
            ++totals.unproven_operations;
        else if (record.collision == CollisionVerdict::Collision)
            ++totals.colliding_operations;
    }
    return totals;
}

SingleToolComparison compare_against_single_tool(
    const std::vector<PreviewRecord> &records,
    int                               single_tool,
    const std::optional<PreviewTotals> &single_tool_estimate,
    const std::vector<std::string>     &impossible_reasons)
{
    SingleToolComparison comparison;

    std::vector<size_t> everything(records.size());
    for (size_t index = 0; index < records.size(); ++index)
        everything[index] = index;
    comparison.mixed = summarize_preview(records, everything);

    if (!impossible_reasons.empty()) {
        // The job cannot be done on one tool, so there is no honest baseline
        // to compare against. Presenting a number here would be fiction.
        comparison.status  = ComparisonStatus::SingleToolImpossible;
        comparison.reasons = impossible_reasons;
        comparison.reasons.insert(comparison.reasons.begin(),
                                  "this job cannot be produced on T" +
                                      std::to_string(single_tool) + " alone");
        return comparison;
    }

    if (!single_tool_estimate.has_value()) {
        comparison.status = ComparisonStatus::Incomparable;
        comparison.reasons.push_back("no single-tool estimate was supplied, so the two cannot "
                                     "be compared");
        return comparison;
    }

    comparison.status      = ComparisonStatus::Comparable;
    comparison.single_tool = *single_tool_estimate;
    comparison.time_delta_s = comparison.mixed.time_s - comparison.single_tool.time_s;
    comparison.waste_delta_mm3 =
        comparison.mixed.waste_mm3 - comparison.single_tool.waste_mm3;

    std::ostringstream time_line;
    time_line << "the mixed-tool schedule takes " << number(std::abs(comparison.time_delta_s))
              << " s " << (comparison.time_delta_s >= 0. ? "longer" : "less")
              << " than T" << single_tool << " alone";
    comparison.reasons.push_back(time_line.str());

    std::ostringstream waste_line;
    waste_line << "and wastes " << number(std::abs(comparison.waste_delta_mm3)) << " mm3 "
               << (comparison.waste_delta_mm3 >= 0. ? "more" : "less");
    comparison.reasons.push_back(waste_line.str());

    if (!comparison.mixed.fully_proven())
        // A comparison over unverified motion is still a comparison, but it
        // must not read as a verified one.
        comparison.reasons.push_back("note: " +
                                     std::to_string(comparison.mixed.unproven_operations +
                                                    comparison.mixed.colliding_operations) +
                                     " operation(s) in the mixed-tool schedule are not proven "
                                     "collision-free");

    return comparison;
}

std::vector<std::string> explain_schedule(const std::vector<PreviewRecord> &records,
                                          const OptimizationResult         &optimization)
{
    std::vector<std::string> lines;

    if (!optimization.error.empty()) {
        lines.push_back("the schedule could not be optimized: " + optimization.error);
        return lines;
    }

    if (optimization.improved) {
        std::ostringstream summary;
        summary << "optimization reduced " << optimization.baseline.toolchange_count()
                << " toolchange(s) to " << optimization.optimized.toolchange_count();
        lines.push_back(summary.str());
    } else {
        lines.push_back("optimization found nothing better than the baseline schedule, which "
                        "is what is being printed");
    }

    for (const std::string &line : optimization.eliminated)
        lines.push_back("removed: " + line);
    for (const std::string &line : optimization.introduced)
        lines.push_back("introduced: " + line);
    for (const std::string &line : optimization.notes)
        lines.push_back("note: " + line);

    // Each surviving change explains itself, in the order it happens.
    std::vector<size_t> order(records.size());
    for (size_t index = 0; index < records.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return records[a].chronological_index < records[b].chronological_index;
    });

    for (size_t index : order) {
        const PreviewRecord &record = records[index];
        if (!record.change_reason.has_value())
            continue;
        std::ostringstream line;
        line << "step " << record.chronological_index << ": changed to T" << record.tool << " ("
             << toolchange_necessity_name(*record.change_reason) << ")";
        if (!record.change_explanation.empty())
            line << " - " << record.change_explanation;
        lines.push_back(line.str());
    }

    return lines;
}

} // namespace Slic3r
