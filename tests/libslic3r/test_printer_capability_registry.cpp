///|/ TigerSlicer Phase UV7 groundwork: the printer capability registry.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/PrinterCapabilityRegistry.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

CapabilityClaim claim(const std::string &field, const std::string &value,
                      CapabilitySource source, EvidenceLevel evidence, int64_t verified_s = 0)
{
    CapabilityClaim c;
    c.field      = field;
    c.value      = value;
    c.source     = source;
    c.evidence   = evidence;
    c.verified_s = verified_s;
    return c;
}

bool contains(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A registry with every print-critical field resolved, so a test only alters
// what it means to.
std::vector<CapabilityClaim> healthy_claims()
{
    std::vector<CapabilityClaim> claims;
    for (const std::string &field : print_critical_fields())
        claims.push_back(claim(field, "ok", CapabilitySource::VendorSpec,
                               EvidenceLevel::ConfirmedByVendorSpec));
    return claims;
}

} // namespace

TEST_CASE("UV7: the strongest evidence wins, whatever source it came from", "[capability-registry]")
{
    SECTION("hardware confirmation outranks a format-family inference")
    {
        const ResolvedField resolved = reconcile_field({
            claim("resolution_x", "3840", CapabilitySource::UVtools,
                  EvidenceLevel::InferredFromFormatFamily),
            claim("resolution_x", "4096", CapabilitySource::TigerSlicer,
                  EvidenceLevel::ConfirmedOnHardware),
        });
        REQUIRE(resolved.resolved());
        CHECK(resolved.value.value() == "4096");
        CHECK(resolved.evidence == EvidenceLevel::ConfirmedOnHardware);
        CHECK(resolved.source == CapabilitySource::TigerSlicer);
    }

    SECTION("no source is authoritative on its own: a weak MSlicer claim loses to a strong UVtools one")
    {
        const ResolvedField resolved = reconcile_field({
            claim("build_z_mm", "160", CapabilitySource::MSlicer, EvidenceLevel::Unknown),
            claim("build_z_mm", "250", CapabilitySource::UVtools,
                  EvidenceLevel::ConfirmedByVendorSpec),
        });
        CHECK(resolved.value.value() == "250");
    }
}

TEST_CASE("UV7: a user value is never silently overwritten", "[capability-registry]")
{
    SECTION("a user value stands even against stronger evidence, and says so")
    {
        const ResolvedField resolved = reconcile_field({
            claim("orientation", "portrait", CapabilitySource::User, EvidenceLevel::Unknown),
            claim("orientation", "landscape", CapabilitySource::VendorSpec,
                  EvidenceLevel::ConfirmedByVendorSpec),
        });
        CHECK(resolved.value.value() == "portrait");
        CHECK(resolved.source == CapabilitySource::User);
        CHECK(resolved.user_value_preserved);
    }

    SECTION("a user value agreeing with the evidence is not flagged as an override")
    {
        const ResolvedField resolved = reconcile_field({
            claim("orientation", "landscape", CapabilitySource::User, EvidenceLevel::Unknown),
            claim("orientation", "landscape", CapabilitySource::VendorSpec,
                  EvidenceLevel::ConfirmedByVendorSpec),
        });
        CHECK(resolved.value.value() == "landscape");
        CHECK_FALSE(resolved.user_value_preserved);
    }
}

TEST_CASE("UV7: equally-strong disagreement is a conflict, not broken by source order",
          "[capability-registry]")
{
    const ResolvedField resolved = reconcile_field({
        claim("build_x_mm", "218", CapabilitySource::UVtools,
              EvidenceLevel::ConfirmedByVendorSpec),
        claim("build_x_mm", "219", CapabilitySource::Upstream,
              EvidenceLevel::ConfirmedByVendorSpec),
    });
    CHECK_FALSE(resolved.resolved());
    CHECK(resolved.conflicted);
    CHECK(resolved.conflicting_values == std::vector<std::string> { "218", "219" });

    // A weaker third claim does not break the tie between the two strong ones.
    const ResolvedField with_weak = reconcile_field({
        claim("build_x_mm", "218", CapabilitySource::UVtools,
              EvidenceLevel::ConfirmedByVendorSpec),
        claim("build_x_mm", "219", CapabilitySource::Upstream,
              EvidenceLevel::ConfirmedByVendorSpec),
        claim("build_x_mm", "218", CapabilitySource::MSlicer, EvidenceLevel::Unknown),
    });
    CHECK(with_weak.conflicted);
}

