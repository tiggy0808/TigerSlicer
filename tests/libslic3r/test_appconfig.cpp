#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"
#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>

using namespace Slic3r;

// Loads an app config file holding one printer ("P") with the given saved filament colours.
static void load_saved_filament_colors(AppConfig &config, const std::string &app_section,
                                       const std::string &colors, const std::string &multi_colors)
{
    ScopedTemporaryFile file(".json");
    {
        boost::nowide::ofstream out(file.string());
        out << "{\n\"app\": {" << app_section << "},\n"
            << "\"orca_presets\": [{\"machine\": \"P\", \"filament_colors\": \"" << colors
            << "\", \"filament_multi_colors\": \"" << multi_colors << "\"}]\n}\n";
    }
    config.set_loading_path(file.string());
    REQUIRE(config.load().empty());
}

TEST_CASE("Saved teal default filament colours load as the pink default", "[AppConfig]") {
    AppConfig config;
    load_saved_filament_colors(config, "", "#26A69A,#FF0000,#26a69a", "#26A69A,#FF0000 #26A69A,#26a69a");

    CHECK(config.get_printer_setting("P", "filament_colors") == "#F6A5C1,#FF0000,#F6A5C1");
    // A multi-colour pack is a user choice even when it contains the old teal.
    CHECK(config.get_printer_setting("P", "filament_multi_colors") == "#F6A5C1,#FF0000 #26A69A,#F6A5C1");
    CHECK(config.get("default_filament_color_migrated") == "1");
}

TEST_CASE("Teal filament colours saved after the migration are kept", "[AppConfig]") {
    AppConfig config;
    load_saved_filament_colors(config, "\"default_filament_color_migrated\": \"1\"", "#26A69A", "#26A69A");

    CHECK(config.get_printer_setting("P", "filament_colors") == "#26A69A");
    CHECK(config.get_printer_setting("P", "filament_multi_colors") == "#26A69A");
}

TEST_CASE("AppConfig network version helpers", "[AppConfig]") {
    AppConfig config;

    SECTION("skipped versions starts empty") {
        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.empty());
    }

    SECTION("add and check skipped version") {
        config.add_skipped_network_version("02.01.01.52");
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE_FALSE(config.is_network_version_skipped("02.03.00.62"));
    }

    SECTION("multiple skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.00.02.50");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 2);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE(config.is_network_version_skipped("02.00.02.50"));
    }

    SECTION("clear skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.clear_skipped_network_versions();
        REQUIRE_FALSE(config.is_network_version_skipped("02.01.01.52"));
    }

    SECTION("duplicate add is idempotent") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.01.01.52");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 1);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
    }
}
