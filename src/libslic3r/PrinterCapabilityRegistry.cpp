///|/ TigerSlicer Phase UV7 groundwork: the normalized printer capability
///|/ registry and multi-source reconciliation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterCapabilityRegistry.hpp"

#include <algorithm>
#include <set>

namespace Slic3r {

const char *capability_source_name(CapabilitySource source)
{
    switch (source) {
    case CapabilitySource::User:        return "user";
    case CapabilitySource::TigerSlicer: return "tigerslicer";
    case CapabilitySource::MSlicer:     return "mslicer";
    case CapabilitySource::UVtools:     return "uvtools";
    case CapabilitySource::VendorSpec:  return "vendor spec";
    case CapabilitySource::Upstream:    return "upstream";
    }
    return "tigerslicer";
}

const char *evidence_level_name(EvidenceLevel level)
{
    switch (level) {
    case EvidenceLevel::Unknown:                  return "unknown";
    case EvidenceLevel::InferredFromFormatFamily: return "inferred from format family";
    case EvidenceLevel::InheritedFromUpstream:    return "inherited from upstream";
    case EvidenceLevel::ConfirmedByVendorSpec:    return "confirmed by vendor spec";
    case EvidenceLevel::ConfirmedOnHardware:      return "confirmed on hardware";
    }
    return "unknown";
}

EvidenceLevel stronger_evidence(EvidenceLevel a, EvidenceLevel b)
{
    return uint8_t(a) >= uint8_t(b) ? a : b;
}

const char *registry_export_verdict_name(RegistryExportVerdict verdict)
{
    switch (verdict) {
    case RegistryExportVerdict::Allowed: return "allowed";
    case RegistryExportVerdict::Blocked: return "blocked";
    }
    return "blocked";
}

// --- reconcile one field ----------------------------------------------------

ResolvedField reconcile_field(const std::vector<CapabilityClaim> &claims)
{
    ResolvedField resolved;
    if (claims.empty())
        return resolved;
    resolved.field = claims.front().field;

    // A user claim is never silently overwritten. If one exists, it stands -
    // and if stronger evidence disagrees, that is flagged so the UI can offer
    // (not force) an update.
    const CapabilityClaim *user = nullptr;
    for (const CapabilityClaim &claim : claims)
        if (claim.source == CapabilitySource::User) {
            // If more than one user claim exists, the later-verified one wins;
            // ties keep the first, deterministically.
            if (user == nullptr || claim.verified_s > user->verified_s)
                user = &claim;
        }

    if (user != nullptr) {
        resolved.value      = user->value;
        resolved.source     = CapabilitySource::User;
        resolved.evidence   = user->evidence;
        resolved.verified_s = user->verified_s;
        for (const CapabilityClaim &claim : claims) {
            if (claim.source == CapabilitySource::User)
                continue;
            if (claim.value != user->value &&
                uint8_t(claim.evidence) > uint8_t(user->evidence)) {
                resolved.user_value_preserved = true;
                break;
            }
        }
        return resolved;
    }

    // No user claim: the strongest evidence wins. Among equally strong claims,
    // agreement resolves; disagreement is a conflict, not broken by order.
    EvidenceLevel best = EvidenceLevel::Unknown;
    for (const CapabilityClaim &claim : claims)
        best = stronger_evidence(best, claim.evidence);

    std::set<std::string> top_values;
    const CapabilityClaim *chosen = nullptr;
    for (const CapabilityClaim &claim : claims)
        if (claim.evidence == best) {
            top_values.insert(claim.value);
            // Keep the most recently verified among the top tier for the
            // provenance fields, deterministically.
            if (chosen == nullptr || claim.verified_s > chosen->verified_s)
                chosen = &claim;
        }

    if (top_values.size() > 1) {
        resolved.conflicted         = true;
        resolved.evidence           = best;
        resolved.conflicting_values.assign(top_values.begin(), top_values.end());
        std::sort(resolved.conflicting_values.begin(), resolved.conflicting_values.end());
        return resolved;
    }

    resolved.value      = chosen->value;
    resolved.source     = chosen->source;
    resolved.evidence   = chosen->evidence;
    resolved.verified_s = chosen->verified_s;
    return resolved;
}

// --- registry ---------------------------------------------------------------

CapabilityRegistry build_registry(const std::vector<CapabilityClaim> &claims)
{
    std::map<std::string, std::vector<CapabilityClaim>> by_field;
    for (const CapabilityClaim &claim : claims)
        by_field[claim.field].push_back(claim);

    CapabilityRegistry registry;
    for (const auto &entry : by_field)
        registry.fields[entry.first] = reconcile_field(entry.second);
    return registry;
}

const ResolvedField *CapabilityRegistry::find(const std::string &field) const
{
    auto it = fields.find(field);
    return it == fields.end() ? nullptr : &it->second;
}

std::vector<std::string> CapabilityRegistry::conflicted_fields() const
{
    std::vector<std::string> out;
    for (const auto &entry : fields)
        if (entry.second.conflicted)
            out.push_back(entry.first);
    return out;
}

// --- export gate ------------------------------------------------------------

const std::vector<std::string> &print_critical_fields()
{
    // A wrong value in any of these prints the wrong thing or risks the
    // machine, so a conflict or a gap in them must stop export.
    static const std::vector<std::string> fields = {
        "resolution_x",  "resolution_y", "build_x_mm",     "build_y_mm",
        "build_z_mm",    "orientation",  "format_revision", "motion_semantics",
    };
    return fields;
}

RegistryExportGate evaluate_registry_export(const CapabilityRegistry &registry)
{
    RegistryExportGate gate;

    for (const std::string &field : print_critical_fields()) {
        const ResolvedField *resolved = registry.find(field);
        if (resolved == nullptr || (! resolved->value.has_value() && ! resolved->conflicted)) {
            gate.unknown.push_back(field);
        } else if (resolved->conflicted) {
            gate.conflicted.push_back(field);
        }
    }

    if (gate.conflicted.empty() && gate.unknown.empty()) {
        gate.verdict = RegistryExportVerdict::Allowed;
        gate.message = "every print-critical field resolved; export is allowed";
    } else {
        gate.verdict = RegistryExportVerdict::Blocked;
        gate.message = std::to_string(gate.conflicted.size()) + " conflicted and " +
                       std::to_string(gate.unknown.size()) +
                       " unknown print-critical field(s) block export";
    }
    return gate;
}

// --- update preview ---------------------------------------------------------

RegistryUpdate preview_registry_update(const CapabilityRegistry           &current,
                                       const std::vector<CapabilityClaim> &incoming)
{
    // Reconstruct the claim set the current registry rests on is not available
    // here, so the preview is computed by merging: for each field the incoming
    // claims touch, reconcile the incoming claims together with a synthetic
    // claim representing the current resolved value, and compare.
    std::map<std::string, std::vector<CapabilityClaim>> by_field;
    for (const CapabilityClaim &claim : incoming)
        by_field[claim.field].push_back(claim);

    RegistryUpdate update;

    for (auto &entry : by_field) {
        const std::string   &field   = entry.first;
        const ResolvedField *existing = current.find(field);

        std::vector<CapabilityClaim> merged = entry.second;
        if (existing != nullptr && existing->value.has_value()) {
            CapabilityClaim current_claim;
            current_claim.field      = field;
            current_claim.value      = *existing->value;
            current_claim.source     = existing->source;
            current_claim.evidence   = existing->evidence;
            current_claim.verified_s = existing->verified_s;
            merged.push_back(current_claim);
        }

        const ResolvedField after = reconcile_field(merged);

        if (existing == nullptr || ! existing->value.has_value()) {
            if (after.value.has_value())
                update.added.push_back(field);
            if (after.conflicted)
                update.newly_conflicted.push_back(field);
            continue;
        }

        if (after.conflicted && ! existing->conflicted) {
            update.newly_conflicted.push_back(field);
            continue;
        }

        // A user value that stronger incoming evidence disagrees with is
        // offered, never applied.
        if (existing->source == CapabilitySource::User && after.user_value_preserved)
            update.user_overrides_offered.push_back(field);
        else if (after.value.has_value() && after.value != existing->value)
            update.changed.push_back(field);
    }

    std::sort(update.added.begin(), update.added.end());
    std::sort(update.changed.begin(), update.changed.end());
    std::sort(update.user_overrides_offered.begin(), update.user_overrides_offered.end());
    std::sort(update.newly_conflicted.begin(), update.newly_conflicted.end());
    return update;
}

} // namespace Slic3r
