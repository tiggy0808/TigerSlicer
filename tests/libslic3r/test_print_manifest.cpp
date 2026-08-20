///|/ TigerSlicer Phase 30: reproducible print manifest.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/PrintManifest.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

constexpr int64_t T0 = 1'700'000'000;

// A manifest with every required section filled in and nothing unattributed,
// so a fixture only says what it is taking away.
PrintManifest complete_manifest()
{
    PrintManifest manifest;
    manifest.tigerslicer_version = "2.9.4-tiger.30";
    manifest.tigerslicer_commit  = "2791bdfa";
    manifest.project_name        = "two towers";
    manifest.project_hash        = "proj-1234";

    manifest.models = { { "tower-a.stl", "mesh-aaaa" }, { "tower-b.stl", "mesh-bbbb" } };

    manifest.profiles = { { ProfileKind::Printer, "tiger-4t", 3 },
                          { ProfileKind::Print, "fine", 2 },
                          { ProfileKind::Filament, "PLA-black", 1 },
                          { ProfileKind::Tool, "T0", 1 },
                          { ProfileKind::Calibration, "T0-flow", 4 },
                          { ProfileKind::Transition, "PLA-PETG", 1 } };

    ToolMapping t0;
    t0.index       = 0;
    t0.material    = "PLA";
    t0.nozzle_mm   = 0.40;
    t0.offset_x_mm = 0.;
    t0.offset_y_mm = 0.;
    t0.offset_z_mm = 0.;
    t0.calibration = ProfileVersionKey { ProfileKind::Calibration, "T0-flow", 4 };

    ToolMapping t1     = t0;
    t1.index           = 1;
    t1.material        = "PETG";
    t1.nozzle_mm       = 0.25;
    t1.offset_x_mm     = 18.5;
    t1.calibration     = ProfileVersionKey { ProfileKind::Calibration, "T1-flow", 2 };
    manifest.tools     = { t0, t1 };

    manifest.calibration = { { "T0-flow", "0.98" }, { "T1-flow", "1.02" } };
    manifest.transition  = { { "PLA->PETG", "purge 90 mm3" } };
    manifest.grids       = { { "tower-a", "grid-aaaa" }, { "tower-b", "grid-bbbb" } };
    manifest.ownership   = { { "walls", 1, "painted" }, { "infill", 0, "explicit" } };
    manifest.scheduler_policy = { { "mode", "bounded-batch" }, { "max_z_lead_mm", "1.0" } };
    manifest.cost_model       = { { "toolchange_s", "25" } };
    manifest.thermal_model    = { { "heat_rate_c_per_s", "2.0" } };
    manifest.validation       = { { "descent", "proven" } };
    manifest.verifier         = { { "gcode-twin", "acceptable" } };
    manifest.estimates        = { { "time_s", "315" }, { "waste_mm3", "270" } };
    manifest.firmware_capabilities = { { "klipper", "0.12" }, { "cancel_object", "yes" } };
    manifest.macro_contracts       = { { "TOOLCHANGE", "selects a tool and purges", true },
                                       { "PARK", "moves to the park position", true } };
    return manifest;
}

const FieldMismatch *mismatch_for(const ManifestComparison &comparison, const std::string &field)
{
    for (const FieldMismatch &m : comparison.mismatches)
        if (m.field == field)
            return &m;
    return nullptr;
}

bool has_finding(const ManifestAudit &audit, const std::string &fragment)
{
    for (const std::string &finding : audit.findings)
        if (finding.find(fragment) != std::string::npos)
            return true;
    return false;
}

std::string join(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace

TEST_CASE("30: a complete manifest is reproducible and says so", "[print-manifest]")
{
    const ManifestAudit audit = audit_manifest(complete_manifest());
    CHECK(audit.verdict == ManifestVerdict::Reproducible);
    CHECK(audit.reproducible());
    CHECK(audit.missing.empty());
    CHECK(audit.findings.empty());
    CHECK(audit.digest.size() == 32);
    CHECK_THAT(audit.summary, Catch::Matchers::ContainsSubstring("nothing in it is unattributed"));
}

TEST_CASE("30: a manifest missing a required section does not describe a print",
          "[print-manifest]")
{
    CHECK(required_sections().size() == 17);

    SECTION("warnings and overrides are deliberately not required")
    {
        const std::vector<ManifestSection> &required = required_sections();
        CHECK(std::find(required.begin(), required.end(), ManifestSection::Warnings) ==
              required.end());
        CHECK(std::find(required.begin(), required.end(), ManifestSection::Overrides) ==
              required.end());
        // A manifest with neither is still reproducible: having none of either
        // is a real state.
        CHECK(audit_manifest(complete_manifest()).reproducible());
    }

    SECTION("an empty section is a missing section")
    {
        PrintManifest manifest = complete_manifest();
        manifest.grids.clear();
        manifest.thermal_model.clear();
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Incomplete);
        CHECK(audit.missing.size() == 2);
        CHECK_THAT(audit.summary, Catch::Matchers::ContainsSubstring("does not describe a print"));
    }

    SECTION("a build with a version but no commit is not a build")
    {
        PrintManifest manifest = complete_manifest();
        manifest.tigerslicer_commit.clear();
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Incomplete);
        CHECK(audit.missing == std::vector<ManifestSection> { ManifestSection::Build });
    }
}

