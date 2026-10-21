#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/CalibrationBuilder.hpp"
#include "libslic3r/calib.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::calib_builder;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kBand = 2.;

// Slices a plain block as a calibration builder tower: one band per value, kBand mm tall. With
// `tower_on_plate` false the bands name an object that is not on the plate (a deleted tower).
// `raft` returns where the tower's own bottom is.
std::string tower_gcode(Setting setting, const std::vector<double> &values, std::initializer_list<ConfigBase::SetDeserializeItem> items,
                        bool tower_on_plate = true, double *raft = nullptr)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict(items);
    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., double(values.size()) * kBand) }, print, model, config);
    Calib_Params params;
    params.mode                = CalibMode::Calib_Builder_Tower;
    params.builder_setting     = int(setting);
    params.builder_band_height = kBand;
    params.builder_values      = values;
    params.builder_object      = model.objects.front()->id().id + (tower_on_plate ? 0 : 1000);
    print.set_calib_params(params);
    std::string gcode = Test::gcode(print);
    if (raft != nullptr)
        *raft = print.objects().front()->slicing_parameters().object_print_z_min;
    return gcode;
}

// Each line with the print height of the layer it is in (from the ";Z:" layer markers).
std::vector<std::pair<double, std::string>> lines_by_z(const std::string &gcode)
{
    std::vector<std::pair<double, std::string>> out;
    std::istringstream in(gcode);
    std::string        line;
    double             z = 0.;
    while (std::getline(in, line)) {
        if (line.rfind(";Z:", 0) == 0)
            z = std::stod(line.substr(3));
        out.emplace_back(z, line);
    }
    return out;
}

// The number after `key` on a line, if there is one.
bool number_after(const std::string &line, const std::string &key, double &value)
{
    const size_t pos = line.find(key);
    if (pos == std::string::npos)
        return false;
    value = std::stod(line.substr(pos + key.size()));
    return true;
}

// The band a layer with this print height is in (its middle, as the generator decides it).
size_t band_of(double print_z, double layer_height = 0.2) { return band_at(print_z - layer_height / 2., kBand, size_t(1000)); }

// The value after `key` on the first line that has it after each band's "; Calibration builder:" marker.
std::map<size_t, double> sent_per_band(const std::string &gcode, const std::string &key)
{
    std::map<size_t, double> out;
    bool after_marker = false;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        if (line.rfind("; Calibration builder:", 0) == 0)
            after_marker = true;
        double v;
        if (after_marker && line.rfind(";", 0) != 0 && number_after(line, key, v)) {
            out[band_of(z)] = v;
            after_marker    = false;
        }
    }
    return out;
}

// The first value after `key` in each band, in band order.
std::map<size_t, double> first_per_band(const std::string &gcode, const std::string &key)
{
    std::map<size_t, double> out;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        double v;
        if (line.rfind(";", 0) != 0 && number_after(line, key, v) && !out.count(band_of(z)))
            out[band_of(z)] = v;
    }
    return out;
}

} // namespace

TEST_CASE("A temperature tower sets each band's temperature as the band starts", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 230., 220., 210., 200. };
    const std::string gcode = tower_gcode(Setting::Temperature, values, { { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 } });
    std::map<size_t, double> set = sent_per_band(gcode, "M104 S");
    // The first band's temperature is the profile's start temperature; every later band sets its own.
    for (size_t band = 1; band < values.size(); ++band) {
        INFO("band " << band);
        REQUIRE(set.count(band));
        CHECK_THAT(set[band], WithinAbs(values[band], 1e-9));
    }
}

TEST_CASE("A pressure advance tower sends each band's value in the firmware's command", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 0.01, 0.03, 0.05 };
    const std::string gcode = tower_gcode(Setting::PressureAdvance, values, { { "gcode_flavor", "klipper" }, { "layer_height", 0.2 } });
    const std::map<size_t, double> set = first_per_band(gcode, "SET_PRESSURE_ADVANCE ADVANCE=");
    for (size_t band = 0; band < values.size(); ++band) {
        INFO("band " << band);
        REQUIRE(set.count(band));
        CHECK_THAT(set.at(band), WithinAbs(values[band], 1e-6));
    }
}

