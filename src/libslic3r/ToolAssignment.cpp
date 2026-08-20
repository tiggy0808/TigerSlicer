///|/ TigerSlicer Phase 5: automatic nozzle/tool assignment proposals.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ToolAssignment.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

const char *visibility_name(Visibility visibility)
{
    switch (visibility) {
    case Visibility::Visible: return "visible";
    case Visibility::Hidden:  return "hidden";
    case Visibility::Unknown: return "unknown";
    }
    return "unknown";
}

const char *assignment_outcome_name(AssignmentOutcome outcome)
{
    switch (outcome) {
    case AssignmentOutcome::Proposed:         return "proposed";
    case AssignmentOutcome::KeptManual:       return "kept manual";
    case AssignmentOutcome::Unchanged:        return "unchanged";
    case AssignmentOutcome::Ambiguous:        return "ambiguous";
    case AssignmentOutcome::NoCompatibleTool: return "no compatible tool";
    case AssignmentOutcome::NeedsMeasurement: return "needs measurement";
    }
    return "unknown";
}

namespace {

std::string tool_label(int tool)
{
    return "T" + std::to_string(tool);
}

std::string mm(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << value << " mm";
    return out.str();
}

// Some feature types settle visibility on their own, whatever was reported.
Visibility effective_visibility(const FeatureGeometry &geometry)
{
    switch (geometry.feature) {
    case MultiRateFeature::SparseInfill:
    case MultiRateFeature::Support:
        return Visibility::Hidden;
    case MultiRateFeature::TopSurface:
    case MultiRateFeature::SupportInterface:
        return Visibility::Visible;
    default:
        return geometry.visibility;
    }
}

// Only work that actually carries detail justifies the finest nozzle. Ordinary
// visible work does not: sending every wall to the smallest nozzle on the
// machine would be slow without being better, so visible work takes the
// coarsest nozzle its measured resolution requirement still permits.
bool wants_fine_resolution(const FeatureGeometry &geometry)
{
    return geometry.carries_text_or_detail;
}

// A resolution requirement is what keeps "coarsest that fits" honest. Without
// one, the coarsest nozzle could ruin a surface nobody measured.
bool has_resolution_requirement(const FeatureGeometry &geometry)
{
    return geometry.minimum_feature_width_mm.has_value() ||
           geometry.required_accuracy_mm.has_value();
}

struct Candidate
{
    const ToolCapability *tool { nullptr };
    std::string           rejection;   // empty when the tool can do the work
};

// Hard capability filter. Nothing here is a preference: a tool either can
// produce this work or it cannot.
std::string reject_reason(const ToolCapability &tool, const FeatureGeometry &geometry)
{
    if (!geometry.required_material.empty()) {
        if (tool.material.empty())
            return tool_label(tool.tool) + " has not reported its loaded material";
        if (tool.material != geometry.required_material)
            return tool_label(tool.tool) + " is loaded with " + tool.material + ", not " +
                   geometry.required_material;
    }

    if (geometry.layer_height_mm.has_value()) {
        const double height = *geometry.layer_height_mm;
        if (tool.min_layer_height_mm > 0. && height + EPSILON < tool.min_layer_height_mm)
            return tool_label(tool.tool) + " cannot print below " + mm(tool.min_layer_height_mm);
        if (tool.max_layer_height_mm > 0. && height > tool.max_layer_height_mm + EPSILON)
            return tool_label(tool.tool) + " cannot print above " + mm(tool.max_layer_height_mm);
    }

    // A nozzle wider than the thinnest thing being printed cannot resolve it.
    if (geometry.minimum_feature_width_mm.has_value() &&
        tool.nozzle_diameter_mm > *geometry.minimum_feature_width_mm + EPSILON)
        return tool_label(tool.tool) + "'s " + mm(tool.nozzle_diameter_mm) +
               " nozzle is wider than the " + mm(*geometry.minimum_feature_width_mm) +
               " detail it would have to resolve";

    if (geometry.required_accuracy_mm.has_value() &&
        tool.nozzle_diameter_mm > *geometry.required_accuracy_mm + EPSILON)
        return tool_label(tool.tool) + "'s " + mm(tool.nozzle_diameter_mm) +
               " nozzle is coarser than the required " + mm(*geometry.required_accuracy_mm) +
               " accuracy";

    return {};
}

// Measurements the decision genuinely depends on. Fine work chosen without
// knowing how fine it is would be a guess wearing a decision's clothes.
std::string missing_measurement(const FeatureGeometry &geometry,
                                const std::vector<ToolCapability> &tools)
{
    // Only matters when the tools actually differ in resolution; on a machine
    // with one nozzle size there is nothing the measurement would change.
    bool differing_nozzles = false;
    for (size_t index = 1; index < tools.size(); ++index)
        if (std::abs(tools[index].nozzle_diameter_mm - tools[0].nozzle_diameter_mm) > EPSILON)
            differing_nozzles = true;
    if (!differing_nozzles)
        return {};

    if (geometry.carries_text_or_detail && !geometry.minimum_feature_width_mm.has_value())
        return "the finest detail on this text/detail feature was never measured";

    // Choosing the coarsest nozzle that fits is only safe when something says
    // what has to fit. For anything that is not settled as hidden, that
    // requirement has to have been measured.
    const Visibility visibility = effective_visibility(geometry);
    if (visibility != Visibility::Hidden && !has_resolution_requirement(geometry))
        return visibility == Visibility::Visible
                   ? std::string("this surface is visible but neither its minimum feature width "
                                 "nor its required accuracy was measured")
                   : std::string("this feature's visibility is unknown and neither its minimum "
                                 "feature width nor its required accuracy was measured");
    return {};
}

struct Choice
{
    int              tool { -1 };
    std::vector<int> tied;
    std::string      reason;
};

// Pick by nozzle size in the requested direction, preferring the tool already
// assigned when two are equally suitable, because not changing tool is the
// cheapest outcome available.
Choice choose(const std::vector<const ToolCapability *> &usable, bool finest, int current_tool)
{
    Choice choice;
    if (usable.empty())
        return choice;

    const ToolCapability *best = usable.front();
    for (const ToolCapability *tool : usable) {
        const double difference = tool->nozzle_diameter_mm - best->nozzle_diameter_mm;
        if (finest ? difference < -EPSILON : difference > EPSILON)
            best = tool;
    }

    std::vector<const ToolCapability *> equal;
    for (const ToolCapability *tool : usable)
        if (std::abs(tool->nozzle_diameter_mm - best->nozzle_diameter_mm) <= EPSILON)
            equal.push_back(tool);

    if (equal.size() > 1 && !finest) {
        // Among equally coarse nozzles the one that can move the most material
        // finishes hidden bulk soonest.
        const ToolCapability *fastest = equal.front();
        for (const ToolCapability *tool : equal)
            if (tool->max_flow_mm3_s > fastest->max_flow_mm3_s + EPSILON)
                fastest = tool;
        std::vector<const ToolCapability *> by_flow;
        for (const ToolCapability *tool : equal)
            if (std::abs(tool->max_flow_mm3_s - fastest->max_flow_mm3_s) <= EPSILON)
                by_flow.push_back(tool);
        if (by_flow.size() < equal.size()) {
            equal = by_flow;
            choice.reason = "highest sustainable flow among equally coarse nozzles";
        }
    }

    if (equal.size() > 1) {
        // Staying on the assigned tool avoids a toolchange, which is a real
        // saving and a legitimate tie-break.
        for (const ToolCapability *tool : equal)
            if (tool->tool == current_tool) {
                choice.tool   = current_tool;
                choice.reason = "already assigned to " + tool_label(current_tool) +
                                ", which is as suitable as any alternative";
                return choice;
            }
        // A genuine tie is reported, never broken by tool index.
        for (const ToolCapability *tool : equal)
            choice.tied.push_back(tool->tool);
        std::sort(choice.tied.begin(), choice.tied.end());
        return choice;
    }

    choice.tool = equal.front()->tool;
    if (choice.reason.empty())
        choice.reason = (finest ? "finest" : "coarsest") + std::string(" nozzle able to do this work (") +
                        mm(equal.front()->nozzle_diameter_mm) + ")";
    return choice;
}

} // namespace

