#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/CalibrationBuilder.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::calib_builder;
using Catch::Matchers::WithinAbs;

TEST_CASE("Every builder setting has a unique id, a valid default sweep and a known layout", "[CalibrationBuilder]")
{
    std::set<std::string> ids;
    for (const SettingInfo &s : settings()) {
        INFO(s.id);
        CHECK(ids.insert(s.id).second);
        Setting parsed;
        REQUIRE(parse_setting(s.id, parsed));
        CHECK(parsed == s.setting);
        CHECK(&info(s.setting) == &s);
        std::string error;
        const std::vector<double> values = sweep(s.from, s.to, s.step, &error);
        CHECK(error.empty());
        CHECK(values.size() >= 3);
        for (double v : values) {
            CHECK(v >= s.min - 1e-9);
            CHECK(v <= s.max + 1e-9);
        }
        // The default sweep prints on the model exactly as it is used.
        CHECK(fits_decimals(s.setting, s.from));
        CHECK(fits_decimals(s.setting, s.to));
        CHECK(fits_decimals(s.setting, s.step));
        // The lowest accepted value is one the printer acts on.
        CHECK(s.min >= 0.);
    }
    CHECK(settings().size() == 11);
    // Settings changed by printer commands while printing cannot differ between copies of one print.
    for (Setting s : {Setting::Temperature, Setting::PressureAdvance, Setting::RetractionLength, Setting::RetractionSpeed,
                      Setting::Jerk, Setting::JunctionDeviation, Setting::InputShapingFrequency, Setting::InputShapingDamping})
        CHECK_FALSE(info(s).grid);
    for (Setting s : {Setting::FlowRatio, Setting::PrintSpeed, Setting::Acceleration})
        CHECK(info(s).grid);
    Setting unused;
    CHECK_FALSE(parse_setting("no_such_setting", unused));
}

TEST_CASE("A sweep runs up or down and includes the end on the step", "[CalibrationBuilder]")
{
    const std::vector<double> up = sweep(0., 0.08, 0.02);
    REQUIRE(up.size() == 5);
    CHECK_THAT(up.back(), WithinAbs(0.08, 1e-12));
    const std::vector<double> down = sweep(230., 190., 5.);
    REQUIRE(down.size() == 9);
    CHECK_THAT(down.front(), WithinAbs(230., 1e-12));
    CHECK_THAT(down.back(), WithinAbs(190., 1e-12));
    // An end between steps is left out; rounding does not drop an end that lies on a step.
    CHECK(sweep(0., 1., 0.3).size() == 4);
    CHECK(sweep(0.9, 1.1, 0.05).size() == 5);
    // A single value is a sweep of one.
    CHECK(sweep(210., 210., 5.).size() == 1);
}