TEST_CASE("A retraction tower retracts by each band's length", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 0.4, 1.0, 1.6 };
    const std::string gcode = tower_gcode(Setting::RetractionLength, values,
                                          { { "layer_height", 0.2 }, { "use_relative_e_distances", 1 }, { "retract_when_changing_layer", "1" },
                                            { "wipe", "0" }, { "use_firmware_retraction", 0 } });
    std::map<size_t, std::vector<double>> retracts;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        double e;
        // A pure retraction: "G1 E-<length> F<speed>" with no X/Y.
        if (line.rfind("G1 E-", 0) == 0 && number_after(line, "E-", e))
            retracts[band_of(z)].push_back(e);
    }
    for (size_t band = 0; band < values.size(); ++band) {
        INFO("band " << band);
        REQUIRE_FALSE(retracts[band].empty());
        // The first retraction of a band may still use the previous length; the rest use this band's.
        CHECK_THAT(retracts[band].back(), WithinAbs(values[band], 1e-4));
    }
}

TEST_CASE("A flow ratio tower scales the plastic of each band", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 0.9, 1.1 };
    // Walls only, so every layer has the same paths.
    const std::string gcode = tower_gcode(Setting::FlowRatio, values,
                                          { { "layer_height", 0.2 }, { "use_relative_e_distances", 1 }, { "sparse_infill_density", "0%" },
                                            { "top_shell_layers", 0 }, { "bottom_shell_layers", 0 }, { "wall_loops", 2 } });
    std::map<double, double> extruded; // per layer
    for (const auto &[z, line] : lines_by_z(gcode)) {
        double e;
        if (line.rfind("G1 X", 0) == 0 && number_after(line, " E", e) && e > 0.)
            extruded[z] += e;
    }
    // One layer from the middle of each band, away from the first layer.
    REQUIRE(extruded.count(1.0));
    REQUIRE(extruded.count(3.0));
    CHECK_THAT(extruded.at(3.0) / extruded.at(1.0), WithinRel(values[1] / values[0], 0.01));
}

TEST_CASE("A print speed tower prints each band's walls at its speed", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 40., 80. };
    const std::string gcode = tower_gcode(Setting::PrintSpeed, values,
                                          { { "layer_height", 0.2 }, { "slow_down_for_layer_cooling", "0" }, { "filament_max_volumetric_speed", "200" },
                                            { "machine_max_speed_x", "500" }, { "machine_max_speed_y", "500" } });
    // The feedrate in force for each extrusion move (Orca often sets it on a line of its own first).
    std::map<size_t, double> fastest;
    double feedrate = 0.;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        if (line.rfind("G1", 0) != 0)
            continue;
        double f;
        if (number_after(line, " F", f))
            feedrate = f / 60.;
        if (z > 0.3 && line.find(" X") != std::string::npos && line.find(" E") != std::string::npos)
            fastest[band_of(z)] = std::max(fastest[band_of(z)], feedrate);
    }
    for (size_t band = 0; band < values.size(); ++band) {
        INFO("band " << band);
        CHECK_THAT(fastest[band], WithinAbs(values[band], 0.5));
    }
}

TEST_CASE("An acceleration tower sets each band's acceleration", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 2000., 6000. };
    const std::string gcode = tower_gcode(Setting::Acceleration, values,
                                          { { "gcode_flavor", "klipper" }, { "layer_height", 0.2 }, { "machine_max_acceleration_extruding", "20000" },
                                            { "machine_max_acceleration_x", "20000" }, { "machine_max_acceleration_y", "20000" } });
    // The acceleration in force for each extrusion move (travel moves have their own, higher one).
    std::map<size_t, double> highest;
    double acceleration = 0.;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        double a;
        if (number_after(line, "SET_VELOCITY_LIMIT ACCEL=", a))
            acceleration = a;
        else if (z > 0.3 && line.rfind("G1 X", 0) == 0 && line.find(" E") != std::string::npos)
            highest[band_of(z)] = std::max(highest[band_of(z)], acceleration);
    }
    for (size_t band = 0; band < values.size(); ++band) {
        INFO("band " << band);
        CHECK_THAT(highest[band], WithinAbs(values[band], 1e-6));
    }
}