TEST_CASE("30: things present but not dependable qualify the manifest", "[print-manifest]")
{
    SECTION("a profile named without a version")
    {
        PrintManifest manifest    = complete_manifest();
        manifest.profiles[1].revision = 0;
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Qualified);
        CHECK(has_finding(audit, "not which version of it was used"));
    }

    SECTION("an override nobody accepted")
    {
        PrintManifest manifest = complete_manifest();
        manifest.overrides.push_back(
            { "printed below the minimum extrusion temperature", "the material is uncharacterized",
              "", T0 });
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Qualified);
        CHECK(has_finding(audit, "it is an unexplained one"));
    }

    SECTION("an override somebody did accept is not a finding")
    {
        PrintManifest manifest = complete_manifest();
        manifest.overrides.push_back({ "printed with an unproven descent", "bench test only",
                                       "tiger", T0 });
        CHECK(audit_manifest(manifest).reproducible());
    }

    SECTION("a tool with no nozzle, no offset or no calibration")
    {
        PrintManifest manifest = complete_manifest();
        manifest.tools[0].nozzle_mm.reset();
        manifest.tools[1].offset_z_mm.reset();
        manifest.tools[1].calibration.reset();
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Qualified);
        CHECK(has_finding(audit, "tool 0 has no recorded nozzle diameter"));
        CHECK(has_finding(audit, "tool 1 has no fully recorded offset"));
        CHECK(has_finding(audit, "tool 1 was used with no calibration version recorded"));
    }

    SECTION("a macro used but never declared")
    {
        PrintManifest manifest = complete_manifest();
        manifest.macro_contracts.push_back({ "CUSTOM_START", "", false });
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Qualified);
        CHECK(has_finding(audit, "its behaviour was never declared"));
    }

    SECTION("ownership with no stated basis")
    {
        PrintManifest manifest       = complete_manifest();
        manifest.ownership[0].basis.clear();
        const ManifestAudit audit = audit_manifest(manifest);
        CHECK(audit.verdict == ManifestVerdict::Qualified);
        CHECK(has_finding(audit, "does not record how it was arrived at"));
    }
}