TEST_CASE("A sweep with no step or too many values is refused with a reason", "[CalibrationBuilder]")
{
    std::string error;
    CHECK(sweep(0., 1., 0., &error).empty());
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK(sweep(0., 1., -0.1, &error).empty());
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK(sweep(0., 100., 1., &error).empty());
    CHECK(error.find("40") != std::string::npos);
    // A step far too small for the range is refused, not wrapped around to a short sweep.
    error.clear();
    CHECK(sweep(0., 1e300, 1e-300, &error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("A value fits a setting when it needs no more decimals than the setting shows", "[CalibrationBuilder]")
{
    CHECK(fits_decimals(Setting::Temperature, 215.));
    CHECK_FALSE(fits_decimals(Setting::Temperature, 192.5));
    CHECK(fits_decimals(Setting::PressureAdvance, 0.005));
    CHECK(fits_decimals(Setting::PressureAdvance, 0.1 + 0.2));
    CHECK_FALSE(fits_decimals(Setting::PressureAdvance, 0.0025));
    CHECK_FALSE(fits_decimals(Setting::FlowRatio, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(fits_decimals(Setting::FlowRatio, std::numeric_limits<double>::infinity()));
}

TEST_CASE("A printer is told when it cannot run a test", "[CalibrationBuilder]")
{
    auto printer = [](const char *flavor, double junction_deviation) {
        DynamicPrintConfig c;
        c.apply(FullPrintConfig::defaults());
        c.set_deserialize_strict({ { "gcode_flavor", flavor } });
        c.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats { junction_deviation });
        return c;
    };
    const DynamicPrintConfig klipper = printer("klipper", 0.), marlin_jd = printer("marlin2", 0.08), marlin_jerk = printer("marlin2", 0.),
                             repetier = printer("repetier", 0.);
    // Jerk and junction deviation: the one the firmware uses.
    CHECK(printer_problem(Setting::Jerk, klipper).empty());
    CHECK(printer_problem(Setting::Jerk, marlin_jerk).empty());
    CHECK_FALSE(printer_problem(Setting::Jerk, marlin_jd).empty());
    CHECK(printer_problem(Setting::JunctionDeviation, marlin_jd).empty());
    CHECK_FALSE(printer_problem(Setting::JunctionDeviation, klipper).empty());
    CHECK_FALSE(printer_problem(Setting::JunctionDeviation, marlin_jerk).empty());
    // Input shaping only where the firmware takes it from G-code.
    CHECK(printer_problem(Setting::InputShapingFrequency, klipper).empty());
    CHECK(printer_problem(Setting::InputShapingDamping, marlin_jerk).empty());
    CHECK_FALSE(printer_problem(Setting::InputShapingFrequency, repetier).empty());
    // Everything else runs anywhere.
    CHECK(printer_problem(Setting::Temperature, repetier).empty());
    CHECK(printer_problem(Setting::FlowRatio, repetier).empty());
}

TEST_CASE("Values print with the setting's decimals and no trailing zeros", "[CalibrationBuilder]")
{
    CHECK(format_value(Setting::Temperature, 215.) == "215");
    CHECK(format_value(Setting::PressureAdvance, 0.025) == "0.025");
    CHECK(format_value(Setting::PressureAdvance, 0.02) == "0.02");
    CHECK(format_value(Setting::FlowRatio, 0.95) == "0.95");
    CHECK(format_value(Setting::FlowRatio, 1.) == "1");
    CHECK(format_value(Setting::RetractionLength, 0.8) == "0.8");
    CHECK(format_value(Setting::Acceleration, 5000.) == "5000");
}

TEST_CASE("A height falls in its band, clamped to the last band", "[CalibrationBuilder]")
{
    CHECK(band_at(0.1, 5., 4) == 0);
    CHECK(band_at(4.9, 5., 4) == 0);
    CHECK(band_at(5.1, 5., 4) == 1);
    CHECK(band_at(19.9, 5., 4) == 3);
    CHECK(band_at(40., 5., 4) == 3);
    CHECK(band_at(-1., 5., 4) == 0);
}

TEST_CASE("A preset survives a round trip through JSON", "[CalibrationBuilder]")
{
    Setup setup;
    setup.setting      = Setting::FlowRatio;
    setup.from         = 0.9;
    setup.to           = 1.1;
    setup.step         = 0.02;
    setup.layout       = Layout::Grid;
    setup.band_height  = 4.;
    setup.model_path   = "C:/models/tile.stl";
    setup.layer_height = 0.16;
    setup.print_speed  = 120.;
    Setup back;
    REQUIRE(from_json(to_json(setup), back));
    CHECK(back.setting == setup.setting);
    CHECK(back.from == setup.from);
    CHECK(back.to == setup.to);
    CHECK(back.step == setup.step);
    CHECK(back.layout == setup.layout);
    CHECK(back.band_height == setup.band_height);
    CHECK(back.model_path == setup.model_path);
    CHECK(back.layer_height == setup.layer_height);
    CHECK(back.print_speed == setup.print_speed);
    Setup untouched;
    CHECK_FALSE(from_json("not json", untouched));
    CHECK_FALSE(from_json(R"({"setting":"no_such_setting"})", untouched));
}

TEST_CASE("Raised digits are as tall and thick as asked and widen with the text", "[CalibrationBuilder]")
{
    double width_one = 0., width_five = 0.;
    const indexed_triangle_set one  = label_mesh("7", 4., 0.6, &width_one);
    const indexed_triangle_set five = label_mesh("0.025", 4., 0.6, &width_five);
    REQUIRE_FALSE(one.indices.empty());
    REQUIRE_FALSE(five.indices.empty());
    const BoundingBoxf3 box = bounding_box(five);
    CHECK_THAT(box.max.z() - box.min.z(), WithinAbs(0.6, 1e-5));
    CHECK_THAT(box.max.y() - box.min.y(), WithinAbs(4., 1e-5));
    CHECK(width_five > 3. * width_one);
    CHECK_THAT(box.max.x() - box.min.x(), WithinAbs(width_five, 1e-5));
    // Characters it cannot draw are skipped, not drawn as garbage.
    double width_bad = 0.;
    CHECK(label_mesh("?", 4., 0.6, &width_bad).indices.empty());
}

TEST_CASE("The built-in tower has one band per value with its label on the front", "[CalibrationBuilder]")
{
    const std::vector<double> values = sweep(230., 200., 10.);
    const TriangleMesh tower = make_tower(Setting::Temperature, values, 5.);
    const BoundingBoxf3 box  = tower.bounding_box();
    CHECK_THAT(box.min.z(), WithinAbs(0., 1e-5));
    CHECK_THAT(box.max.z(), WithinAbs(values.size() * 5., 1e-5));
    // 20 mm across, and the raised digits stand proud of the front face.
    CHECK_THAT(box.max.x() - box.min.x(), WithinAbs(20., 1e-5));
    CHECK(box.max.y() - box.min.y() > 20.);
    CHECK(box.max.y() - box.min.y() < 21.5);
}

TEST_CASE("The built-in tile is flat with its label on top", "[CalibrationBuilder]")
{
    const TriangleMesh tile = make_tile(Setting::FlowRatio, 0.95);
    const BoundingBoxf3 box = tile.bounding_box();
    CHECK_THAT(box.max.x() - box.min.x(), WithinAbs(22., 1e-5));
    CHECK_THAT(box.max.y() - box.min.y(), WithinAbs(22., 1e-5));
    CHECK(box.max.z() > 4.);
    CHECK(box.max.z() < 5.);
}

TEST_CASE("Grid copies override the tested setting on the object", "[CalibrationBuilder]")
{
    const DynamicPrintConfig flow = object_overrides(Setting::FlowRatio, 0.95);
    CHECK_THAT(flow.opt_float("print_flow_ratio"), WithinAbs(0.95, 1e-12));
    const DynamicPrintConfig speed = object_overrides(Setting::PrintSpeed, 150.);
    for (const char *key : {"outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed"}) {
        INFO(key);
        REQUIRE(speed.has(key));
        CHECK_THAT(speed.option<ConfigOptionFloatsNullable>(key)->get_at(0), WithinAbs(150., 1e-12));
    }
    const DynamicPrintConfig accel = object_overrides(Setting::Acceleration, 4000.);
    CHECK_THAT(accel.option<ConfigOptionFloatsNullable>("outer_wall_acceleration")->get_at(0), WithinAbs(4000., 1e-12));
    CHECK_THAT(accel.option<ConfigOptionFloatsOrPercentsNullable>("sparse_infill_acceleration")->get_at(0).value, WithinAbs(4000., 1e-12));
    // Only per-object options: every key must be allowed on a model object.
    for (const DynamicPrintConfig *cfg : {&flow, &speed, &accel})
        for (const std::string &key : cfg->keys()) {
            INFO(key);
            const bool per_object = PrintObjectConfig().has(key) || PrintRegionConfig().has(key);
            CHECK(per_object);
        }
    CHECK(object_overrides(Setting::Temperature, 215.).empty());
    CHECK(object_overrides(Setting::Jerk, 8.).empty());
}

TEST_CASE("Profiles are prepared so the tested setting shows its effect", "[CalibrationBuilder]")
{
    auto fresh = []() {
        DynamicPrintConfig c;
        c.apply(FullPrintConfig::defaults());
        return c;
    };
    SECTION("temperature starts at the first value")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        prepare_profiles(Setting::Temperature, 230., 230., print, filament, printer);
        CHECK(filament.option<ConfigOptionInts>("nozzle_temperature")->get_at(0) == 230);
        CHECK(filament.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->get_at(0) == 230);
    }
    SECTION("pressure advance is on, starts at the first value and is not adaptive")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        prepare_profiles(Setting::PressureAdvance, 0.01, 0.08, print, filament, printer);
        CHECK(filament.option<ConfigOptionBools>("enable_pressure_advance")->get_at(0));
        CHECK_THAT(filament.option<ConfigOptionFloats>("pressure_advance")->get_at(0), WithinAbs(0.01, 1e-12));
        CHECK_FALSE(filament.option<ConfigOptionBools>("adaptive_pressure_advance")->get_at(0));
    }
    SECTION("retraction is done by the slicer, not the firmware")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        printer.set_key_value("use_firmware_retraction", new ConfigOptionBool(true));
        prepare_profiles(Setting::RetractionLength, 0.2, 2., print, filament, printer);
        CHECK_FALSE(printer.opt_bool("use_firmware_retraction"));
    }
    SECTION("a speed test is not slowed down by cooling or the volumetric limit")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        prepare_profiles(Setting::PrintSpeed, 50., 300., print, filament, printer);
        CHECK_FALSE(filament.option<ConfigOptionBools>("slow_down_for_layer_cooling")->get_at(0));
        CHECK(filament.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0) >= 200.);
    }
    SECTION("an acceleration test is not capped by the machine limits")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        printer.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats { 3000. });
        prepare_profiles(Setting::Acceleration, 2000., 12000., print, filament, printer);
        CHECK(printer.option<ConfigOptionFloats>("machine_max_acceleration_extruding")->get_at(0) >= 12000.);
        CHECK(printer.option<ConfigOptionFloats>("machine_max_acceleration_x")->get_at(0) >= 12000.);
    }
    SECTION("a jerk test turns per-feature jerk off and lifts the machine limit")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        prepare_profiles(Setting::Jerk, 4., 20., print, filament, printer);
        CHECK_THAT(print.option<ConfigOptionFloatsNullable>("default_jerk")->get_at(0), WithinAbs(0., 1e-12));
        CHECK(printer.option<ConfigOptionFloats>("machine_max_jerk_x")->get_at(0) >= 20.);
        CHECK(printer.option<ConfigOptionFloats>("machine_max_jerk_y")->get_at(0) >= 20.);
    }
    SECTION("a junction deviation test lifts the junction deviation limit, not the jerk limit")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        printer.set_deserialize_strict({ { "gcode_flavor", "marlin2" } });
        printer.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats { 0.05 });
        printer.set_key_value("machine_max_jerk_x", new ConfigOptionFloats { 8. });
        prepare_profiles(Setting::JunctionDeviation, 0.02, 0.2, print, filament, printer);
        CHECK(printer.option<ConfigOptionFloats>("machine_max_junction_deviation")->get_at(0) >= 0.2);
        CHECK_THAT(print.option<ConfigOptionFloatsNullable>("default_junction_deviation")->get_at(0), WithinAbs(0., 1e-12));
        CHECK_THAT(printer.option<ConfigOptionFloats>("machine_max_jerk_x")->get_at(0), WithinAbs(8., 1e-12));
    }
    SECTION("input shaping is left to the test, not emitted from the profile")
    {
        DynamicPrintConfig print = fresh(), filament = fresh(), printer = fresh();
        prepare_profiles(Setting::InputShapingFrequency, 20., 80., print, filament, printer);
        CHECK_FALSE(printer.opt_bool("input_shaping_emit"));
        CHECK(print.opt_bool("spiral_mode"));
    }
}
