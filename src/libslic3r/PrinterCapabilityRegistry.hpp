///|/ TigerSlicer Phase UV7 groundwork: the normalized printer capability
///|/ registry and multi-source reconciliation.
///|/
///|/ Reconciles TigerSlicer, MSlicer and UVtools machine/profile data without
///|/ treating any one source as infallible, ranks every field by evidence,
///|/ never silently overwrites a user value, and blocks export on an
///|/ unresolved conflict of a print-critical field. Testable libslic3r logic;
///|/ it links no UVtools code and contains no catalog data - the catalog
///|/ (UV7A) and its cell values need the audited external source.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterCapabilityRegistry_hpp_
#define slic3r_PrinterCapabilityRegistry_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Where a field's value came from. No source is authoritative on its own; the
// evidence level, not the source, decides which value wins.
enum class CapabilitySource : uint8_t
{
    User,        // the user set it; never silently overwritten
    TigerSlicer,
    MSlicer,
    UVtools,
    VendorSpec,
    Upstream,
};

const char *capability_source_name(CapabilitySource source);

// How well a field is known. This is the axis reconciliation ranks on: a value
// confirmed on hardware outranks one inferred from a format family, whatever
// source each came from.
enum class EvidenceLevel : uint8_t
{
    // Ordered weakest-first so a numeric compare ranks them.
    Unknown,
    InferredFromFormatFamily,
    InheritedFromUpstream,
    ConfirmedByVendorSpec,
    ConfirmedOnHardware,
};

const char *evidence_level_name(EvidenceLevel level);
// The stronger of two evidence levels.
EvidenceLevel stronger_evidence(EvidenceLevel a, EvidenceLevel b);

// One field's value from one source, with its evidence and when it was last
// verified. The date is supplied by the caller; nothing here reads a clock.
struct CapabilityClaim
{
    std::string      field;
    std::string      value;
    CapabilitySource source { CapabilitySource::TigerSlicer };
    EvidenceLevel    evidence { EvidenceLevel::Unknown };
    // Seconds since the epoch, 0 when never verified.
    int64_t verified_s { 0 };
};

// The resolved value of one field, and why. A conflict is not hidden: when two
// claims of equal, strongest evidence disagree, the field is unresolved and
// the disagreement is recorded.
struct ResolvedField
{
    std::string field;

    // Set when the field resolved to a single value.
    std::optional<std::string> value;
    CapabilitySource           source { CapabilitySource::TigerSlicer };
    EvidenceLevel              evidence { EvidenceLevel::Unknown };
    int64_t                    verified_s { 0 };

    // True when equally-strong claims disagree and no rule breaks the tie. The
    // conflicting values are listed so the UI can ask the user.
    bool                     conflicted { false };
    std::vector<std::string> conflicting_values;

    // True when a user value was kept over stronger evidence from another
    // source, so the caller can surface that the user's choice stands.
    bool user_value_preserved { false };

    bool resolved() const { return value.has_value() && ! conflicted; }
};

// Resolve one field from all claims about it.
//
// The strongest evidence wins. A user claim is never silently overwritten: it
// is kept even against stronger evidence, and the fact is flagged so the UI can
// show the user their value stands (and offer, not force, an update). Absent a
// user claim, equally-strong disagreeing claims are a conflict, reported rather
// than broken by source order.
ResolvedField reconcile_field(const std::vector<CapabilityClaim> &claims);

// The whole registry for one printer: every field, resolved.
struct CapabilityRegistry
{
    std::map<std::string, ResolvedField> fields;

    const ResolvedField *find(const std::string &field) const;
    std::vector<std::string> conflicted_fields() const;
};

CapabilityRegistry build_registry(const std::vector<CapabilityClaim> &claims);

// --- export conflict prevention (UV7.6) ------------------------------------

// The fields whose conflict must prevent export, because a wrong value prints
// the wrong thing or damages the machine.
const std::vector<std::string> &print_critical_fields();

enum class RegistryExportVerdict : uint8_t
{
    Allowed,
    // A print-critical field is conflicted or unknown.
    Blocked,
};

const char *registry_export_verdict_name(RegistryExportVerdict verdict);

struct RegistryExportGate
{
    RegistryExportVerdict verdict { RegistryExportVerdict::Blocked };
    // Print-critical fields that are conflicted.
    std::vector<std::string> conflicted;
    // Print-critical fields that are not known at all.
    std::vector<std::string> unknown;
    std::string message;

    bool may_export() const { return verdict == RegistryExportVerdict::Allowed; }
};

// Block export when any print-critical field conflicts across sources or is
// unknown. A field known only by weak evidence is allowed (it resolved); a
// field that did not resolve at all is not.
RegistryExportGate evaluate_registry_export(const CapabilityRegistry &registry);

// --- registry update (UV7.4) ------------------------------------------------

// The effect a proposed set of new claims would have, computed without
// applying anything. A field where the update would change a user value is
// listed separately: the update never overwrites it, it offers the change.
struct RegistryUpdate
{
    // Fields that would gain a value they did not have.
    std::vector<std::string> added;
    // Fields whose resolved value would change (non-user).
    std::vector<std::string> changed;
    // Fields where a stronger claim disagrees with the user's value; offered,
    // never applied.
    std::vector<std::string> user_overrides_offered;
    // Fields that would become conflicted.
    std::vector<std::string> newly_conflicted;
};

// Compare the registry a new set of claims would produce against the current
// one, and report the diff without mutating anything. The caller decides what
// to apply; a user value is never in `changed`, only ever in
// `user_overrides_offered`.
RegistryUpdate preview_registry_update(const CapabilityRegistry           &current,
                                       const std::vector<CapabilityClaim> &incoming);

} // namespace Slic3r

#endif // slic3r_PrinterCapabilityRegistry_hpp_
