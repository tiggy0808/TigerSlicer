#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Feature/SmallFeatures/SmallFeatures.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;

static const std::string kSmallFeatures = std::string(TEST_DATA_DIR) + "/small_features/";

// The reference pulley: a GT2-style 22-tooth pulley, 8.96 mm tall, with 0.27 mm wide tooth tips at r = 6.749 mm.
static TriangleMesh pulley_mesh()
{
    Model        model = Model::read_from_step(kSmallFeatures + "22T_pulley.step", LoadStrategy::AddDefaultInstances, nullptr, nullptr,
                                               nullptr, 0.003, 0.5, false);
    TriangleMesh mesh  = model.objects.front()->mesh();
    // Flat on the bed, as in the reference slice.
    const Vec3d size = mesh.bounding_box().size();
    if (std::abs(size.y() - 8.96) < 0.1)
        mesh.rotate_x(float(M_PI / 2));
    else if (std::abs(size.x() - 8.96) < 0.1)
        mesh.rotate_y(float(M_PI / 2));
    return mesh;
}

// The settings of the reference slice with classic walls, from the "; key = value" lines of its config block.
static DynamicPrintConfig pulley_config()
{
    DynamicPrintConfig        config = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    std::ifstream             in(kSmallFeatures + "22T_pulley_PLA_15m23s.gcode");
    std::string               line;
    bool                      in_block = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "; CONFIG_BLOCK_START")
            in_block = true;
        else if (line == "; CONFIG_BLOCK_END")
            break;
        else if (in_block && line.rfind("; ", 0) == 0) {
            const size_t      equals = line.find(" = ");
            const std::string key    = equals == std::string::npos ? std::string() : line.substr(2, equals - 2);
            const ConfigOptionDef *def = key.empty() ? nullptr : print_config_def.get(key);
            // Lists of enums such as extruder_type ("Direct Drive") crash set_deserialize(); they describe the
            // printer's hardware and don't change the slice.
            if (def == nullptr || def->type == coEnums)
                continue;
            try {
                config.set_deserialize(key, line.substr(equals + 3), substitutions);
            } catch (const std::exception &) {
                // Settings this version reads differently keep their defaults.
            }
        }
    }
    return config;
}

// The position of `p` (scaled) relative to `center`, in mm.
static Vec2d relative_mm(const Point &p, const Point &center) { return (p - center).cast<double>() * SCALING_FACTOR; }

// Which of the 22 teeth each angle falls on, counted from the first.
static std::set<int> teeth_hit(const std::vector<double> &angles)
{
    std::set<int> teeth;
    const double step = 2. * M_PI / 22.;
    for (double angle : angles)
        teeth.insert(int(((std::lround((angle - angles.front()) / step) % 22) + 22) % 22));
    return teeth;
}

TEST_CASE("The deviation of the classic pulley slice shows every tooth tip unprinted", "[SmallFeatures]")
{
    Print print;
    Model model;
    init_print({pulley_mesh()}, print, model, pulley_config());
    print.process();
    const PrintObject &object  = *print.objects().front();
    size_t             checked = 0;
    for (const Layer *layer : object.layers()) {
        // The teeth stand from z = 1.16 to 8.0.
        if (layer->print_z < 1.2 || layer->print_z > 7.9)
            continue;
        const Point         center = get_extents(layer->lslices).center();
        std::vector<double> angles;
        for (const SmallFeatures::DeviationArea &area : SmallFeatures::layer_deviation(*layer).undershoot) {
            const Vec2d c = relative_mm(area.shape.contour.centroid(), center);
            // Between the cusp where the outer wall turns (r = 6.32) and the tip (r = 6.749).
            if (c.norm() > 6.3 && c.norm() < 6.8)
                angles.push_back(std::atan2(c.y(), c.x()));
        }
        CAPTURE(layer->print_z, angles.size());
        REQUIRE(!angles.empty());
        CHECK(teeth_hit(angles).size() == 22);
        ++checked;
    }
    CHECK(checked > 40);
}

TEST_CASE("Slicing warns about features below the printable width", "[SmallFeatures]")
{
    const bool experimental = GENERATE(true, false);
    CAPTURE(experimental);
    // A 10 x 10 x 3 mm block with a 0.15 mm thin fin standing 4 mm out of one side. 0.15 mm is below the default
    // small_feature_min_width, 60% of the 0.4 mm nozzle.
    TriangleMesh part = make_cube(10, 10, 3);
    TriangleMesh fin  = make_cube(4, 0.15, 3);
    fin.translate(10, 5, 0);
    part.merge(fin);
    Print print;
    Model model;
    print.set_experimental_features_enabled(experimental);
    init_print({part}, print, model, {{"nozzle_diameter", "0.4"}, {"layer_height", 0.2}});
    print.process();
    const auto state = print.objects().front()->step_state_with_warnings(posPerimeters);
    if (experimental) {
        REQUIRE(state.warnings.size() == 1);
        CHECK(state.warnings.front().message.find("1 features below printable width for a 0.4 mm nozzle") != std::string::npos);
    } else {
        // An experimental-only warning.
        CHECK(state.warnings.empty());
    }
}
