///|/ TigerSlicer Phase 22: advanced bridge planner.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BridgePlanner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *bridge_role_name(BridgeRole role)
{
    switch (role) {
    case BridgeRole::Structural: return "structural";
    case BridgeRole::Surface:    return "surface";
    case BridgeRole::Unknown:    return "unclassified";
    }
    return "unclassified";
}

const char *bridge_verdict_name(BridgeVerdict verdict)
{
    switch (verdict) {
    case BridgeVerdict::Planned:      return "planned";
    case BridgeVerdict::NeedsSupport: return "needs support";
    case BridgeVerdict::Unproven:     return "unproven";
    case BridgeVerdict::Refused:      return "refused";
    }
    return "refused";
}

const char *anchor_state_name(AnchorState state)
{
    switch (state) {
    case AnchorState::Anchored:          return "anchored";
    case AnchorState::PartiallyAnchored: return "partially anchored";
    case AnchorState::Unanchored:        return "unanchored";
    case AnchorState::Unknown:           return "unknown";
    }
    return "unknown";
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

std::string tool_name(int tool) { return "T" + std::to_string(tool); }

// Severity only ever escalates, so a verdict is combined by taking the worse
// of the two rather than by whichever check happened to run last.
int severity(BridgeVerdict verdict)
{
    switch (verdict) {
    case BridgeVerdict::Planned:      return 0;
    case BridgeVerdict::NeedsSupport: return 1;
    case BridgeVerdict::Unproven:     return 2;
    case BridgeVerdict::Refused:      return 3;
    }
    return 3;
}

BridgeVerdict worse_of(BridgeVerdict left, BridgeVerdict right)
{
    return severity(left) >= severity(right) ? left : right;
}

// What the anchors alone allow, before any tool is considered.
AnchorState evaluate_anchors(const BridgeGeometry &bridge, const BridgePolicy &policy)
{
    if (bridge.anchors.size() < 2)
        return AnchorState::Unanchored;

    bool any_unmeasured = false;
    bool any_weak       = false;
    for (const BridgeAnchor &anchor : bridge.anchors) {
        if (!anchor.measured)
            any_unmeasured = true;
        if (!anchor.solid || anchor.width_mm < policy.anchor_min_width_mm - EPSILON)
            any_weak = true;
    }

    if (any_unmeasured && !policy.allow_unmeasured_anchors)
        return AnchorState::Unknown;
    if (any_weak)
        return AnchorState::PartiallyAnchored;
    return AnchorState::Anchored;
}

double bridge_flow(const BridgeGeometry &bridge, const BridgeTool &tool,
                   const BridgePolicy &policy)
{
    // Bridge extrusion is round, so the nozzle diameter sets the width and the
    // flow ratio scales it.
    const double ratio = tool.bridge_flow_ratio.value_or(1.);
    return tool.nozzle_mm * bridge.layer_height_mm * policy.bridge_speed_mm_s * ratio;
}

} // namespace