TEST_CASE("Jerk and input shaping towers send each band's value to the firmware", "[CalibrationBuilder]")
{
    SECTION("square corner velocity on Klipper")
    {
        const std::vector<double> values = { 3., 9., 15. };
        const std::string gcode = tower_gcode(Setting::Jerk, values,
                                              { { "gcode_flavor", "klipper" }, { "layer_height", 0.2 }, { "default_jerk", "0" },
                                                { "machine_max_jerk_x", "20" }, { "machine_max_jerk_y", "20" } });
        const std::map<size_t, double> set = first_per_band(gcode, "SQUARE_CORNER_VELOCITY=");
        for (size_t band = 0; band < values.size(); ++band) {
            INFO("band " << band);
            REQUIRE(set.count(band));
            CHECK_THAT(set.at(band), WithinAbs(values[band], 1e-6));
        }
    }
    SECTION("junction deviation on Marlin 2")
    {
        const std::vector<double> values = { 0.02, 0.06, 0.1 };
        const std::string gcode = tower_gcode(Setting::JunctionDeviation, values,
                                              { { "gcode_flavor", "marlin2" }, { "layer_height", 0.2 }, { "default_junction_deviation", "0" },
                                                { "machine_max_junction_deviation", "0.3" } });
        const std::map<size_t, double> set = sent_per_band(gcode, "M205 J");
        for (size_t band = 0; band < values.size(); ++band) {
            INFO("band " << band);
            REQUIRE(set.count(band));
            CHECK_THAT(set.at(band), WithinAbs(values[band], 1e-6));
        }
    }
    SECTION("input shaper frequency on Klipper")
    {
        const std::vector<double> values = { 30., 50. };
        const std::string gcode = tower_gcode(Setting::InputShapingFrequency, values,
                                              { { "gcode_flavor", "klipper" }, { "layer_height", 0.2 }, { "input_shaping_emit", 0 } });
        const std::map<size_t, double> set = first_per_band(gcode, "SHAPER_FREQ_X=");
        for (size_t band = 0; band < values.size(); ++band) {
            INFO("band " << band);
            REQUIRE(set.count(band));
            CHECK_THAT(set.at(band), WithinAbs(values[band], 1e-6));
        }
        CHECK(gcode.find("MINIMUM_CRUISE_RATIO=0") != std::string::npos);
    }
}

TEST_CASE("Grid copies print with their own value", "[CalibrationBuilder]")
{
    // Two copies as the grid makes them: each object carries object_overrides() for its value.
    auto items = [](double value) {
        std::vector<ConfigBase::SetDeserializeItem> out;
        const DynamicPrintConfig overrides = object_overrides(Setting::FlowRatio, value);
        for (const std::string &key : overrides.keys())
            out.emplace_back(key, overrides.option(key)->serialize());
        return out;
    };
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Walls only, so both copies have the same paths.
    config.set_deserialize_strict({ { "layer_height", 0.2 }, { "use_relative_e_distances", 1 }, { "sparse_infill_density", "0%" },
                                    { "top_shell_layers", 0 }, { "bottom_shell_layers", 0 }, { "wall_loops", 2 } });
    const std::string gcode = Test::slice_with_object_overrides({ make_cube(20., 20., 4.), make_cube(20., 20., 4.) }, config,
                                                                { items(0.9), items(1.1) });
    // Plastic per object on one layer above the first, from the "; printing object" markers.
    std::map<std::string, double> extruded;
    std::string object;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        if (line.rfind("; printing object ", 0) == 0)
            object = line; // holds the object's id
        else if (line.rfind("; stop printing object", 0) == 0)
            object.clear();
        double e;
        if (!object.empty() && std::abs(z - 2.0) < 0.05 && line.rfind("G1 X", 0) == 0 && number_after(line, " E", e) && e > 0.)
            extruded[object] += e;
    }
    REQUIRE(extruded.size() == 2);
    const double a = extruded.begin()->second, b = extruded.rbegin()->second;
    CHECK_THAT(std::max(a, b) / std::min(a, b), WithinRel(1.1 / 0.9, 0.01));
}

