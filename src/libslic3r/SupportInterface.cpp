///|/ TigerSlicer Phase 9S: multi-material support interface engine, supplementing
///|/ Phase 9's mixed-nozzle support planning.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SupportInterface.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *support_strategy_name(SupportStrategy strategy)
{
    switch (strategy) {
    case SupportStrategy::Soluble:      return "soluble";
    case SupportStrategy::Breakaway:    return "breakaway";
    case SupportStrategy::SameMaterial: return "same material";
    case SupportStrategy::None:         return "none";
    }
    return "none";
}

const char *adhesion_class_name(AdhesionClass adhesion)
{
    switch (adhesion) {
    case AdhesionClass::TooWeak:   return "too weak";
    case AdhesionClass::Removable: return "removable";
    case AdhesionClass::Bonded:    return "bonded";
    case AdhesionClass::Unknown:   return "unknown";
    }
    return "unknown";
}

const char *removal_outcome_name(RemovalOutcome outcome)
{
    switch (outcome) {
    case RemovalOutcome::Dissolves:     return "dissolves";
    case RemovalOutcome::PeelsAway:     return "peels away";
    case RemovalOutcome::RequiresForce: return "requires force";
    case RemovalOutcome::NotRemovable:  return "not removable";
    case RemovalOutcome::Unknown:       return "unknown";
    }
    return "unknown";
}

const char *interface_verdict_name(InterfaceVerdict verdict)
{
    switch (verdict) {
    case InterfaceVerdict::Planned:    return "planned";
    case InterfaceVerdict::Compromise: return "compromise";
    case InterfaceVerdict::Unproven:   return "unproven";
    case InterfaceVerdict::Refused:    return "refused";
    }
    return "refused";
}

const MaterialPairing *find_pairing(const std::vector<MaterialPairing> &pairings,
                                    const std::string &a, const std::string &b)
{
    for (const MaterialPairing &pairing : pairings)
        if ((pairing.a == a && pairing.b == b) || (pairing.a == b && pairing.b == a))
            return &pairing;
    return nullptr;
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

int severity(InterfaceVerdict verdict)
{
    switch (verdict) {
    case InterfaceVerdict::Planned:    return 0;
    case InterfaceVerdict::Compromise: return 1;
    case InterfaceVerdict::Unproven:   return 2;
    case InterfaceVerdict::Refused:    return 3;
    }
    return 3;
}

InterfaceVerdict worse_of(InterfaceVerdict left, InterfaceVerdict right)
{
    return severity(left) >= severity(right) ? left : right;
}

AdhesionClass classify(const std::optional<double> &adhesion, const SupportPolicy &policy)
{
    if (!adhesion.has_value())
        return AdhesionClass::Unknown;
    if (*adhesion < policy.min_adhesion_to_hold - EPSILON)
        return AdhesionClass::TooWeak;
    if (*adhesion > policy.max_adhesion_to_remove + EPSILON)
        return AdhesionClass::Bonded;
    return AdhesionClass::Removable;
}

const SupportTool *tool_by_index(const std::vector<SupportTool> &tools, int index)
{
    for (const SupportTool &tool : tools)
        if (tool.tool == index)
            return &tool;
    return nullptr;
}

} // namespace

