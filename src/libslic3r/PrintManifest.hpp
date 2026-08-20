///|/ TigerSlicer Phase 30: reproducible print manifest.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrintManifest_hpp_
#define slic3r_PrintManifest_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ProfileVersioning.hpp"

namespace Slic3r {

// Everything the manifest must carry for a print to be reproducible. A
// manifest missing any of these is incomplete and says which; it is never
// presented as a record of the print.
enum class ManifestSection : uint8_t
{
    Build,
    Project,
    Models,
    Profiles,
    ToolMap,
    Calibration,
    Transition,
    Grids,
    Ownership,
    SchedulerPolicy,
    CostModel,
    ThermalModel,
    Validation,
    Verifier,
    Estimates,
    FirmwareCapabilities,
    MacroContracts,
    Warnings,
    Overrides,
};

const char *manifest_section_name(ManifestSection section);

// The sections that must be present and non-empty. Warnings and overrides are
// deliberately not here: having none of either is a real state, and demanding
// one would push callers into writing something meaningless.
const std::vector<ManifestSection> &required_sections();

// A named value inside a section. Kept generic for the sections whose contents
// are model- and machine-specific, so the manifest records what was actually
// in force rather than only the fields somebody anticipated.
struct ManifestEntry
{
    std::string key;
    std::string value;
};

struct ModelHash
{
    std::string name;
    std::string hash;
};

struct ToolMapping
{
    int         index { 0 };
    std::string material;

    // Absent means it was never declared. A manifest that writes 0.4 because
    // nobody said would be a manifest that cannot be trusted at all.
    std::optional<double> nozzle_mm;
    std::optional<double> offset_x_mm, offset_y_mm, offset_z_mm;

    // The calibration version in force for this tool, if there was one.
    std::optional<ProfileVersionKey> calibration;
};

struct GridRecord
{
    std::string name;
    std::string digest;
};

struct OwnershipRecord
{
    std::string region;
    int         tool { 0 };
    // How the assignment was arrived at: painted, inferred, explicit.
    std::string basis;
};

struct MacroRecord
{
    std::string name;
    std::string description;
    // False when the macro was used but its behaviour was never declared. The
    // manifest records the gap rather than omitting the macro.
    bool declared { false };
};

// A rule the user was told about and chose to proceed past. Recorded with who
// accepted it: an override nobody is named for is not an accepted override,
// it is an unexplained one.
struct AcceptedOverride
{
    std::string what;
    std::string why;
    std::string accepted_by;
    int64_t     accepted_s { 0 };
};

struct PrintManifest
{
    // Build.
    std::string tigerslicer_version;
    std::string tigerslicer_commit;

    // Project.
    std::string project_name;
    std::string project_hash;

    std::vector<ModelHash> models;

    // Exact profile versions, from Phase 27. A profile named without a
    // revision does not describe the print and is reported as such.
    std::vector<ProfileVersionKey> profiles;

    std::vector<ToolMapping>     tools;
    std::vector<ManifestEntry>   calibration;
    std::vector<ManifestEntry>   transition;
    std::vector<GridRecord>      grids;
    std::vector<OwnershipRecord> ownership;
    std::vector<ManifestEntry>   scheduler_policy;
    std::vector<ManifestEntry>   cost_model;
    std::vector<ManifestEntry>   thermal_model;
    std::vector<ManifestEntry>   validation;
    std::vector<ManifestEntry>   verifier;
    std::vector<ManifestEntry>   estimates;
    std::vector<ManifestEntry>   firmware_capabilities;
    std::vector<MacroRecord>     macro_contracts;

