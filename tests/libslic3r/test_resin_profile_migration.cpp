#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ResinProfileMigration.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

namespace {

DynamicPrintConfig native_resin_config()
{
    DynamicPrintConfig config;
    config.apply(SLAFullPrintConfig::defaults());
    config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    config.set_key_value("sla_archive_format", new ConfigOptionString("goo"));
    config.set_key_value("resin_supported_formats", new ConfigOptionStrings({ "ctb", "goo", "nanodlp" }));
    config.set_key_value("display_width", new ConfigOptionFloat(192.));
    config.set_key_value("display_height", new ConfigOptionFloat(120.));
    config.set_key_value("display_pixels_x", new ConfigOptionInt(3840));
    config.set_key_value("display_pixels_y", new ConfigOptionInt(2400));
    return config;
}

} // namespace

TEST_CASE("native Resin schema validates exact printer and format capabilities", "[resin-profiles]")
{
    ConfigOptionStrings serialized_formats;
    REQUIRE(serialized_formats.deserialize("ctb;goo;nanodlp", false));
    CHECK(serialized_formats.values == std::vector<std::string>{ "ctb", "goo", "nanodlp" });

    DynamicPrintConfig config = native_resin_config();
    CHECK(validate_resin_profile_contract(config).empty());

    config.set_key_value("sla_archive_format", new ConfigOptionString("ctb"));
    CHECK(validate_resin_profile_contract(config).empty());

    config.set_key_value("sla_archive_format", new ConfigOptionString("svg"));
    CHECK(validate_resin_profile_contract(config).find("not supported by this printer") != std::string::npos);
    config.set_key_value("resin_supported_formats", new ConfigOptionStrings({ "goo", "svg" }));
    CHECK(validate_resin_profile_contract(config).empty());

    config.set_key_value("resin_supported_formats", new ConfigOptionStrings({ "goo" }));
    CHECK(validate_resin_profile_contract(config).find("not supported by this printer") != std::string::npos);

    config.set_key_value("sla_archive_format", new ConfigOptionString("sl1"));
    CHECK(validate_resin_profile_contract(config).find("not supported by the integrated MSlicer engine") != std::string::npos);
}

TEST_CASE("Resin physical-printer protocol is explicit and round-trips", "[resin-profiles]")
{
    ConfigOptionEnum<ResinPrintHostType> protocol;
    CHECK(protocol.value == rhtLocalHttp);
    REQUIRE(protocol.deserialize("mslicer_sdcp", false));
    CHECK(protocol.value == rhtMSlicerSDCP);
    CHECK(protocol.serialize() == "mslicer_sdcp");

    DynamicPrintConfig config;
    config.set_key_value("resin_host_type", new ConfigOptionEnum<ResinPrintHostType>(rhtMSlicerSDCP));
    CHECK(config.option<ConfigOptionEnum<ResinPrintHostType>>("resin_host_type")->value == rhtMSlicerSDCP);
}

TEST_CASE("safe legacy Resin print fields migrate without changing their meaning", "[resin-profiles]")
{
    DynamicPrintConfig legacy;
    legacy.set_key_value("faded_layers", new ConfigOptionInt(7));

    const ResinMigrationResult result = migrate_resin_profile(legacy, ResinProfileKind::Print);
    CHECK(result.source_version == 0);
    CHECK(result.changed);
    CHECK_FALSE(result.requires_choice());
    CHECK(legacy.opt_int("mslicer_transition_layers") == 7);
    CHECK(legacy.opt_int("resin_print_schema_version") == RESIN_PROFILE_SCHEMA_VERSION);
    CHECK_FALSE(legacy.opt_bool("resin_print_migration_required"));
}

TEST_CASE("ambiguous legacy Resin semantics are preserved as blocking diagnostics", "[resin-profiles]")
{
    DynamicPrintConfig material;
    material.set_key_value("material_print_speed", new ConfigOptionEnum<SLAMaterialSpeed>(slamsFast));
    material.set_key_value("delay_before_exposure", new ConfigOptionFloats({ 1., 2. }));
    const ResinMigrationResult material_result = migrate_resin_profile(material, ResinProfileKind::Material);
    REQUIRE(material_result.requires_choice());
    CHECK(material.opt_int("resin_material_schema_version") == RESIN_PROFILE_SCHEMA_VERSION);
    CHECK(material.opt_bool("resin_material_migration_required"));
    CHECK(material.option<ConfigOptionStrings>("resin_material_migration_diagnostics")->values.size() == 2);

    DynamicPrintConfig printer;
    printer.set_key_value("sla_archive_format", new ConfigOptionString("sl1"));
    const ResinMigrationResult printer_result = migrate_resin_profile(printer, ResinProfileKind::Printer);
    REQUIRE(printer_result.requires_choice());
    CHECK(printer.opt_string("sla_archive_format") == "sl1");
    CHECK(printer.opt_bool("resin_printer_migration_required"));

    DynamicPrintConfig full = native_resin_config();
    full.set_key_value("resin_printer_migration_required", new ConfigOptionBool(true));
    full.set_key_value("resin_printer_migration_diagnostics",
        new ConfigOptionStrings({ "unsupported_legacy_format: preserved" }));
    CHECK(validate_resin_profile_contract(full).find("requires a choice") != std::string::npos);
}

TEST_CASE("future Resin schema versions are rejected rather than downgraded", "[resin-profiles]")
{
    DynamicPrintConfig profile;
    profile.set_key_value("resin_printer_schema_version", new ConfigOptionInt(RESIN_PROFILE_SCHEMA_VERSION + 1));
    const ResinMigrationResult result = migrate_resin_profile(profile, ResinProfileKind::Printer);
    CHECK(result.requires_choice());
    CHECK(profile.opt_int("resin_printer_schema_version") == RESIN_PROFILE_SCHEMA_VERSION + 1);
}

TEST_CASE("Resin schema remains disjoint from FDM defaults and projects", "[resin-profiles]")
{
    PresetBundle bundle;
    const DynamicPrintConfig &fdm_printer = bundle.printers.default_preset(0).config;

    CHECK_FALSE(fdm_printer.has("resin_supported_formats"));

    const DynamicPrintConfig fdm_project = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(fdm_project.has("resin_print_schema_version"));
    CHECK_FALSE(fdm_project.has("resin_material_schema_version"));
    CHECK_FALSE(fdm_project.has("resin_printer_schema_version"));
}