TEST_CASE("A tower on a raft counts its bands from its own bottom", "[CalibrationBuilder]")
{
    const std::vector<double> values = { 230., 210. };
    double raft = 0.;
    const std::string gcode = tower_gcode(Setting::Temperature, values,
                                          { { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 }, { "raft_layers", 3 } }, true, &raft);
    REQUIRE(raft > 0.);
    double second_band_z = -1.;
    for (const auto &[z, line] : lines_by_z(gcode))
        if (line.rfind("; Calibration builder:", 0) == 0 && line.find(" 210") != std::string::npos) {
            second_band_z = z;
            break;
        }
    REQUIRE(second_band_z > 0.);
    // The first layer whose middle is kBand above the tower's bottom, not above the bed.
    CHECK(second_band_z - raft > kBand);
    CHECK(second_band_z - raft < kBand + 0.5);
}

TEST_CASE("Layers above the tower print without its overrides", "[CalibrationBuilder]")
{
    // A two-band flow tower (0.9, then 1.1) next to a cube twice its height.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", 0.2 }, { "use_relative_e_distances", 1 }, { "sparse_infill_density", "0%" },
                                    { "top_shell_layers", 0 }, { "bottom_shell_layers", 0 }, { "wall_loops", 2 } });
    Print print;
    Model model;
    Test::init_print({ make_cube(20., 20., 2. * kBand), make_cube(20., 20., 4. * kBand) }, print, model, config);
    Calib_Params params;
    params.mode                = CalibMode::Calib_Builder_Tower;
    params.builder_setting     = int(Setting::FlowRatio);
    params.builder_band_height = kBand;
    params.builder_values      = { 0.9, 1.1 };
    params.builder_object      = model.objects.front()->id().id;
    print.set_calib_params(params);
    const std::string gcode = Test::gcode(print);
    // Plastic per object and layer, from the "; printing object" markers.
    std::map<std::string, std::map<double, double>> extruded;
    std::string object;
    for (const auto &[z, line] : lines_by_z(gcode)) {
        if (line.rfind("; printing object ", 0) == 0)
            object = line; // holds the object's id
        else if (line.rfind("; stop printing object", 0) == 0)
            object.clear();
        double e;
        if (!object.empty() && line.rfind("G1 X", 0) == 0 && number_after(line, " E", e) && e > 0.)
            extruded[object][z] += e;
    }
    // The cube is the object still printing above the tower.
    const double above = 3. * kBand;
    const auto cube = std::find_if(extruded.begin(), extruded.end(), [above](const auto &o) { return o.second.count(above) > 0; });
    REQUIRE(cube != extruded.end());
    REQUIRE(cube->second.count(1.0));
    // In the first band the cube shares the tower's 0.9; above the tower it is back at 1.0, not the last band's 1.1.
    CHECK_THAT(cube->second.at(above) / cube->second.at(1.0), WithinRel(1.0 / 0.9, 0.01));
}

TEST_CASE("Without a builder tower nothing is changed per band", "[CalibrationBuilder]")
{
    const std::string gcode = Test::slice({ make_cube(20., 20., 6.) }, { { "layer_height", 0.2 } });
    CHECK(gcode.find("; Calibration builder:") == std::string::npos);
}

TEST_CASE("Once the tower is off the plate its bands change nothing", "[CalibrationBuilder]")
{
    const std::string gcode = tower_gcode(Setting::Temperature, { 230., 220., 210. }, { { "layer_height", 0.2 } }, false);
    CHECK(gcode.find("; Calibration builder:") == std::string::npos);
}
