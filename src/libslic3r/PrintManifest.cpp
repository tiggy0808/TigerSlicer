///|/ TigerSlicer Phase 30: reproducible print manifest.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrintManifest.hpp"

#include "SliceCache.hpp"

#include <algorithm>
#include <tuple>
#include <sstream>

namespace Slic3r {

const char *manifest_section_name(ManifestSection section)
{
    switch (section) {
    case ManifestSection::Build:                return "build";
    case ManifestSection::Project:              return "project";
    case ManifestSection::Models:               return "models";
    case ManifestSection::Profiles:             return "profiles";
    case ManifestSection::ToolMap:              return "toolmap";
    case ManifestSection::Calibration:          return "calibration";
    case ManifestSection::Transition:           return "transition";
    case ManifestSection::Grids:                return "grids";
    case ManifestSection::Ownership:            return "ownership";
    case ManifestSection::SchedulerPolicy:      return "scheduler";
    case ManifestSection::CostModel:            return "cost";
    case ManifestSection::ThermalModel:         return "thermal";
    case ManifestSection::Validation:           return "validation";
    case ManifestSection::Verifier:             return "verifier";
    case ManifestSection::Estimates:            return "estimates";
    case ManifestSection::FirmwareCapabilities: return "firmware";
    case ManifestSection::MacroContracts:       return "macros";
    case ManifestSection::Warnings:             return "warnings";
    case ManifestSection::Overrides:            return "overrides";
    }
    return "build";
}

const std::vector<ManifestSection> &required_sections()
{
    // Warnings and overrides are absent on purpose: having none of either is a
    // real state, and requiring them would push callers into writing something
    // meaningless to satisfy a check.
    static const std::vector<ManifestSection> sections = {
        ManifestSection::Build,           ManifestSection::Project,
        ManifestSection::Models,          ManifestSection::Profiles,
        ManifestSection::ToolMap,         ManifestSection::Calibration,
        ManifestSection::Transition,      ManifestSection::Grids,
        ManifestSection::Ownership,       ManifestSection::SchedulerPolicy,
        ManifestSection::CostModel,       ManifestSection::ThermalModel,
        ManifestSection::Validation,      ManifestSection::Verifier,
        ManifestSection::Estimates,       ManifestSection::FirmwareCapabilities,
        ManifestSection::MacroContracts,
    };
    return sections;
}

const char *manifest_verdict_name(ManifestVerdict verdict)
{
    switch (verdict) {
    case ManifestVerdict::Reproducible: return "reproducible";
    case ManifestVerdict::Qualified:    return "qualified";
    case ManifestVerdict::Incomplete:   return "incomplete";
    }
    return "incomplete";
}

const char *reference_parse_name(ReferenceParse status)
{
    switch (status) {
    case ReferenceParse::Found:     return "found";
    case ReferenceParse::Absent:    return "absent";
    case ReferenceParse::Malformed: return "malformed";
    }
    return "absent";
}

const char *comparison_verdict_name(ComparisonVerdict verdict)
{
    switch (verdict) {
    case ComparisonVerdict::Match:    return "match";
    case ComparisonVerdict::Mismatch: return "mismatch";
    case ComparisonVerdict::Unproven: return "unproven";
    }
    return "unproven";
}

// --- serialization ---------------------------------------------------------

namespace {

// Tabs and newlines separate fields, so a value containing one would make a
// line that reads as two. Escaped rather than stripped: a manifest that
// silently alters what it records is worse than one that is awkward to read.
std::string escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:   out += c; break;
        }
    }
    return out;
}

std::string unescape(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) {
            out += value[i];
            continue;
        }
        switch (value[++i]) {
        case 't':  out += '\t'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case '\\': out += '\\'; break;
        default:   out += value[i]; break;
        }
    }
    return out;
}

void emit(std::ostringstream &out, ManifestSection section, const std::string &key,
          const std::string &value)
{
    out << manifest_section_name(section) << '\t' << escape(key) << '\t' << escape(value)
        << '\n';
}

std::string optional_number(const std::optional<double> &value)
{
    if (! value.has_value())
        // A distinct token, so a reader can tell "nobody measured it" from any
        // number, including zero.
        return "-";
    std::ostringstream out;
    out.precision(10);
    out << *value;
    return out.str();
}