TEST_CASE("UV7: the registry resolves every field and lists its conflicts",
          "[capability-registry]")
{
    std::vector<CapabilityClaim> claims = {
        claim("resolution_x", "3840", CapabilitySource::VendorSpec,
              EvidenceLevel::ConfirmedByVendorSpec),
        claim("build_x_mm", "218", CapabilitySource::UVtools,
              EvidenceLevel::ConfirmedByVendorSpec),
        claim("build_x_mm", "219", CapabilitySource::Upstream,
              EvidenceLevel::ConfirmedByVendorSpec),
    };
    const CapabilityRegistry registry = build_registry(claims);
    REQUIRE(registry.find("resolution_x") != nullptr);
    CHECK(registry.find("resolution_x")->resolved());
    CHECK(registry.conflicted_fields() == std::vector<std::string> { "build_x_mm" });
}

TEST_CASE("UV7: export is blocked when a print-critical field conflicts or is unknown",
          "[capability-registry]")
{
    SECTION("a fully resolved registry allows export")
    {
        const CapabilityRegistry registry = build_registry(healthy_claims());
        const RegistryExportGate gate = evaluate_registry_export(registry);
        CHECK(gate.may_export());
    }

    SECTION("a conflicted print-critical field blocks export and names it")
    {
        std::vector<CapabilityClaim> claims = healthy_claims();
        claims.push_back(claim("build_z_mm", "300", CapabilitySource::Upstream,
                               EvidenceLevel::ConfirmedByVendorSpec));
        // Now build_z_mm has "ok" and "300" at equal evidence: a conflict.
        const CapabilityRegistry registry = build_registry(claims);
        const RegistryExportGate gate = evaluate_registry_export(registry);
        CHECK_FALSE(gate.may_export());
        CHECK(contains(gate.conflicted, "build_z_mm"));
    }

    SECTION("a missing print-critical field blocks export")
    {
        std::vector<CapabilityClaim> claims = healthy_claims();
        claims.erase(std::remove_if(claims.begin(), claims.end(),
                                    [](const CapabilityClaim &c) {
                                        return c.field == "motion_semantics";
                                    }),
                     claims.end());
        const CapabilityRegistry registry = build_registry(claims);
        const RegistryExportGate gate = evaluate_registry_export(registry);
        CHECK_FALSE(gate.may_export());
        CHECK(contains(gate.unknown, "motion_semantics"));
    }

    SECTION("a field known only by weak evidence still resolves and allows export")
    {
        std::vector<CapabilityClaim> claims;
        for (const std::string &field : print_critical_fields())
            claims.push_back(claim(field, "ok", CapabilitySource::UVtools,
                                   EvidenceLevel::InferredFromFormatFamily));
        const CapabilityRegistry registry = build_registry(claims);
        // Weak, but resolved and unconflicted: not the gate's job to demand
        // strong evidence, only to catch conflict and absence.
        CHECK(evaluate_registry_export(registry).may_export());
    }
}

TEST_CASE("UV7: a registry update is previewed without applying anything", "[capability-registry]")
{
    const CapabilityRegistry current = build_registry({
        claim("resolution_x", "3840", CapabilitySource::Upstream,
              EvidenceLevel::InheritedFromUpstream),
        claim("orientation", "portrait", CapabilitySource::User, EvidenceLevel::Unknown),
    });

    SECTION("a stronger incoming claim would change a non-user field")
    {
        const RegistryUpdate update = preview_registry_update(current, {
            claim("resolution_x", "4096", CapabilitySource::VendorSpec,
                  EvidenceLevel::ConfirmedByVendorSpec),
        });
        CHECK(contains(update.changed, "resolution_x"));
        // The current registry is untouched: preview does not mutate.
        CHECK(current.find("resolution_x")->value.value() == "3840");
    }

    SECTION("a stronger claim disagreeing with a user value is offered, not applied")
    {
        const RegistryUpdate update = preview_registry_update(current, {
            claim("orientation", "landscape", CapabilitySource::VendorSpec,
                  EvidenceLevel::ConfirmedByVendorSpec),
        });
        CHECK(contains(update.user_overrides_offered, "orientation"));
        CHECK_FALSE(contains(update.changed, "orientation"));
    }

    SECTION("a claim for a field not yet present is an addition")
    {
        const RegistryUpdate update = preview_registry_update(current, {
            claim("build_z_mm", "160", CapabilitySource::VendorSpec,
                  EvidenceLevel::ConfirmedByVendorSpec),
        });
        CHECK(contains(update.added, "build_z_mm"));
    }

    SECTION("a conflicting incoming claim makes the field newly conflicted")
    {
        const RegistryUpdate update = preview_registry_update(current, {
            claim("resolution_x", "5000", CapabilitySource::VendorSpec,
                  EvidenceLevel::InheritedFromUpstream),
        });
        // Equal evidence to the current inherited value, different value.
        CHECK(contains(update.newly_conflicted, "resolution_x"));
    }
}