std::vector<ToolProposal> propose_tool_assignments(
    const std::vector<FeatureAssignmentState> &features,
    const std::vector<ToolCapability>         &tools,
    const AssignmentOptions                   &options)
{
    std::vector<ToolProposal> proposals;
    proposals.reserve(features.size());

    for (const FeatureAssignmentState &state : features) {
        const FeatureGeometry &geometry = state.geometry;

        ToolProposal proposal;
        proposal.feature       = geometry.id;
        proposal.current_tool  = state.current_tool;
        proposal.proposed_tool = state.current_tool;

        if (tools.empty()) {
            proposal.outcome = AssignmentOutcome::NoCompatibleTool;
            proposal.reasons.push_back("no tools were described");
            proposals.push_back(std::move(proposal));
            continue;
        }

        // Manual ownership is authoritative. Nothing below can override it
        // unless remapping was explicitly enabled.
        if (state.manually_assigned && !options.allow_remapping_manual) {
            proposal.outcome = AssignmentOutcome::KeptManual;
            proposal.reasons.push_back(tool_label(state.current_tool) +
                                       " was assigned manually and remapping is not enabled");
            proposals.push_back(std::move(proposal));
            continue;
        }

        const std::string missing = missing_measurement(geometry, tools);
        if (!missing.empty()) {
            proposal.outcome = AssignmentOutcome::NeedsMeasurement;
            proposal.reasons.push_back(missing);
            proposals.push_back(std::move(proposal));
            continue;
        }

        std::vector<const ToolCapability *> usable;
        for (const ToolCapability &tool : tools) {
            const std::string rejection = reject_reason(tool, geometry);
            if (rejection.empty())
                usable.push_back(&tool);
            else
                proposal.reasons.push_back("ruled out: " + rejection);
        }

        if (usable.empty()) {
            proposal.outcome = AssignmentOutcome::NoCompatibleTool;
            proposal.reasons.push_back("no tool on this machine can produce this work");
            proposals.push_back(std::move(proposal));
            continue;
        }

        const bool fine = wants_fine_resolution(geometry);
        const Choice choice = choose(usable, fine, state.current_tool);
        if (fine)
            proposal.reasons.push_back("treated as detail work: the finest nozzle that can "
                                       "resolve it");
        else
            proposal.reasons.push_back(
                std::string("treated as ") +
                (effective_visibility(geometry) == Visibility::Hidden ? "hidden bulk"
                                                                      : "ordinary visible work") +
                ": the coarsest nozzle its resolution requirement allows");

        if (choice.tool < 0) {
            proposal.outcome    = AssignmentOutcome::Ambiguous;
            proposal.tied_tools = choice.tied;
            std::ostringstream line;
            line << "equally suitable: ";
            for (size_t index = 0; index < choice.tied.size(); ++index)
                line << (index == 0 ? "" : ", ") << tool_label(choice.tied[index]);
            proposal.reasons.push_back(line.str());
            proposals.push_back(std::move(proposal));
            continue;
        }

        proposal.reasons.push_back(choice.reason);
        proposal.proposed_tool = choice.tool;
        proposal.outcome = choice.tool == state.current_tool ? AssignmentOutcome::Unchanged
                                                             : AssignmentOutcome::Proposed;
        if (state.manually_assigned && proposal.changes_assignment()) {
            // Enabling remapping permits the proposal to exist; it does not
            // permit it to be applied without being seen.
            proposal.requires_confirmation = true;
            proposal.reasons.push_back("this would replace a manual assignment of " +
                                       tool_label(state.current_tool));
        }
        proposals.push_back(std::move(proposal));
    }

    return proposals;
}