std::optional<double> parse_optional_number(const std::string &text)
{
    if (text == "-" || text.empty())
        return std::nullopt;
    try {
        return std::stod(text);
    } catch (...) {
        return std::nullopt;
    }
}

std::string profile_key_text(const ProfileVersionKey &key)
{
    return key.to_string();
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string              current;
    for (char c : text) {
        if (c == '\n') {
            if (! current.empty() && current.back() == '\r')
                current.pop_back();
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (! current.empty())
        lines.push_back(current);
    return lines;
}

// Sorted, so two manifests with the same content serialize identically
// whatever order the caller built them in.
std::vector<ManifestEntry> sorted(const std::vector<ManifestEntry> &entries)
{
    std::vector<ManifestEntry> out = entries;
    std::sort(out.begin(), out.end(), [](const ManifestEntry &a, const ManifestEntry &b) {
        if (a.key != b.key)
            return a.key < b.key;
        return a.value < b.value;
    });
    return out;
}

void emit_entries(std::ostringstream &out, ManifestSection section,
                  const std::vector<ManifestEntry> &entries)
{
    for (const ManifestEntry &entry : sorted(entries))
        emit(out, section, entry.key, entry.value);
}

} // namespace

std::string serialize_manifest(const PrintManifest &manifest)
{
    std::ostringstream out;

    emit(out, ManifestSection::Build, "version", manifest.tigerslicer_version);
    emit(out, ManifestSection::Build, "commit", manifest.tigerslicer_commit);

    emit(out, ManifestSection::Project, "name", manifest.project_name);
    emit(out, ManifestSection::Project, "hash", manifest.project_hash);

    {
        std::vector<ModelHash> models = manifest.models;
        std::sort(models.begin(), models.end(), [](const ModelHash &a, const ModelHash &b) {
            if (a.name != b.name)
                return a.name < b.name;
            return a.hash < b.hash;
        });
        for (const ModelHash &model : models)
            emit(out, ManifestSection::Models, model.name, model.hash);
    }

    {
        std::vector<ProfileVersionKey> profiles = manifest.profiles;
        std::sort(profiles.begin(), profiles.end());
        for (const ProfileVersionKey &profile : profiles)
            emit(out, ManifestSection::Profiles, profile_kind_name(profile.kind),
                 profile_key_text(profile));
    }

    {
        std::vector<ToolMapping> tools = manifest.tools;
        std::sort(tools.begin(), tools.end(),
                  [](const ToolMapping &a, const ToolMapping &b) { return a.index < b.index; });
        for (const ToolMapping &tool : tools) {
            const std::string id = std::to_string(tool.index);
            emit(out, ManifestSection::ToolMap, id + ".material", tool.material);
            emit(out, ManifestSection::ToolMap, id + ".nozzle_mm",
                 optional_number(tool.nozzle_mm));
            emit(out, ManifestSection::ToolMap, id + ".offset_x_mm",
                 optional_number(tool.offset_x_mm));
            emit(out, ManifestSection::ToolMap, id + ".offset_y_mm",
                 optional_number(tool.offset_y_mm));
            emit(out, ManifestSection::ToolMap, id + ".offset_z_mm",
                 optional_number(tool.offset_z_mm));
            emit(out, ManifestSection::ToolMap, id + ".calibration",
                 tool.calibration.has_value() ? profile_key_text(*tool.calibration)
                                              : std::string("-"));
        }
    }

    emit_entries(out, ManifestSection::Calibration, manifest.calibration);
    emit_entries(out, ManifestSection::Transition, manifest.transition);

    {
        std::vector<GridRecord> grids = manifest.grids;
        std::sort(grids.begin(), grids.end(), [](const GridRecord &a, const GridRecord &b) {
            if (a.name != b.name)
                return a.name < b.name;
            return a.digest < b.digest;
        });
        for (const GridRecord &grid : grids)
            emit(out, ManifestSection::Grids, grid.name, grid.digest);
    }

    {
        std::vector<OwnershipRecord> ownership = manifest.ownership;
        std::sort(ownership.begin(), ownership.end(),
                  [](const OwnershipRecord &a, const OwnershipRecord &b) {
                      // Total order over every emitted field, so two records that
                      // share region+tool but differ in basis have a defined,
                      // order-independent position and the digest stays stable.
                      return std::tie(a.region, a.tool, a.basis) <
                             std::tie(b.region, b.tool, b.basis);
                  });
        for (const OwnershipRecord &record : ownership)
            emit(out, ManifestSection::Ownership, record.region,
                 std::to_string(record.tool) + " (" +
                     (record.basis.empty() ? "unstated" : record.basis) + ")");
    }

    emit_entries(out, ManifestSection::SchedulerPolicy, manifest.scheduler_policy);
    emit_entries(out, ManifestSection::CostModel, manifest.cost_model);
    emit_entries(out, ManifestSection::ThermalModel, manifest.thermal_model);
    emit_entries(out, ManifestSection::Validation, manifest.validation);
    emit_entries(out, ManifestSection::Verifier, manifest.verifier);
    emit_entries(out, ManifestSection::Estimates, manifest.estimates);
    emit_entries(out, ManifestSection::FirmwareCapabilities, manifest.firmware_capabilities);

    {
        std::vector<MacroRecord> macros = manifest.macro_contracts;
        std::sort(macros.begin(), macros.end(),
                  [](const MacroRecord &a, const MacroRecord &b) { return a.name < b.name; });
        for (const MacroRecord &macro : macros)
            emit(out, ManifestSection::MacroContracts, macro.name,
                 (macro.declared ? std::string("declared: ") : std::string("undeclared: ")) +
                     (macro.description.empty() ? "no description" : macro.description));
    }

    {
        // Warnings keep the order they were raised in - that order is part of
        // what happened - but are indexed so the file stays line-oriented.
        size_t index = 0;
        for (const std::string &warning : manifest.warnings)
            emit(out, ManifestSection::Warnings, std::to_string(index++), warning);
    }

    {
        std::vector<AcceptedOverride> overrides = manifest.overrides;
        std::sort(overrides.begin(), overrides.end(),
                  [](const AcceptedOverride &a, const AcceptedOverride &b) {
                      // Total order over every emitted field: two overrides that
                      // share what+accepted_s but differ in accepted_by/why must
                      // still sort deterministically, or the same content built in
                      // a different order would serialize to different bytes.
                      return std::tie(a.what, a.accepted_s, a.accepted_by, a.why) <
                             std::tie(b.what, b.accepted_s, b.accepted_by, b.why);
                  });
        for (const AcceptedOverride &override_ : overrides)
            emit(out, ManifestSection::Overrides, override_.what,
                 (override_.accepted_by.empty() ? std::string("<nobody>")
                                                : override_.accepted_by) +
                     "\x01" + std::to_string(override_.accepted_s) + "\x01" + override_.why);
    }

    return out.str();
}

ParseResult parse_manifest(const std::string &text)
{
    ParseResult result;

    std::map<std::string, ManifestSection> by_name;
    for (uint8_t i = 0; i <= uint8_t(ManifestSection::Overrides); ++i) {
        const ManifestSection section = ManifestSection(i);
        by_name[manifest_section_name(section)] = section;
    }

    const std::vector<std::string> lines = split_lines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (line.empty())
            continue;

        const size_t first = line.find('\t');
        if (first == std::string::npos) {
            result.unrecognized_lines.push_back(i + 1);
            continue;
        }
        const size_t second = line.find('\t', first + 1);
        if (second == std::string::npos) {
            result.unrecognized_lines.push_back(i + 1);
            continue;
        }

        const std::string section_name = line.substr(0, first);
        auto              section      = by_name.find(section_name);
        if (section == by_name.end()) {
            result.unrecognized_lines.push_back(i + 1);
            continue;
        }

        const std::string key   = unescape(line.substr(first + 1, second - first - 1));
        const std::string value = unescape(line.substr(second + 1));

        PrintManifest &manifest = result.manifest;
        switch (section->second) {
        case ManifestSection::Build:
            if (key == "version")
                manifest.tigerslicer_version = value;
            else if (key == "commit")
                manifest.tigerslicer_commit = value;
            else
                result.unrecognized_lines.push_back(i + 1);
            break;
        case ManifestSection::Project:
            if (key == "name")
                manifest.project_name = value;
            else if (key == "hash")
                manifest.project_hash = value;
            else
                result.unrecognized_lines.push_back(i + 1);
            break;
        case ManifestSection::Models: manifest.models.push_back({ key, value }); break;
        case ManifestSection::Profiles: {
            // "kind:id@revision", as ProfileVersionKey::to_string writes it.
            const size_t colon = value.find(':');
            const size_t at    = value.rfind('@');
            if (colon == std::string::npos || at == std::string::npos || at < colon) {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            ProfileVersionKey profile;
            const std::string kind_name = value.substr(0, colon);
            bool              matched   = false;
            for (ProfileKind kind :
                 { ProfileKind::Printer, ProfileKind::Print, ProfileKind::Filament,
                   ProfileKind::Tool, ProfileKind::Calibration, ProfileKind::Transition })
                if (kind_name == profile_kind_name(kind)) {
                    profile.kind = kind;
                    matched      = true;
                }
            if (! matched) {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            profile.profile_id       = value.substr(colon + 1, at - colon - 1);
            const std::string number = value.substr(at + 1);
            profile.revision         = 0;
            try {
                profile.revision = std::stoi(number);
            } catch (...) {
                // Left at 0, which ProfileVersionKey::complete() reports as no
                // revision at all - exactly what the audit needs to see.
            }
            manifest.profiles.push_back(profile);
            break;
        }
        case ManifestSection::ToolMap: {
            const size_t dot = key.find('.');
            if (dot == std::string::npos) {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            int index = 0;
            try {
                index = std::stoi(key.substr(0, dot));
            } catch (...) {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            ToolMapping *tool = nullptr;
            for (ToolMapping &candidate : manifest.tools)
                if (candidate.index == index)
                    tool = &candidate;
            if (tool == nullptr) {
                ToolMapping fresh;
                fresh.index = index;
                manifest.tools.push_back(fresh);
                tool = &manifest.tools.back();
            }
            const std::string field = key.substr(dot + 1);
            if (field == "material")
                tool->material = value;
            else if (field == "nozzle_mm")
                tool->nozzle_mm = parse_optional_number(value);
            else if (field == "offset_x_mm")
                tool->offset_x_mm = parse_optional_number(value);
            else if (field == "offset_y_mm")
                tool->offset_y_mm = parse_optional_number(value);
            else if (field == "offset_z_mm")
                tool->offset_z_mm = parse_optional_number(value);
            else if (field == "calibration") {
                if (value != "-") {
                    const size_t colon = value.find(':');
                    const size_t at    = value.rfind('@');
                    if (colon != std::string::npos && at != std::string::npos && at > colon) {
                        ProfileVersionKey calibration;
                        calibration.kind       = ProfileKind::Calibration;
                        calibration.profile_id = value.substr(colon + 1, at - colon - 1);
                        try {
                            calibration.revision = std::stoi(value.substr(at + 1));
                        } catch (...) {
                            calibration.revision = 0;
                        }
                        tool->calibration = calibration;
                    } else {
                        result.unrecognized_lines.push_back(i + 1);
                    }
                }
            } else
                result.unrecognized_lines.push_back(i + 1);
            break;
        }
        case ManifestSection::Calibration:
            manifest.calibration.push_back({ key, value });
            break;
        case ManifestSection::Transition: manifest.transition.push_back({ key, value }); break;
        case ManifestSection::Grids: manifest.grids.push_back({ key, value }); break;
        case ManifestSection::Ownership: {
            OwnershipRecord record;
            record.region     = key;
            const size_t open = value.find(" (");
            try {
                record.tool = std::stoi(open == std::string::npos ? value
                                                                  : value.substr(0, open));
            } catch (...) {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            if (open != std::string::npos && value.size() > open + 2)
                record.basis = value.substr(open + 2, value.size() - open - 3);
            manifest.ownership.push_back(record);
            break;
        }
        case ManifestSection::SchedulerPolicy:
            manifest.scheduler_policy.push_back({ key, value });
            break;
        case ManifestSection::CostModel: manifest.cost_model.push_back({ key, value }); break;
        case ManifestSection::ThermalModel:
            manifest.thermal_model.push_back({ key, value });
            break;
        case ManifestSection::Validation: manifest.validation.push_back({ key, value }); break;
        case ManifestSection::Verifier: manifest.verifier.push_back({ key, value }); break;
        case ManifestSection::Estimates: manifest.estimates.push_back({ key, value }); break;
        case ManifestSection::FirmwareCapabilities:
            manifest.firmware_capabilities.push_back({ key, value });
            break;
        case ManifestSection::MacroContracts: {
            MacroRecord macro;
            macro.name                 = key;
            const std::string declared = "declared: ";
            const std::string undeclared = "undeclared: ";
            if (value.rfind(undeclared, 0) == 0) {
                macro.declared    = false;
                macro.description = value.substr(undeclared.size());
            } else if (value.rfind(declared, 0) == 0) {
                macro.declared    = true;
                macro.description = value.substr(declared.size());
            } else {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            if (macro.description == "no description")
                macro.description.clear();
            manifest.macro_contracts.push_back(macro);
            break;
        }
        case ManifestSection::Warnings: manifest.warnings.push_back(value); break;
        case ManifestSection::Overrides: {
            AcceptedOverride override_;
            override_.what      = key;
            const size_t first_sep  = value.find('\x01');
            const size_t second_sep = first_sep == std::string::npos
                                          ? std::string::npos
                                          : value.find('\x01', first_sep + 1);
            if (first_sep == std::string::npos || second_sep == std::string::npos) {
                result.unrecognized_lines.push_back(i + 1);
                break;
            }
            override_.accepted_by = value.substr(0, first_sep);
            if (override_.accepted_by == "<nobody>")
                override_.accepted_by.clear();
            try {
                override_.accepted_s =
                    std::stoll(value.substr(first_sep + 1, second_sep - first_sep - 1));
            } catch (...) {
                override_.accepted_s = 0;
            }
            override_.why = value.substr(second_sep + 1);
            manifest.overrides.push_back(override_);
            break;
        }
        }
    }

    if (! result.unrecognized_lines.empty()) {
        result.ok    = false;
        result.error = std::to_string(result.unrecognized_lines.size()) +
                       " line(s) were not understood; a manifest read with lines dropped is not "
                       "the manifest that was written";
        return result;
    }

    result.ok = true;
    return result;
}

std::string manifest_digest(const PrintManifest &manifest)
{
    // Phase 29's key builder, so the manifest and the cache agree on what "the
    // same inputs" means.
    CacheInput input;
    input.kind  = CacheInputKind::Policy;
    input.name  = "manifest";
    input.value = serialize_manifest(manifest);
    return build_partial_key({ input }).key.digest;
}

// --- audit -----------------------------------------------------------------

namespace {

bool section_present(const PrintManifest &manifest, ManifestSection section)
{
    switch (section) {
    case ManifestSection::Build:
        return ! manifest.tigerslicer_version.empty() && ! manifest.tigerslicer_commit.empty();
    case ManifestSection::Project:
        return ! manifest.project_hash.empty();
    case ManifestSection::Models:               return ! manifest.models.empty();
    case ManifestSection::Profiles:             return ! manifest.profiles.empty();
    case ManifestSection::ToolMap:              return ! manifest.tools.empty();
    case ManifestSection::Calibration:          return ! manifest.calibration.empty();
    case ManifestSection::Transition:           return ! manifest.transition.empty();
    case ManifestSection::Grids:                return ! manifest.grids.empty();
    case ManifestSection::Ownership:            return ! manifest.ownership.empty();
    case ManifestSection::SchedulerPolicy:      return ! manifest.scheduler_policy.empty();
    case ManifestSection::CostModel:            return ! manifest.cost_model.empty();
    case ManifestSection::ThermalModel:         return ! manifest.thermal_model.empty();
    case ManifestSection::Validation:           return ! manifest.validation.empty();
    case ManifestSection::Verifier:             return ! manifest.verifier.empty();
    case ManifestSection::Estimates:            return ! manifest.estimates.empty();
    case ManifestSection::FirmwareCapabilities: return ! manifest.firmware_capabilities.empty();
    case ManifestSection::MacroContracts:       return ! manifest.macro_contracts.empty();
    case ManifestSection::Warnings:             return ! manifest.warnings.empty();
    case ManifestSection::Overrides:            return ! manifest.overrides.empty();
    }
    return false;
}

} // namespace

ManifestAudit audit_manifest(const PrintManifest &manifest)
{
    ManifestAudit audit;
    audit.digest = manifest_digest(manifest);

    for (ManifestSection section : required_sections())
        if (! section_present(manifest, section))
            audit.missing.push_back(section);

    for (const ProfileVersionKey &profile : manifest.profiles)
        if (! profile.complete())
            audit.findings.push_back(
                std::string("the ") + profile_kind_name(profile.kind) + " profile is recorded as " +
                profile.to_string() +
                ", which names a profile but not which version of it was used");

    for (const AcceptedOverride &override_ : manifest.overrides)
        if (override_.accepted_by.empty())
            audit.findings.push_back(
                "the override \"" + override_.what +
                "\" has nobody recorded as having accepted it; an override nobody is named for "
                "is not an accepted override, it is an unexplained one");

    for (const ToolMapping &tool : manifest.tools) {
        const std::string id = "tool " + std::to_string(tool.index);
        if (! tool.nozzle_mm.has_value())
            audit.findings.push_back(id + " has no recorded nozzle diameter, so what it laid "
                                          "down cannot be reconstructed");
        if (! tool.offset_x_mm.has_value() || ! tool.offset_y_mm.has_value() ||
            ! tool.offset_z_mm.has_value())
            audit.findings.push_back(id +
                                     " has no fully recorded offset, so where it put material "
                                     "cannot be reconstructed");
        if (! tool.calibration.has_value())
            audit.findings.push_back(id + " was used with no calibration version recorded");
        else if (! tool.calibration->complete())
            audit.findings.push_back(id + " names a calibration without a version");
    }

    for (const MacroRecord &macro : manifest.macro_contracts)
        if (! macro.declared)
            audit.findings.push_back("the macro \"" + macro.name +
                                     "\" was used but its behaviour was never declared");

    for (const OwnershipRecord &record : manifest.ownership)
        if (record.basis.empty())
            audit.findings.push_back("the ownership of \"" + record.region +
                                     "\" does not record how it was arrived at");

    if (! audit.missing.empty()) {
        audit.verdict = ManifestVerdict::Incomplete;
        audit.summary = std::to_string(audit.missing.size()) +
                        " required section(s) are missing, so this does not describe a print";
    } else if (! audit.findings.empty()) {
        audit.verdict = ManifestVerdict::Qualified;
        audit.summary = "every required section is present, but " +
                        std::to_string(audit.findings.size()) +
                        " thing(s) in it cannot be relied on";
    } else {
        audit.verdict = ManifestVerdict::Reproducible;
        audit.summary = "every required section is present and nothing in it is unattributed";
    }

    return audit;
}

// --- the compact reference -------------------------------------------------

namespace {

const char *REFERENCE_PREFIX = "; TIGERSLICER_MANIFEST ";

} // namespace

std::vector<std::string> format_gcode_reference(const PrintManifest &manifest,
                                                bool                 full_manifest_saved)
{
    std::vector<std::string> lines;
    lines.push_back(std::string(REFERENCE_PREFIX) + "version=" + manifest.tigerslicer_version);
    lines.push_back(std::string(REFERENCE_PREFIX) + "commit=" + manifest.tigerslicer_commit);
    lines.push_back(std::string(REFERENCE_PREFIX) + "digest=" + manifest_digest(manifest));
    lines.push_back(std::string(REFERENCE_PREFIX) + "profiles=" +
                    std::to_string(manifest.profiles.size()));
    lines.push_back(std::string(REFERENCE_PREFIX) + "warnings=" +
                    std::to_string(manifest.warnings.size()));
    lines.push_back(std::string(REFERENCE_PREFIX) + "overrides=" +
                    std::to_string(manifest.overrides.size()));
    lines.push_back(std::string(REFERENCE_PREFIX) + "full=" +
                    (full_manifest_saved ? "saved" : "not-saved"));
    // Last, and unconditional. Anything that reads the file and stops at the
    // fields above would otherwise have no way of knowing this is a pointer
    // and not the record.
    lines.push_back(std::string(REFERENCE_PREFIX) +
                    "note=this is a reference to the manifest, not the manifest");
    return lines;
}

ReferenceResult parse_gcode_reference(const std::string &gcode)
{
    ReferenceResult result;

    std::map<std::string, std::string> fields;
    size_t                             found = 0;
    for (const std::string &line : split_lines(gcode)) {
        if (line.rfind(REFERENCE_PREFIX, 0) != 0)
            continue;
        ++found;
        const std::string body  = line.substr(std::string(REFERENCE_PREFIX).size());
        const size_t      equal = body.find('=');
        if (equal == std::string::npos) {
            result.status  = ReferenceParse::Malformed;
            result.message = "a manifest reference line has no field: \"" + body + "\"";
            return result;
        }
        fields[body.substr(0, equal)] = body.substr(equal + 1);
    }

    if (found == 0) {
        result.status  = ReferenceParse::Absent;
        result.message = "the g-code carries no manifest reference, so nothing in it can be "
                         "traced back to what produced it";
        return result;
    }

    for (const char *required : { "version", "commit", "digest" })
        if (! fields.count(required)) {
            result.status  = ReferenceParse::Malformed;
            result.message = std::string("the manifest reference has no ") + required +
                             " field, so it does not identify a manifest";
            return result;
        }

    ManifestReference reference;
    reference.version = fields["version"];
    reference.commit  = fields["commit"];
    reference.digest  = fields["digest"];
    auto number       = [&fields](const char *name) -> size_t {
        auto it = fields.find(name);
        if (it == fields.end())
            return 0;
        try {
            return size_t(std::stoul(it->second));
        } catch (...) {
            return 0;
        }
    };
    reference.profiles            = number("profiles");
    reference.warnings            = number("warnings");
    reference.overrides           = number("overrides");
    reference.full_manifest_saved = fields.count("full") && fields["full"] == "saved";

    result.status    = ReferenceParse::Found;
    result.reference = reference;
    result.message   = "manifest " + reference.digest + " referenced";
    return result;
}

// --- comparison ------------------------------------------------------------

namespace {

void settle(ManifestComparison &comparison)
{
    if (! comparison.mismatches.empty()) {
        comparison.verdict = ComparisonVerdict::Mismatch;
        comparison.summary = std::to_string(comparison.mismatches.size()) + " of " +
                             std::to_string(comparison.compared) + " compared field(s) disagree";
    } else if (comparison.compared == 0) {
        comparison.verdict = ComparisonVerdict::Unproven;
        comparison.summary = "nothing could be compared against the " + comparison.against +
                             ", so agreement has not been shown";
    } else {
        comparison.verdict = ComparisonVerdict::Match;
        comparison.summary = "all " + std::to_string(comparison.compared) +
                             " compared field(s) agree";
    }
    if (! comparison.unchecked.empty())
        comparison.summary += "; " + std::to_string(comparison.unchecked.size()) +
                              " field(s) could not be checked";
}

void compare_field(ManifestComparison &comparison, const std::string &field,
                   const std::string &mine, const std::string &theirs)
{
    if (mine.empty() || theirs.empty()) {
        comparison.unchecked.push_back(
            field + ": " + (mine.empty() ? "the manifest does not record it"
                                         : "the other side does not record it"));
        return;
    }
    ++comparison.compared;
    if (mine != theirs)
        comparison.mismatches.push_back({ field, mine, theirs });
}

} // namespace

ManifestComparison compare_to_project(const PrintManifest                  &manifest,
                                      const std::string                    &project_hash,
                                      const std::vector<ModelHash>         &models,
                                      const std::vector<ProfileVersionKey> &profiles)
{
    ManifestComparison comparison;
    comparison.against = "project";

    compare_field(comparison, "project hash", manifest.project_hash, project_hash);

    std::map<std::string, std::string> mine, theirs;
    for (const ModelHash &model : manifest.models)
        mine[model.name] = model.hash;
    for (const ModelHash &model : models)
        theirs[model.name] = model.hash;

    std::vector<std::string> names;
    for (const auto &pair : mine)
        names.push_back(pair.first);
    for (const auto &pair : theirs)
        if (! mine.count(pair.first))
            names.push_back(pair.first);
    std::sort(names.begin(), names.end());

    for (const std::string &name : names) {
        auto a = mine.find(name);
        auto b = theirs.find(name);
        if (a == mine.end()) {
            comparison.mismatches.push_back({ "model " + name, "<absent>", b->second });
            ++comparison.compared;
            continue;
        }
        if (b == theirs.end()) {
            comparison.mismatches.push_back({ "model " + name, a->second, "<absent>" });
            ++comparison.compared;
            continue;
        }
        ++comparison.compared;
        if (a->second != b->second)
            comparison.mismatches.push_back({ "model " + name, a->second, b->second });
    }

    std::vector<ProfileVersionKey> sorted_mine = manifest.profiles;
    std::vector<ProfileVersionKey> sorted_theirs = profiles;
    std::sort(sorted_mine.begin(), sorted_mine.end());
    std::sort(sorted_theirs.begin(), sorted_theirs.end());

    if (sorted_mine.size() != sorted_theirs.size()) {
        ++comparison.compared;
        comparison.mismatches.push_back({ "profile count",
                                          std::to_string(sorted_mine.size()),
                                          std::to_string(sorted_theirs.size()) });
    } else {
        for (size_t i = 0; i < sorted_mine.size(); ++i) {
            ++comparison.compared;
            if (sorted_mine[i] != sorted_theirs[i])
                comparison.mismatches.push_back({ "profile", sorted_mine[i].to_string(),
                                                  sorted_theirs[i].to_string() });
        }
    }

    settle(comparison);
    return comparison;
}

ManifestComparison compare_to_gcode(const PrintManifest &manifest, const std::string &gcode)
{
    ManifestComparison comparison;
    comparison.against = "g-code";

    const ReferenceResult reference = parse_gcode_reference(gcode);
    if (reference.status != ReferenceParse::Found) {
        comparison.unchecked.push_back(std::string("the whole comparison: ") + reference.message);
        settle(comparison);
        return comparison;
    }

    compare_field(comparison, "version", manifest.tigerslicer_version,
                  reference.reference->version);
    compare_field(comparison, "commit", manifest.tigerslicer_commit,
                  reference.reference->commit);
    compare_field(comparison, "digest", manifest_digest(manifest), reference.reference->digest);

    ++comparison.compared;
    if (manifest.profiles.size() != reference.reference->profiles)
        comparison.mismatches.push_back({ "profile count",
                                          std::to_string(manifest.profiles.size()),
                                          std::to_string(reference.reference->profiles) });

    ++comparison.compared;
    if (manifest.warnings.size() != reference.reference->warnings)
        comparison.mismatches.push_back({ "warning count",
                                          std::to_string(manifest.warnings.size()),
                                          std::to_string(reference.reference->warnings) });

    ++comparison.compared;
    if (manifest.overrides.size() != reference.reference->overrides)
        comparison.mismatches.push_back({ "override count",
                                          std::to_string(manifest.overrides.size()),
                                          std::to_string(reference.reference->overrides) });

    if (! reference.reference->full_manifest_saved)
        comparison.unchecked.push_back(
            "everything the reference does not carry: the full manifest was not saved beside "
            "the g-code, so only the digest and the counts could be checked");

    settle(comparison);
    return comparison;
}

ManifestComparison compare_to_telemetry(const PrintManifest                      &manifest,
                                        const std::map<std::string, std::string> &telemetry)
{
    ManifestComparison comparison;
    comparison.against = "telemetry";

    // Everything the manifest claims, flattened, so a printer's report can be
    // checked against it by name.
    std::map<std::string, std::string> claimed;
    claimed["version"] = manifest.tigerslicer_version;
    claimed["commit"]  = manifest.tigerslicer_commit;
    claimed["project"] = manifest.project_hash;
    for (const ManifestEntry &entry : manifest.estimates)
        claimed["estimate." + entry.key] = entry.value;
    for (const ManifestEntry &entry : manifest.firmware_capabilities)
        claimed["firmware." + entry.key] = entry.value;
    for (const ToolMapping &tool : manifest.tools) {
        claimed["tool." + std::to_string(tool.index) + ".material"] = tool.material;
        if (tool.nozzle_mm.has_value())
            claimed["tool." + std::to_string(tool.index) + ".nozzle_mm"] =
                optional_number(tool.nozzle_mm);
    }

    for (const auto &reported : telemetry) {
        auto mine = claimed.find(reported.first);
        if (mine == claimed.end() || mine->second.empty()) {
            // A printer reporting something the slicer never claimed is not a
            // contradiction, and calling it one would bury the real ones.
            comparison.unchecked.push_back(reported.first +
                                           ": the manifest makes no claim about it");
            continue;
        }
        ++comparison.compared;
        if (mine->second != reported.second)
            comparison.mismatches.push_back({ reported.first, mine->second, reported.second });
    }

    settle(comparison);
    return comparison;
}

} // namespace Slic3r
