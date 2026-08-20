#include "ResinProfileMigration.hpp"

#include <boost/algorithm/string/case_conv.hpp>

#include <algorithm>

namespace Slic3r {

namespace {

struct SchemaKeys {
    const char *version;
    const char *required;
    const char *diagnostics;
};

SchemaKeys schema_keys(ResinProfileKind kind)
{
    switch (kind) {
    case ResinProfileKind::Print:
        return { "resin_print_schema_version", "resin_print_migration_required", "resin_print_migration_diagnostics" };
    case ResinProfileKind::Material:
        return { "resin_material_schema_version", "resin_material_migration_required", "resin_material_migration_diagnostics" };
    case ResinProfileKind::Printer:
        return { "resin_printer_schema_version", "resin_printer_migration_required", "resin_printer_migration_diagnostics" };
    }
    return {};
}

void add_issue(ResinMigrationResult &result, std::string code, std::string message, bool requires_choice)
{
    result.issues.push_back({ std::move(code), std::move(message), requires_choice });
}

std::vector<std::string> normalized_formats(const DynamicPrintConfig &config)
{
    const auto *formats = config.option<ConfigOptionStrings>("resin_supported_formats");
    if (formats == nullptr)
        return {};
    std::vector<std::string> out;
    out.reserve(formats->values.size());
    for (const std::string &value : formats->values)
        out.emplace_back(canonical_resin_output_format(value));
    return out;
}

} // namespace

bool ResinMigrationResult::requires_choice() const noexcept
{
    return std::any_of(issues.begin(), issues.end(), [](const ResinMigrationIssue &issue) { return issue.requires_choice; });
}

std::string canonical_resin_output_format(std::string format)
{
    boost::algorithm::to_lower(format);
    if (format == "nano-dlp")
        return "nanodlp";
    return format;
}

bool is_mslicer_output_format(std::string format)
{
    format = canonical_resin_output_format(std::move(format));
    return format == "ctb" || format == "goo" || format == "nanodlp" || format == "svg";
}

ResinMigrationResult migrate_resin_profile(DynamicPrintConfig &config, ResinProfileKind kind)
{
    const SchemaKeys keys = schema_keys(kind);
    ResinMigrationResult result;
    if (const auto *version = config.option<ConfigOptionInt>(keys.version, false))
        result.source_version = version->value;

    if (result.source_version > RESIN_PROFILE_SCHEMA_VERSION) {
        add_issue(result, "future_schema",
            "This Resin profile was created by a newer schema version and cannot be interpreted safely.", true);
    } else if (result.source_version == 0) {
        if (kind == ResinProfileKind::Print && config.has("faded_layers") && !config.has("mslicer_transition_layers")) {
            config.set_key_value("mslicer_transition_layers",
                new ConfigOptionInt(config.opt_int("faded_layers")));
            result.changed = true;
        }

        if (kind == ResinProfileKind::Material) {
            if (config.has("material_print_speed"))
                add_issue(result, "legacy_motion_mode",
                    "Legacy Resin speed modes do not uniquely define MSlicer lift and retract motion; review native motion values.", true);
            if (config.has("delay_before_exposure") || config.has("delay_after_exposure"))
                add_issue(result, "legacy_tilt_timing",
                    "Legacy tilt timing cannot be converted to MSlicer wait and motion fields without printer-specific knowledge.", true);
        }

        if (kind == ResinProfileKind::Printer && config.has("sla_archive_format")) {
            const std::string source = config.opt_string("sla_archive_format");
            const std::string canonical = canonical_resin_output_format(source);
            if (is_mslicer_output_format(canonical)) {
                if (canonical != source) {
                    config.set_key_value("sla_archive_format", new ConfigOptionString(canonical));
                    result.changed = true;
                }
            } else {
                add_issue(result, "unsupported_legacy_format",
                    "Legacy Resin output format '" + source + "' is not implemented by the integrated MSlicer engine; choose a compatible native format.", true);
            }
        }
    }

    std::vector<std::string> diagnostics;
    diagnostics.reserve(result.issues.size());
    for (const ResinMigrationIssue &issue : result.issues)
        diagnostics.emplace_back(issue.code + ": " + issue.message);

    const int stored_version = result.source_version > RESIN_PROFILE_SCHEMA_VERSION ?
        result.source_version : RESIN_PROFILE_SCHEMA_VERSION;
    config.set_key_value(keys.version, new ConfigOptionInt(stored_version));
    config.set_key_value(keys.required, new ConfigOptionBool(result.requires_choice()));
    config.set_key_value(keys.diagnostics, new ConfigOptionStrings(std::move(diagnostics)));
    result.changed = result.changed || result.source_version != RESIN_PROFILE_SCHEMA_VERSION || !result.issues.empty();
    return result;
}

std::string validate_resin_profile_contract(const DynamicPrintConfig &config)
{
    for (ResinProfileKind kind : { ResinProfileKind::Print, ResinProfileKind::Material, ResinProfileKind::Printer }) {
        const SchemaKeys keys = schema_keys(kind);
        if (const auto *required = config.option<ConfigOptionBool>(keys.required); required && required->value) {
            const auto *diagnostics = config.option<ConfigOptionStrings>(keys.diagnostics);
            const std::string detail = diagnostics && !diagnostics->values.empty() ? diagnostics->values.front() : "unresolved legacy values";
            return "Resin profile migration requires a choice: " + detail;
        }
        if (const auto *version = config.option<ConfigOptionInt>(keys.version);
            version && version->value != RESIN_PROFILE_SCHEMA_VERSION)
            return "Unsupported Resin profile schema version " + std::to_string(version->value) + ".";
    }

    const std::string selected = canonical_resin_output_format(config.opt_string("sla_archive_format"));
    if (!is_mslicer_output_format(selected))
        return "The selected Resin output format '" + selected + "' is not supported by the integrated MSlicer engine.";

    const std::vector<std::string> formats = normalized_formats(config);
    if (formats.empty())
        return "The Resin printer profile does not declare any compatible output formats.";
    if (std::find(formats.begin(), formats.end(), selected) == formats.end())
        return "The selected Resin output format '" + selected + "' is not supported by this printer profile.";

    const int pixels_x = config.opt_int("display_pixels_x");
    const int pixels_y = config.opt_int("display_pixels_y");
    const double width = config.opt_float("display_width");
    const double height = config.opt_float("display_height");
    if (pixels_x <= 0 || pixels_y <= 0 || width <= 0. || height <= 0.)
        return "Resin printer display dimensions and pixel resolution must be positive explicit values.";

    return {};
}

} // namespace Slic3r