bool apply_tool_proposals(const std::vector<ToolProposal> &proposals,
                          const std::vector<bool>         &accepted,
                          std::vector<int>                *resolved_tools,
                          std::string                     *error)
{
    if (error != nullptr)
        error->clear();
    if (resolved_tools == nullptr) {
        if (error != nullptr)
            *error = "no output was supplied";
        return false;
    }
    resolved_tools->clear();

    if (!accepted.empty() && accepted.size() != proposals.size()) {
        if (error != nullptr)
            *error = "acceptance was supplied for " + std::to_string(accepted.size()) + " of " +
                     std::to_string(proposals.size()) + " proposals";
        return false;
    }

    resolved_tools->reserve(proposals.size());
    for (size_t index = 0; index < proposals.size(); ++index) {
        const ToolProposal &proposal = proposals[index];
        const bool take = !accepted.empty() && accepted[index];

        if (take && proposal.outcome != AssignmentOutcome::Proposed &&
            proposal.outcome != AssignmentOutcome::Unchanged) {
            // An ambiguous or unmeasurable proposal carries no decision.
            // Accepting one is a caller error, not something to resolve by
            // picking whichever tool happened to be listed first.
            if (error != nullptr)
                *error = "proposal for feature " + std::to_string(proposal.feature) +
                         " was accepted but its outcome is " +
                         assignment_outcome_name(proposal.outcome);
            resolved_tools->clear();
            return false;
        }

        resolved_tools->push_back(take ? proposal.proposed_tool : proposal.current_tool);
    }
    return true;
}

} // namespace Slic3r
