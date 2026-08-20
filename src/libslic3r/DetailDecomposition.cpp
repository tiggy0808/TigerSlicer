///|/ TigerSlicer Phase 19: automatic detail decomposition.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "DetailDecomposition.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *region_outcome_name(RegionOutcome outcome)
{
    switch (outcome) {
    case RegionOutcome::Proposed:         return "proposed";
    case RegionOutcome::KeptExplicit:     return "kept explicit";
    case RegionOutcome::Unchanged:        return "unchanged";
    case RegionOutcome::Ambiguous:        return "ambiguous";
    case RegionOutcome::NoCapableTool:    return "no capable tool";
    case RegionOutcome::NeedsMeasurement: return "needs measurement";
    }
    return "needs measurement";
}

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

const DecompositionTool *find_tool(const std::vector<DecompositionTool> &tools, int tool)
{
    for (const DecompositionTool &candidate : tools)
        if (candidate.tool == tool)
            return &candidate;
    return nullptr;
}

// Does this region carry detail that justifies a finer nozzle? Curvature and
// text do; merely being visible does not, exactly as Phase 5 concluded.
bool carries_detail(const RegionGeometry &region, const DecompositionOptions &options)
{
    return region.carries_text || region.curvature >= options.detail_curvature;
}

} // namespace