SupportInterfacePlan plan_support_interfaces(const std::vector<SupportedRegion> &regions,
                                             const std::vector<SupportTool>     &tools,
                                             const std::vector<MaterialPairing> &pairings,
                                             const SupportPolicy                &policy)
{
    SupportInterfacePlan plan;

    if (regions.empty()) {
        plan.error = "no supported regions were described, so there are no interfaces to plan";
        return plan;
    }
    if (tools.empty()) {
        plan.error = "no tools were described, so no support material can be chosen";
        return plan;
    }

    size_t considered = regions.size();
    if (considered > policy.max_regions) {
        considered = policy.max_regions;
        plan.notes.push_back("this print has " + std::to_string(regions.size()) +
                             " supported regions and the plan stopped after " +
                             std::to_string(policy.max_regions) +
                             "; the rest were not examined at all");
    }

    bool all_planned = true;

    for (size_t index = 0; index < considered; ++index) {
        const SupportedRegion &region = regions[index];

        InterfaceDecision decision;
        decision.region  = region.id;
        decision.verdict = InterfaceVerdict::Planned;

        // The cheapest loaded soluble tool, if there is one at all.
        const SupportTool *soluble = nullptr;
        for (const SupportTool &tool : tools) {
            if (!tool.soluble)
                continue;
            if (!tool.loaded) {
                decision.rejected_tools.push_back(tool_name(tool.tool) + " carries soluble " +
                                                  tool.material + " but is not loaded");
                continue;
            }
            if (soluble == nullptr || tool.change_cost_s < soluble->change_cost_s)
                soluble = &tool;
        }

        // A support sealed inside a cavity can only ever come out by
        // dissolving. This is decided before anything is printed.
        if (region.enclosed && soluble == nullptr) {
            if (!policy.allow_trapped_support) {
                decision.verdict = InterfaceVerdict::Refused;
                decision.removal = RemovalOutcome::NotRemovable;
                decision.reasons.push_back(
                    "this support sits inside a closed cavity and no soluble material is "
                    "loaded; nothing could reach it to break it out, so it would be printed "
                    "and then trapped in the part forever");
                all_planned = false;
                plan.decisions.push_back(decision);
                continue;
            }
            decision.verdict = worse_of(decision.verdict, InterfaceVerdict::Compromise);
            decision.reasons.push_back(
                "trapped support was explicitly permitted; this support will be sealed "
                "inside the part and cannot be removed");
        }

        // Strategy resolution. The preferred strategy is tried first, and any
        // fall-back is reported as a compromise rather than presented as the
        // thing that was asked for.
        SupportStrategy    strategy = policy.preferred;
        const SupportTool *chosen   = nullptr;

        if (region.enclosed)
            strategy = SupportStrategy::Soluble;

        if (strategy == SupportStrategy::None) {
            decision.reasons.push_back("no support was asked for in this region");
            plan.decisions.push_back(decision);
            continue;
        }

        if (strategy == SupportStrategy::Soluble) {
            if (soluble != nullptr) {
                chosen = soluble;
            } else {
                decision.verdict = worse_of(decision.verdict, InterfaceVerdict::Compromise);
                if (!region.enclosed)
                    decision.reasons.push_back(
                        "a soluble interface was asked for but no soluble material is "
                        "loaded, so a mechanical support is planned instead");
                strategy = SupportStrategy::Breakaway;
            }
        }

        if (strategy == SupportStrategy::Breakaway && chosen == nullptr) {
            const SupportTool *best        = nullptr;
            const SupportTool *unmeasured  = nullptr;
            for (const SupportTool &tool : tools) {
                if (tool.material == region.part_material)
                    continue; // that is the same-material strategy, not this one
                if (!tool.loaded) {
                    decision.rejected_tools.push_back(tool_name(tool.tool) +
                                                      " is not loaded");
                    continue;
                }
                const MaterialPairing *pairing =
                    find_pairing(pairings, region.part_material, tool.material);
                const std::optional<double> adhesion =
                    pairing == nullptr ? std::nullopt : pairing->adhesion;
                const AdhesionClass klass = classify(adhesion, policy);

                switch (klass) {
                case AdhesionClass::Unknown:
                    if (policy.allow_unmeasured_pairs) {
                        if (unmeasured == nullptr ||
                            tool.change_cost_s < unmeasured->change_cost_s)
                            unmeasured = &tool;
                    } else {
                        decision.rejected_tools.push_back(
                            "the adhesion between " + region.part_material + " and " +
                            tool.material + " on " + tool_name(tool.tool) +
                            " has never been measured, so it is unknown rather than "
                            "assumed compatible");
                    }
                    continue;
                case AdhesionClass::TooWeak:
                    decision.rejected_tools.push_back(
                        tool_name(tool.tool) + " carries " + tool.material +
                        ", whose adhesion to " + region.part_material + " is " +
                        number(*adhesion, 2) + "; the support would come loose while it "
                        "printed");
                    continue;
                case AdhesionClass::Bonded:
                    decision.rejected_tools.push_back(
                        tool_name(tool.tool) + " carries " + tool.material +
                        ", whose adhesion to " + region.part_material + " is " +
                        number(*adhesion, 2) + "; it would weld to the part and damage it "
                        "on removal");
                    continue;
                case AdhesionClass::Removable:
                    break;
                }

                if (best == nullptr || tool.change_cost_s < best->change_cost_s) {
                    best                    = &tool;
                    decision.adhesion_value = adhesion;
                }
            }

            if (best != nullptr) {
                chosen = best;
            } else if (unmeasured != nullptr) {
                chosen           = unmeasured;
                decision.verdict = worse_of(decision.verdict, InterfaceVerdict::Unproven);
                decision.reasons.push_back(
                    "the adhesion between " + region.part_material + " and " +
                    unmeasured->material + " has never been measured; this interface was "
                    "planned because unmeasured pairs were explicitly permitted, and it is "
                    "not known whether the support will hold or come off");
            } else {
                decision.verdict = worse_of(decision.verdict, InterfaceVerdict::Compromise);
                decision.reasons.push_back(
                    "no loaded material breaks away cleanly from " + region.part_material +
                    ", so the support is printed in the part's own material");
                strategy = SupportStrategy::SameMaterial;
            }
        }

        if (strategy == SupportStrategy::SameMaterial && chosen == nullptr) {
            const SupportTool *part = tool_by_index(tools, region.part_tool);
            if (part == nullptr || !part->loaded) {
                decision.verdict = InterfaceVerdict::Refused;
                decision.reasons.push_back(
                    "the support would fall back to the part's own tool " +
                    tool_name(region.part_tool) +
                    ", which is not available, so no support can be planned at all");
                all_planned = false;
                plan.decisions.push_back(decision);
                continue;
            }
            chosen = part;
        }

        if (chosen == nullptr) {
            decision.verdict = InterfaceVerdict::Refused;
            decision.reasons.push_back(
                "no interface material could be chosen for this region");
            all_planned = false;
            plan.decisions.push_back(decision);
            continue;
        }

        decision.strategy           = strategy;
        decision.interface_tool     = chosen->tool;
        decision.interface_material = chosen->material;

        // Contact distance and how the support comes off follow from the
        // strategy, not from a default nobody chose.
        switch (strategy) {
        case SupportStrategy::Soluble:
            decision.contact_distance_mm = policy.soluble_contact_distance_mm;
            decision.adhesion            = AdhesionClass::Removable;
            decision.removal             = RemovalOutcome::Dissolves;
            decision.reasons.push_back(
                tool_name(chosen->tool) + " prints a soluble " + chosen->material +
                " interface touching the part" +
                (chosen->solvent.empty() ? "; no solvent is recorded for it"
                                         : ", removed in " + chosen->solvent));
            if (chosen->solvent.empty())
                decision.verdict = worse_of(decision.verdict, InterfaceVerdict::Unproven);
            break;
        case SupportStrategy::Breakaway:
            decision.contact_distance_mm = policy.interface_contact_distance_mm;
            decision.adhesion            = decision.adhesion_value.has_value()
                                               ? AdhesionClass::Removable
                                               : AdhesionClass::Unknown;
            decision.removal             = decision.adhesion_value.has_value()
                                               ? RemovalOutcome::PeelsAway
                                               : RemovalOutcome::Unknown;
            decision.reasons.push_back(
                tool_name(chosen->tool) + " prints a breakaway " + chosen->material +
                " interface " + number(policy.interface_contact_distance_mm, 2) +
                " mm off the part");
            break;
        case SupportStrategy::SameMaterial:
            decision.contact_distance_mm = policy.interface_contact_distance_mm;
            decision.adhesion            = AdhesionClass::Bonded;
            decision.removal             = RemovalOutcome::RequiresForce;
            decision.reasons.push_back(
                "a material welds to itself, so this interface relies entirely on the " +
                number(policy.interface_contact_distance_mm, 2) +
                " mm gap; the support will take force to remove and will mark the surface");
            break;
        case SupportStrategy::None:
            break;
        }

        if (region.enclosed && strategy != SupportStrategy::Soluble)
            decision.removal = RemovalOutcome::NotRemovable;

        // Interface layers.
        decision.interface_layers = policy.min_interface_layers;
        if (region.overhang_angle_deg < 20. - EPSILON) {
            ++decision.interface_layers;
            decision.reasons.push_back(
                "this overhang is within " + number(region.overhang_angle_deg, 0) +
                " degrees of horizontal, so its whole weight rests on the interface and one "
                "extra interface layer is used");
        }

        if (region.visible_after_removal && strategy != SupportStrategy::Soluble)
            decision.reasons.push_back(
                "this contact surface is on show after the support comes off, and a "
                "mechanical support will leave a mark on it");

        if (chosen->tool != region.part_tool) {
            decision.change_cost_s = chosen->change_cost_s;
            decision.reasons.push_back(
                "the interface is on " + tool_name(chosen->tool) + " while the part is on " +
                tool_name(region.part_tool) + ", which costs " +
                number(chosen->change_cost_s, 1) + " s per change");
        }

        plan.decisions.push_back(decision);
    }

    if (all_planned) {
        double total = 0.;
        for (const InterfaceDecision &decision : plan.decisions)
            total += decision.change_cost_s.value_or(0.);
        plan.total_change_cost_s = total;
    } else {
        plan.notes.push_back("at least one region could not be planned, so no total "
                             "toolchange cost is reported");
    }

    return plan;
}

} // namespace Slic3r