BridgePlan plan_bridges(const std::vector<BridgeGeometry> &bridges,
                        const std::vector<BridgeTool>     &tools,
                        const BridgePolicy                &policy)
{
    BridgePlan plan;

    if (bridges.empty()) {
        plan.error = "there are no bridges to plan";
        return plan;
    }
    if (tools.empty()) {
        plan.error = "no tools were described, so no bridge can be assigned to anything";
        return plan;
    }

    size_t considered = bridges.size();
    if (considered > policy.max_bridges) {
        considered = policy.max_bridges;
        plan.notes.push_back("this print has " + std::to_string(bridges.size()) +
                             " bridges and the plan stopped after " +
                             std::to_string(policy.max_bridges) +
                             "; the rest were not examined at all");
    }

    // What each already-decided bridge ended up on, so dependants can be kept
    // on the same tool when splitting is not allowed.
    std::map<size_t, int>           decided_tool;
    std::map<size_t, BridgeVerdict> decided_verdict;

    bool all_priced = true;

    for (size_t index = 0; index < considered; ++index) {
        const BridgeGeometry &bridge = bridges[index];

        BridgeDecision decision;
        decision.bridge  = bridge.id;
        decision.verdict = BridgeVerdict::Planned;
        decision.anchors = evaluate_anchors(bridge, policy);

        // Anchors first. A bridge with nothing to land on is not a planning
        // problem, it is a refusal.
        switch (decision.anchors) {
        case AnchorState::Unanchored:
            decision.verdict = BridgeVerdict::Refused;
            decision.reasons.push_back(
                "this bridge has " + std::to_string(bridge.anchors.size()) +
                " anchor(s); a bridge needs two ends to land on, so it is refused rather "
                "than attempted");
            break;
        case AnchorState::Unknown:
            decision.verdict = worse_of(decision.verdict, BridgeVerdict::Unproven);
            decision.reasons.push_back(
                "at least one anchor was never measured, so whether this bridge is "
                "supported could not be checked - that is not the same as it being fine");
            break;
        case AnchorState::PartiallyAnchored:
            decision.verdict = worse_of(decision.verdict, BridgeVerdict::NeedsSupport);
            decision.reasons.push_back(
                "an anchor is sparse or narrower than " +
                number(policy.anchor_min_width_mm, 1) +
                " mm, so this bridge wants support under it before it is printed");
            break;
        case AnchorState::Anchored:
            break;
        }

        // Synchronization: a bridge that lands on another bridge is only
        // printable if that one is actually being printed.
        decision.synchronized = true;
        std::optional<int> parent_tool;
        for (size_t dependency : bridge.depends_on) {
            const auto verdict = decided_verdict.find(dependency);
            if (verdict == decided_verdict.end()) {
                decision.synchronized = false;
                decision.verdict      = worse_of(decision.verdict, BridgeVerdict::Unproven);
                decision.reasons.push_back(
                    "this bridge lands on bridge " + std::to_string(dependency) +
                    ", which has not been planned before it; the order could not be "
                    "checked");
                continue;
            }
            if (verdict->second == BridgeVerdict::Refused) {
                decision.synchronized = false;
                decision.verdict      = worse_of(decision.verdict, BridgeVerdict::Refused);
                decision.reasons.push_back(
                    "this bridge lands on bridge " + std::to_string(dependency) +
                    ", which is itself refused, so there is nothing for it to land on");
            }
            const auto assigned = decided_tool.find(dependency);
            if (assigned != decided_tool.end())
                parent_tool = assigned->second;
        }

        if (decision.verdict == BridgeVerdict::Refused) {
            decided_verdict[bridge.id] = decision.verdict;
            all_priced                 = false;
            plan.decisions.push_back(decision);
            continue;
        }

        // Tool selection. Every rejection names the tool and the reason, so a
        // refusal says what is responsible for it.
        const BridgeTool *best          = nullptr;
        double            best_score    = -1e30;
        size_t            ties          = 0;
        const BridgeTool *best_unproven = nullptr;

        for (const BridgeTool &tool : tools) {
            if (bridge.required_tool.has_value() && tool.tool != *bridge.required_tool)
                continue;
            if (!policy.allow_split_tools && parent_tool.has_value() &&
                tool.tool != *parent_tool)
                continue;

            if (policy.require_cooling && !tool.has_part_cooling_fan) {
                decision.rejected_tools.push_back(
                    tool_name(tool.tool) + " has no part cooling fan, and an uncooled bridge "
                    "sags");
                continue;
            }
            if (!tool.pressure_can_be_established) {
                decision.rejected_tools.push_back(
                    tool_name(tool.tool) + " cannot be brought up to full pressure before the "
                    "bridge starts, and a bridge that begins under-pressured breaks");
                continue;
            }
            if (tool.min_layer_height_mm.has_value() &&
                bridge.layer_height_mm < *tool.min_layer_height_mm - EPSILON) {
                decision.rejected_tools.push_back(
                    tool_name(tool.tool) + " cannot print a layer as thin as " +
                    number(bridge.layer_height_mm, 2) + " mm");
                continue;
            }
            if (tool.max_layer_height_mm.has_value() &&
                bridge.layer_height_mm > *tool.max_layer_height_mm + EPSILON) {
                decision.rejected_tools.push_back(
                    tool_name(tool.tool) + " cannot print a layer as thick as " +
                    number(bridge.layer_height_mm, 2) + " mm");
                continue;
            }

            const double flow = bridge_flow(bridge, tool, policy);
            if (tool.max_flow_mm3_s.has_value() && flow > *tool.max_flow_mm3_s + EPSILON) {
                // The speed is not quietly lowered to fit: a slow bridge sags,
                // so this is a rejection rather than an adjustment.
                decision.rejected_tools.push_back(
                    tool_name(tool.tool) + " would need " + number(flow, 2) +
                    " mm3/s to bridge at " + number(policy.bridge_speed_mm_s, 0) +
                    " mm/s and can only deliver " + number(*tool.max_flow_mm3_s, 2));
                continue;
            }

            if (!tool.max_bridge_span_mm.has_value()) {
                // Never characterized. Usable, but it cannot be called proven.
                if (best_unproven == nullptr ||
                    tool.change_cost_s < best_unproven->change_cost_s)
                    best_unproven = &tool;
                continue;
            }
            if (bridge.span_mm > *tool.max_bridge_span_mm + EPSILON) {
                decision.rejected_tools.push_back(
                    tool_name(tool.tool) + " has been shown to bridge " +
                    number(*tool.max_bridge_span_mm, 1) + " mm and this span is " +
                    number(bridge.span_mm, 1) + " mm");
                continue;
            }

            // A visible bridge wants the finest nozzle available; a hidden
            // structural one wants the biggest margin over the span. Both
            // prefer not to pay for a toolchange.
            double score = 0.;
            if (bridge.visible || bridge.role == BridgeRole::Surface)
                score = -tool.nozzle_mm * 100.;
            else
                score = (*tool.max_bridge_span_mm - bridge.span_mm) * 0.5;
            if (!parent_tool.has_value() || tool.tool != *parent_tool)
                score -= tool.change_cost_s * 0.1;

            if (score > best_score + EPSILON) {
                best_score = score;
                best       = &tool;
                ties       = 0;
            } else if (std::abs(score - best_score) <= EPSILON) {
                ++ties;
            }
        }

        if (best == nullptr && best_unproven != nullptr) {
            best = best_unproven;
            decision.verdict = worse_of(decision.verdict, BridgeVerdict::Unproven);
            decision.reasons.push_back(
                tool_name(best->tool) + " has never had its maximum bridge span measured, "
                "so a " + number(bridge.span_mm, 1) +
                " mm span cannot be shown to be within it");
        }

        if (best == nullptr) {
            decision.verdict = BridgeVerdict::Refused;
            if (bridge.required_tool.has_value())
                decision.reasons.push_back(
                    "this bridge is pinned to " + tool_name(*bridge.required_tool) +
                    ", which cannot print it; no other tool was substituted for it");
            else if (!policy.allow_split_tools && parent_tool.has_value())
                decision.reasons.push_back(
                    "splitting tools is not allowed, so this bridge had to use " +
                    tool_name(*parent_tool) + " like the bridge it lands on, and that tool "
                    "cannot print it");
            else
                decision.reasons.push_back(
                    "no tool on this machine can print this bridge; every one of them was "
                    "rejected for the reason recorded against it");
            decided_verdict[bridge.id] = decision.verdict;
            all_priced                 = false;
            plan.decisions.push_back(decision);
            continue;
        }

        decision.tool       = best->tool;
        decision.flow_mm3_s = bridge_flow(bridge, *best, policy);

        if (ties > 0)
            decision.reasons.push_back(
                std::to_string(ties + 1) + " tools scored identically for this bridge; " +
                tool_name(best->tool) + " was used and the tie is reported rather than "
                "presented as a preference");

        if (parent_tool.has_value() && *parent_tool != best->tool) {
            decision.change_cost_s = best->change_cost_s;
            decision.reasons.push_back(
                "this bridge uses " + tool_name(best->tool) + " while the bridge it lands on "
                "uses " + tool_name(*parent_tool) + "; the dependency order leaves room for "
                "the toolchange, which costs " + number(best->change_cost_s, 1) + " s");
        }

        std::ostringstream chosen;
        chosen << tool_name(best->tool) << " prints this " << bridge_role_name(bridge.role)
               << " bridge: " << number(bridge.span_mm, 1) << " mm at "
               << number(bridge.direction_deg, 0) << " degrees, "
               << number(best->nozzle_mm, 2) << " mm nozzle carrying " << best->material
               << (bridge.visible ? ", on a surface the user will see" : ", hidden inside the part");
        decision.reasons.push_back(chosen.str());

        if (policy.bridge_speed_mm_s > EPSILON) {
            decision.estimated_time_s =
                bridge.span_mm / policy.bridge_speed_mm_s + decision.change_cost_s.value_or(0.);
        } else {
            all_priced = false;
            decision.reasons.push_back(
                "the bridge speed is zero, so this bridge could not be timed");
        }

        decided_tool[bridge.id]    = best->tool;
        decided_verdict[bridge.id] = decision.verdict;
        plan.decisions.push_back(decision);
    }

    if (all_priced) {
        double total = 0.;
        for (const BridgeDecision &decision : plan.decisions)
            total += decision.estimated_time_s.value_or(0.);
        plan.total_time_s = total;
    } else {
        plan.notes.push_back("at least one bridge could not be priced, so no total bridging "
                             "time is reported");
    }

    return plan;
}

} // namespace Slic3r