    std::vector<std::string>      warnings;
    std::vector<AcceptedOverride> overrides;
};

// --- serialization ---------------------------------------------------------

// A stable, line-oriented, machine-readable form. Sections and keys are
// emitted in a fixed order and values are escaped, so the same manifest always
// serializes to the same bytes and can be digested.
std::string serialize_manifest(const PrintManifest &manifest);

struct ParseResult
{
    PrintManifest manifest;
    bool          ok { false };
    std::string   error;
    // Lines that were not understood. Reported rather than skipped: a manifest
    // read with lines dropped is not the manifest that was written.
    std::vector<size_t> unrecognized_lines;
};

ParseResult parse_manifest(const std::string &text);

// A content address over the serialized manifest, using Phase 29's key
// builder so the two agree on what "the same inputs" means.
std::string manifest_digest(const PrintManifest &manifest);

// --- audit -----------------------------------------------------------------

enum class ManifestVerdict : uint8_t
{
    // Every required section present, every profile exact, every override
    // attributed.
    Reproducible,
    // Complete enough to read, but something in it cannot be relied on.
    Qualified,
    // A required section is missing; this does not describe a print.
    Incomplete,
};

const char *manifest_verdict_name(ManifestVerdict verdict);

struct ManifestAudit
{
    ManifestVerdict verdict { ManifestVerdict::Incomplete };

    std::vector<ManifestSection> missing;
    // Things present but not dependable, each naming what is responsible.
    std::vector<std::string> findings;

    std::string digest;
    std::string summary;

    bool reproducible() const { return verdict == ManifestVerdict::Reproducible; }
};

ManifestAudit audit_manifest(const PrintManifest &manifest);

// --- the compact G-code reference -----------------------------------------

// What goes into the G-code itself: enough to find and check the manifest, not
// the manifest. It says so explicitly, so nothing reads it as the record.
struct ManifestReference
{
    std::string version;
    std::string commit;
    std::string digest;

    size_t profiles { 0 };
    size_t warnings { 0 };
    size_t overrides { 0 };

    // Whether the full manifest was written beside the G-code. False means the
    // reference is all there is, and a comparison against it can only go so
    // far.
    bool full_manifest_saved { false };
};

// Comment lines to embed. Always includes the line that says this is a
// reference and not the manifest.
std::vector<std::string> format_gcode_reference(const PrintManifest &manifest,
                                                bool                 full_manifest_saved);

enum class ReferenceParse : uint8_t
{
    Found,
    Absent,
    Malformed,
};

const char *reference_parse_name(ReferenceParse status);

struct ReferenceResult
{
    ReferenceParse                   status { ReferenceParse::Absent };
    std::string                      message;
    std::optional<ManifestReference> reference;
};

ReferenceResult parse_gcode_reference(const std::string &gcode);

// --- comparison ------------------------------------------------------------

enum class ComparisonVerdict : uint8_t
{
    Match,
    Mismatch,
    // Nothing that could be compared was present on both sides. Not a match.
    Unproven,
};

const char *comparison_verdict_name(ComparisonVerdict verdict);

struct FieldMismatch
{
    std::string field;
    std::string manifest_says;
    std::string other_says;
};

struct ManifestComparison
{
    ComparisonVerdict verdict { ComparisonVerdict::Unproven };
    std::string       against;

    size_t                     compared { 0 };
    std::vector<FieldMismatch> mismatches;
    // Field, and why it could not be checked.
    std::vector<std::string> unchecked;

    std::string summary;
};

// Compare against the project the manifest claims to describe. The project is
// given as its own hash, model hashes and the exact profile versions it
// references.
ManifestComparison compare_to_project(const PrintManifest                  &manifest,
                                      const std::string                    &project_hash,
                                      const std::vector<ModelHash>         &models,
                                      const std::vector<ProfileVersionKey> &profiles);

// Compare against the G-code the manifest claims to have produced, using the
// reference embedded in it.
ManifestComparison compare_to_gcode(const PrintManifest &manifest, const std::string &gcode);

// Compare against what a machine reported back. Telemetry keys that the
// manifest has no counterpart for are unchecked, not mismatches: a printer
// reporting something the slicer never claimed is not a contradiction.
ManifestComparison compare_to_telemetry(const PrintManifest                      &manifest,
                                        const std::map<std::string, std::string> &telemetry);

} // namespace Slic3r

#endif // slic3r_PrintManifest_hpp_