TEST_CASE("30: the manifest serializes the same way every time and reads back", "[print-manifest]")
{
    const PrintManifest manifest = complete_manifest();

    SECTION("serialization is stable")
    {
        CHECK(serialize_manifest(manifest) == serialize_manifest(manifest));
        CHECK(manifest_digest(manifest) == manifest_digest(manifest));
    }

    SECTION("the order the caller built it in does not change the bytes")
    {
        PrintManifest shuffled = manifest;
        std::reverse(shuffled.models.begin(), shuffled.models.end());
        std::reverse(shuffled.profiles.begin(), shuffled.profiles.end());
        std::reverse(shuffled.grids.begin(), shuffled.grids.end());
        std::reverse(shuffled.estimates.begin(), shuffled.estimates.end());
        std::reverse(shuffled.tools.begin(), shuffled.tools.end());
        CHECK(serialize_manifest(shuffled) == serialize_manifest(manifest));
        CHECK(manifest_digest(shuffled) == manifest_digest(manifest));
    }

    SECTION("changing anything at all changes the digest")
    {
        PrintManifest changed = manifest;
        changed.estimates[0].value = "316";
        CHECK(manifest_digest(changed) != manifest_digest(manifest));

        PrintManifest recommitted   = manifest;
        recommitted.tigerslicer_commit = "deadbeef";
        CHECK(manifest_digest(recommitted) != manifest_digest(manifest));
    }

    SECTION("it round-trips through its own text")
    {
        const ParseResult parsed = parse_manifest(serialize_manifest(manifest));
        REQUIRE(parsed.ok);
        CHECK(parsed.unrecognized_lines.empty());
        CHECK(serialize_manifest(parsed.manifest) == serialize_manifest(manifest));
        CHECK(manifest_digest(parsed.manifest) == manifest_digest(manifest));

        CHECK(parsed.manifest.tigerslicer_commit == "2791bdfa");
        REQUIRE(parsed.manifest.tools.size() == 2);
        CHECK(parsed.manifest.tools[1].nozzle_mm.value() == 0.25);
        REQUIRE(parsed.manifest.tools[1].calibration.has_value());
        CHECK(parsed.manifest.tools[1].calibration->revision == 2);
        CHECK(parsed.manifest.ownership.size() == 2);
    }

    SECTION("a value with a tab or a newline in it survives the round trip")
    {
        PrintManifest awkward = manifest;
        awkward.warnings.push_back("tool 1\tis uncalibrated\nand was used anyway");
        const ParseResult parsed = parse_manifest(serialize_manifest(awkward));
        REQUIRE(parsed.ok);
        REQUIRE(parsed.manifest.warnings.size() == 1);
        CHECK(parsed.manifest.warnings[0] == "tool 1\tis uncalibrated\nand was used anyway");
    }

    SECTION("an absent measurement reads back absent, not zero")
    {
        PrintManifest partial = manifest;
        partial.tools[0].nozzle_mm.reset();
        const ParseResult parsed = parse_manifest(serialize_manifest(partial));
        REQUIRE(parsed.ok);
        CHECK_FALSE(parsed.manifest.tools[0].nozzle_mm.has_value());
    }

    SECTION("warnings keep the order they were raised in")
    {
        PrintManifest noisy = manifest;
        noisy.warnings      = { "zeroth", "first", "second" };
        const ParseResult parsed = parse_manifest(serialize_manifest(noisy));
        REQUIRE(parsed.ok);
        CHECK(parsed.manifest.warnings == noisy.warnings);
    }
}

TEST_CASE("30: a manifest read with lines dropped is not the manifest that was written",
          "[print-manifest]")
{
    std::string text = serialize_manifest(complete_manifest());

    SECTION("an unknown section is reported, not skipped")
    {
        text += "nonsense\tkey\tvalue\n";
        const ParseResult parsed = parse_manifest(text);
        CHECK_FALSE(parsed.ok);
        CHECK(parsed.unrecognized_lines.size() == 1);
        CHECK_THAT(parsed.error, Catch::Matchers::ContainsSubstring("lines dropped"));
    }

    SECTION("a line with too few fields is reported")
    {
        text += "build\tonlyonefield\n";
        CHECK_FALSE(parse_manifest(text).ok);
    }

    SECTION("a profile reference that is not a version key is reported")
    {
        const ParseResult parsed = parse_manifest("profiles\tprint\tnot-a-key\n");
        CHECK_FALSE(parsed.ok);
        CHECK(parsed.unrecognized_lines == std::vector<size_t> { 1 });
    }

    SECTION("blank lines are not content and are not errors")
    {
        const ParseResult parsed = parse_manifest("\n" + text + "\n\n");
        CHECK(parsed.ok);
    }
}

TEST_CASE("30: the g-code carries a reference and says it is only a reference",
          "[print-manifest]")
{
    const PrintManifest manifest = complete_manifest();

    SECTION("the reference names itself as a reference, unconditionally")
    {
        const std::vector<std::string> lines = format_gcode_reference(manifest, true);
        bool                           noted = false;
        for (const std::string &line : lines)
            if (line.find("this is a reference to the manifest, not the manifest") !=
                std::string::npos)
                noted = true;
        CHECK(noted);
        // Every line is a comment, so a firmware that does not know about it
        // ignores it rather than choking.
        for (const std::string &line : lines)
            CHECK(line.front() == ';');
    }

    SECTION("it parses back out of a whole g-code file")
    {
        const std::string gcode = "; generated by TigerSlicer\nG28\n" +
                                  join(format_gcode_reference(manifest, true)) +
                                  "G1 X10 Y10 Z0.2 E1 F1200\n";
        const ReferenceResult result = parse_gcode_reference(gcode);
        REQUIRE(result.status == ReferenceParse::Found);
        REQUIRE(result.reference.has_value());
        CHECK(result.reference->version == "2.9.4-tiger.30");
        CHECK(result.reference->commit == "2791bdfa");
        CHECK(result.reference->digest == manifest_digest(manifest));
        CHECK(result.reference->profiles == 6);
        CHECK(result.reference->warnings == 0);
        CHECK(result.reference->full_manifest_saved);
    }

    SECTION("g-code with no reference cannot be traced back, and says that")
    {
        const ReferenceResult result = parse_gcode_reference("G28\nG1 X0 Y0\n");
        CHECK(result.status == ReferenceParse::Absent);
        CHECK_FALSE(result.reference.has_value());
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("nothing in it can be "
                                                                     "traced back"));
    }

    SECTION("a reference with no digest does not identify a manifest")
    {
        const ReferenceResult result =
            parse_gcode_reference("; TIGERSLICER_MANIFEST version=1\n"
                                  "; TIGERSLICER_MANIFEST commit=abc\n");
        CHECK(result.status == ReferenceParse::Malformed);
        CHECK_THAT(result.message, Catch::Matchers::ContainsSubstring("no digest field"));
    }

    SECTION("a malformed reference line is malformed, not ignored")
    {
        const ReferenceResult result =
            parse_gcode_reference("; TIGERSLICER_MANIFEST garbage-with-no-equals\n");
        CHECK(result.status == ReferenceParse::Malformed);
    }
}