DecompositionResult decompose_details(const std::vector<RegionGeometry>            &regions,
                                      const std::vector<DecompositionTool>         &tools,
                                      const std::vector<std::pair<size_t, size_t>> &adjacency,
                                      const DecompositionOptions                   &options)
{
    DecompositionResult result;

    if (regions.empty()) {
        result.error = "there are no regions to decompose";
        return result;
    }
    if (tools.empty()) {
        result.error = "no tools were described, so no region can be assigned";
        return result;
    }

    std::vector<std::optional<int>> owner(regions.size());

    for (size_t index = 0; index < regions.size(); ++index) {
        const RegionGeometry &region = regions[index];

        RegionProposal proposal;
        proposal.region       = region.id;
        proposal.current_tool = region.current_tool;

        // The user's own decision comes first and is not quietly improved on.
        if ((region.explicit_tool.has_value() || region.explicit_color.has_value()) &&
            !options.allow_color_remap) {
            proposal.outcome       = RegionOutcome::KeptExplicit;
            proposal.proposed_tool = region.explicit_tool.has_value() ? region.explicit_tool
                                                                      : region.current_tool;
            proposal.reasons.push_back(
                region.explicit_tool.has_value()
                    ? "this region is explicitly assigned to T" +
                          std::to_string(*region.explicit_tool)
                    : "this region is explicitly coloured " + *region.explicit_color);
            proposal.reasons.push_back("remapping is disabled, so the assignment stands");
            owner[index] = proposal.proposed_tool;
            result.proposals.push_back(proposal);
            continue;
        }

        if (!region.accessibility_known) {
            // An empty reachable list must not read as "every tool can get
            // there".
            proposal.outcome = RegionOutcome::NeedsMeasurement;
            proposal.reasons.push_back("which tools can reach this region was never worked "
                                       "out, so no tool can be proposed for it");
            owner[index] = region.current_tool;
            result.proposals.push_back(proposal);
            continue;
        }

        if (!region.min_feature_mm.has_value() && carries_detail(region, options)) {
            proposal.outcome = RegionOutcome::NeedsMeasurement;
            proposal.reasons.push_back("this region carries detail but its minimum feature "
                                       "size was never measured, so the nozzle it needs is "
                                       "unknown");
            owner[index] = region.current_tool;
            result.proposals.push_back(proposal);
            continue;
        }

        // Which tools could actually do it.
        std::vector<const DecompositionTool *> capable;
        std::vector<std::string>               rejected;
        for (int reachable : region.reachable_tools) {
            const DecompositionTool *tool = find_tool(tools, reachable);
            if (tool == nullptr) {
                rejected.push_back("T" + std::to_string(reachable) +
                                   " is listed as reachable but was not described");
                continue;
            }
            if (region.min_feature_mm.has_value() &&
                tool->min_resolvable_mm > *region.min_feature_mm + EPSILON) {
                rejected.push_back("T" + std::to_string(tool->tool) + " resolves " +
                                   number(tool->min_resolvable_mm, 2) +
                                   " mm, too coarse for a " +
                                   number(*region.min_feature_mm, 2) + " mm feature");
                continue;
            }
            if (region.required_accuracy_mm.has_value()) {
                if (!tool->accuracy_mm.has_value()) {
                    rejected.push_back("T" + std::to_string(tool->tool) +
                                       " has no measured accuracy, so it cannot be shown to "
                                       "meet a " + number(*region.required_accuracy_mm, 3) +
                                       " mm requirement");
                    continue;
                }
                if (*tool->accuracy_mm > *region.required_accuracy_mm + EPSILON) {
                    rejected.push_back("T" + std::to_string(tool->tool) + " holds " +
                                       number(*tool->accuracy_mm, 3) + " mm, short of the " +
                                       number(*region.required_accuracy_mm, 3) +
                                       " mm required");
                    continue;
                }
            }
            capable.push_back(tool);
        }

        if (capable.empty()) {
            proposal.outcome = RegionOutcome::NoCapableTool;
            proposal.reasons = rejected;
            if (proposal.reasons.empty())
                proposal.reasons.push_back("no tool on this machine can reach and produce this "
                                           "region");
            owner[index] = region.current_tool;
            result.proposals.push_back(proposal);
            continue;
        }

        // Detail takes the finest capable nozzle; everything else takes the
        // coarsest one that still satisfies the measured requirement, because
        // sending ordinary work to the smallest nozzle is slow without being
        // better.
        const bool detail = carries_detail(region, options);
        std::sort(capable.begin(), capable.end(),
                  [detail](const DecompositionTool *a, const DecompositionTool *b) {
                      if (std::abs(a->nozzle_diameter_mm - b->nozzle_diameter_mm) > EPSILON)
                          return detail ? a->nozzle_diameter_mm < b->nozzle_diameter_mm
                                        : a->nozzle_diameter_mm > b->nozzle_diameter_mm;
                      return a->tool < b->tool;
                  });

        const DecompositionTool *best = capable.front();
        for (const DecompositionTool *candidate : capable)
            if (candidate != best &&
                std::abs(candidate->nozzle_diameter_mm - best->nozzle_diameter_mm) <= EPSILON)
                proposal.tied_tools.push_back(candidate->tool);

        if (!proposal.tied_tools.empty()) {
            // A tie is reported, never broken by index and presented as a
            // decision.
            proposal.tied_tools.insert(proposal.tied_tools.begin(), best->tool);
            proposal.outcome = RegionOutcome::Ambiguous;
            std::ostringstream line;
            line << "T" << best->tool;
            for (size_t at = 1; at < proposal.tied_tools.size(); ++at)
                line << " and T" << proposal.tied_tools[at];
            proposal.reasons.push_back(line.str() +
                                       " are equally suitable here; choose one");
            owner[index] = region.current_tool;
            result.proposals.push_back(proposal);
            continue;
        }

        proposal.reasons.push_back(detail
                                       ? "this region carries detail, so it takes the finest "
                                         "capable nozzle, T" + std::to_string(best->tool)
                                       : "ordinary work takes the coarsest nozzle its "
                                         "requirements permit, T" + std::to_string(best->tool));

        if (region.current_tool.has_value() && *region.current_tool == best->tool) {
            proposal.outcome       = RegionOutcome::Unchanged;
            proposal.proposed_tool = best->tool;
            owner[index]           = best->tool;
            result.proposals.push_back(proposal);
            continue;
        }

        // A move must repay the toolchange it costs.
        if (region.current_tool.has_value()) {
            const DecompositionTool *current = find_tool(tools, *region.current_tool);
            if (current != nullptr && current->throughput_mm3_s > EPSILON &&
                best->throughput_mm3_s > EPSILON) {
                const double saving = region.volume_mm3 / current->throughput_mm3_s -
                                      region.volume_mm3 / best->throughput_mm3_s;
                const double net = saving - options.toolchange_cost_s;
                if (!detail && net < options.min_benefit_s) {
                    proposal.outcome = RegionOutcome::Unchanged;
                    proposal.proposed_tool = region.current_tool;
                    proposal.reasons.push_back(
                        "moving this region to T" + std::to_string(best->tool) + " would save " +
                        number(saving, 1) + " s but cost a " +
                        number(options.toolchange_cost_s, 1) +
                        " s toolchange, which does not repay itself");
                    owner[index] = region.current_tool;
                    result.proposals.push_back(proposal);
                    continue;
                }
            }
        }

        proposal.outcome       = RegionOutcome::Proposed;
        proposal.proposed_tool = best->tool;
        owner[index]           = best->tool;
        result.proposals.push_back(proposal);
    }

    // Boundaries between differently owned regions become requirements for the
    // ownership and synchronization engine, not decisions taken here.
    for (const std::pair<size_t, size_t> &pair : adjacency) {
        if (pair.first >= regions.size() || pair.second >= regions.size()) {
            result.notes.push_back("an adjacency names a region that does not exist and was "
                                   "ignored");
            continue;
        }
        const std::optional<int> &a = owner[pair.first];
        const std::optional<int> &b = owner[pair.second];
        if (!a.has_value() || !b.has_value()) {
            result.notes.push_back("the boundary between regions " +
                                   std::to_string(regions[pair.first].id) + " and " +
                                   std::to_string(regions[pair.second].id) +
                                   " cannot be checked because one side has no owner yet");
            continue;
        }
        if (*a == *b)
            continue;

        InterfaceRequirement requirement;
        requirement.region_a = regions[pair.first].id;
        requirement.region_b = regions[pair.second].id;
        requirement.tool_a   = *a;
        requirement.tool_b   = *b;
        requirement.print_z  = std::max(regions[pair.first].print_z,
                                        regions[pair.second].print_z);

        const DecompositionTool *tool_a = find_tool(tools, *a);
        const DecompositionTool *tool_b = find_tool(tools, *b);
        if (tool_a != nullptr && tool_b != nullptr) {
            // The boundary has to sit on a plane both grids reach.
            requirement.synchronization_height_mm =
                std::max(tool_a->grid_height_mm, tool_b->grid_height_mm);
            const double finer = std::min(tool_a->grid_height_mm, tool_b->grid_height_mm);
            if (finer > EPSILON) {
                const double ratio = *requirement.synchronization_height_mm / finer;
                if (std::abs(ratio - std::round(ratio)) > 1e-6)
                    requirement.reasons.push_back(
                        "the two grids do not divide evenly, so this boundary needs the "
                        "synchronization engine to resolve it");
            }
        }
        requirement.reasons.push_back("regions " + std::to_string(requirement.region_a) +
                                      " and " + std::to_string(requirement.region_b) +
                                      " are owned by different tools");
        result.interfaces.push_back(requirement);
    }

    return result;
}

} // namespace Slic3r