TEST_CASE("30: the manifest is compared against the project it claims to describe",
          "[print-manifest]")
{
    const PrintManifest manifest = complete_manifest();

    SECTION("the same project agrees")
    {
        const ManifestComparison comparison =
            compare_to_project(manifest, "proj-1234", manifest.models, manifest.profiles);
        CHECK(comparison.verdict == ComparisonVerdict::Match);
        CHECK(comparison.mismatches.empty());
        CHECK(comparison.compared == 9);
    }

    SECTION("an edited model is a mismatch that names the model")
    {
        std::vector<ModelHash> models = manifest.models;
        models[1].hash                = "mesh-cccc";
        const ManifestComparison comparison =
            compare_to_project(manifest, "proj-1234", models, manifest.profiles);
        CHECK(comparison.verdict == ComparisonVerdict::Mismatch);
        const FieldMismatch *mismatch = mismatch_for(comparison, "model tower-b.stl");
        REQUIRE(mismatch != nullptr);
        CHECK(mismatch->manifest_says == "mesh-bbbb");
        CHECK(mismatch->other_says == "mesh-cccc");
    }

    SECTION("a model in one and not the other is a mismatch, not an omission")
    {
        std::vector<ModelHash> models = manifest.models;
        models.push_back({ "tower-c.stl", "mesh-cccc" });
        const ManifestComparison comparison =
            compare_to_project(manifest, "proj-1234", models, manifest.profiles);
        CHECK(comparison.verdict == ComparisonVerdict::Mismatch);
        REQUIRE(mismatch_for(comparison, "model tower-c.stl") != nullptr);
        CHECK(mismatch_for(comparison, "model tower-c.stl")->manifest_says == "<absent>");
    }

    SECTION("a project using a different revision of a profile is a mismatch")
    {
        std::vector<ProfileVersionKey> profiles = manifest.profiles;
        profiles[1].revision                    = 3;
        const ManifestComparison comparison =
            compare_to_project(manifest, "proj-1234", manifest.models, profiles);
        CHECK(comparison.verdict == ComparisonVerdict::Mismatch);
    }

    SECTION("a project hash nobody recorded cannot be compared and is not a match")
    {
        PrintManifest anonymous = manifest;
        anonymous.project_hash.clear();
        anonymous.models.clear();
        anonymous.profiles.clear();
        const ManifestComparison comparison = compare_to_project(anonymous, "", {}, {});
        CHECK(comparison.verdict == ComparisonVerdict::Unproven);
        CHECK_THAT(comparison.summary,
                   Catch::Matchers::ContainsSubstring("agreement has not been shown"));
    }
}

TEST_CASE("30: the manifest is compared against the g-code it claims to have produced",
          "[print-manifest]")
{
    const PrintManifest manifest = complete_manifest();

    SECTION("its own g-code agrees")
    {
        const std::string gcode = join(format_gcode_reference(manifest, true));
        const ManifestComparison comparison = compare_to_gcode(manifest, gcode);
        CHECK(comparison.verdict == ComparisonVerdict::Match);
        CHECK(comparison.unchecked.empty());
    }

    SECTION("g-code from a different manifest disagrees on the digest")
    {
        PrintManifest other = manifest;
        other.estimates[0].value = "999";
        const std::string        gcode = join(format_gcode_reference(other, true));
        const ManifestComparison comparison = compare_to_gcode(manifest, gcode);
        CHECK(comparison.verdict == ComparisonVerdict::Mismatch);
        REQUIRE(mismatch_for(comparison, "digest") != nullptr);
    }

    SECTION("without the full manifest saved, only so much could be checked")
    {
        const std::string        gcode = join(format_gcode_reference(manifest, false));
        const ManifestComparison comparison = compare_to_gcode(manifest, gcode);
        CHECK(comparison.verdict == ComparisonVerdict::Match);
        REQUIRE(comparison.unchecked.size() == 1);
        CHECK_THAT(comparison.unchecked[0],
                   Catch::Matchers::ContainsSubstring("only the digest and the counts"));
        CHECK_THAT(comparison.summary,
                   Catch::Matchers::ContainsSubstring("could not be checked"));
    }

    SECTION("g-code with no reference proves nothing either way")
    {
        const ManifestComparison comparison = compare_to_gcode(manifest, "G28\nG1 X0\n");
        CHECK(comparison.verdict == ComparisonVerdict::Unproven);
        CHECK(comparison.compared == 0);
        CHECK(comparison.mismatches.empty());
    }
}

TEST_CASE("30: telemetry is compared without inventing contradictions", "[print-manifest]")
{
    const PrintManifest manifest = complete_manifest();

    SECTION("what the printer reports and the manifest claims agree")
    {
        const std::map<std::string, std::string> telemetry = {
            { "version", "2.9.4-tiger.30" },
            { "commit", "2791bdfa" },
            { "tool.0.material", "PLA" },
            { "firmware.klipper", "0.12" },
        };
        const ManifestComparison comparison = compare_to_telemetry(manifest, telemetry);
        CHECK(comparison.verdict == ComparisonVerdict::Match);
        CHECK(comparison.compared == 4);
    }

    SECTION("a printer running a different build is a mismatch")
    {
        const ManifestComparison comparison =
            compare_to_telemetry(manifest, { { "commit", "0000000" } });
        CHECK(comparison.verdict == ComparisonVerdict::Mismatch);
        REQUIRE(mismatch_for(comparison, "commit") != nullptr);
        CHECK(mismatch_for(comparison, "commit")->manifest_says == "2791bdfa");
    }

    SECTION("a printer reporting something the manifest never claimed is not a contradiction")
    {
        const ManifestComparison comparison = compare_to_telemetry(
            manifest, { { "chamber_temperature_c", "42" }, { "commit", "2791bdfa" } });
        CHECK(comparison.verdict == ComparisonVerdict::Match);
        CHECK(comparison.compared == 1);
        REQUIRE(comparison.unchecked.size() == 1);
        CHECK_THAT(comparison.unchecked[0],
                   Catch::Matchers::ContainsSubstring("makes no claim about it"));
    }

    SECTION("telemetry with nothing in common proves nothing")
    {
        const ManifestComparison comparison =
            compare_to_telemetry(manifest, { { "bed_mesh", "ok" } });
        CHECK(comparison.verdict == ComparisonVerdict::Unproven);
        CHECK(comparison.compared == 0);
    }
}

TEST_CASE("30: a print is reproduced end to end from its manifest", "[print-manifest]")
{
    // The phase's gate: write a manifest, embed its reference in the g-code,
    // then recover the manifest from its own text and show that it still
    // agrees with the project, the g-code and what the machine reported.
    const PrintManifest original = complete_manifest();
    const std::string   gcode =
        "; generated by TigerSlicer\n" + join(format_gcode_reference(original, true)) +
        "G28\nM109 S210\nT0\nG1 X10 Y10 Z0.2 E1 F1200\n";

    const std::string saved  = serialize_manifest(original);
    const ParseResult reread = parse_manifest(saved);
    REQUIRE(reread.ok);

    const PrintManifest &recovered = reread.manifest;
    CHECK(manifest_digest(recovered) == manifest_digest(original));

    const ManifestAudit audit = audit_manifest(recovered);
    CHECK(audit.reproducible());

    CHECK(compare_to_project(recovered, original.project_hash, original.models,
                             original.profiles)
              .verdict == ComparisonVerdict::Match);
    CHECK(compare_to_gcode(recovered, gcode).verdict == ComparisonVerdict::Match);
    CHECK(compare_to_telemetry(recovered, { { "version", "2.9.4-tiger.30" },
                                            { "commit", "2791bdfa" },
                                            { "tool.1.material", "PETG" } })
              .verdict == ComparisonVerdict::Match);

    // And a print that differs anywhere is caught by the same three checks.
    PrintManifest tampered = original;
    tampered.tools[1].nozzle_mm = 0.30;
    CHECK(manifest_digest(tampered) != manifest_digest(original));
    CHECK(compare_to_gcode(tampered, gcode).verdict == ComparisonVerdict::Mismatch);
}
