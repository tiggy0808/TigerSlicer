#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
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

// The reference pulley sliced with its settings and `extra` on top.
static std::unique_ptr<Print> slice_pulley(std::initializer_list<ConfigBase::SetDeserializeItem> extra, Model &model)
{
    DynamicPrintConfig config = pulley_config();
    config.set_deserialize_strict(extra);
    auto print = std::make_unique<Print>();
    init_print({pulley_mesh()}, *print, model, config);
    print->process();
    return print;
}

// The dead ends (small feature lines started on a wall loop) of the layer.
static ExtrusionPaths layer_dead_ends(const Layer &layer)
{
    ExtrusionPaths out;
    std::function<void(const ExtrusionEntity &)> visit = [&out, &visit](const ExtrusionEntity &entity) {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
            for (const ExtrusionEntity *child : collection->entities)
                visit(*child);
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
            append(out, loop->dead_ends);
    };
    for (const LayerRegion *region : layer.regions())
        visit(region->perimeters);
    return out;
}

// The tooth-tip lines of the hand-made reference: per layer height, the radius of each line's end around the
// pulley's center (the mean of its 22 ends).
static std::map<double, std::vector<double>> reference_tip_radii()
{
    std::map<double, std::vector<Vec2d>> ends;
    std::ifstream                        in(kSmallFeatures + "22T_pulley_PLA_spiketeeth.gcode");
    std::string                          line;
    double                               z         = 0.;
    bool                                 next_tip  = false;
    while (std::getline(in, line)) {
        if (line.rfind(";Z:", 0) == 0)
            z = std::stod(line.substr(3));
        else if (line.rfind("; tooth spike", 0) == 0)
            next_tip = true;
        else if (next_tip && line.rfind("G1 X", 0) == 0) {
            std::istringstream words(line);
            std::string        word;
            Vec2d              p = Vec2d::Zero();
            while (words >> word) {
                if (word[0] == 'X')
                    p.x() = std::stod(word.substr(1));
                else if (word[0] == 'Y')
                    p.y() = std::stod(word.substr(1));
            }
            ends[z].push_back(p);
            next_tip = false;
        }
    }
    std::map<double, std::vector<double>> radii;
    for (const auto &[height, points] : ends) {
        Vec2d center = Vec2d::Zero();
        for (const Vec2d &p : points)
            center += p / double(points.size());
        for (const Vec2d &p : points)
            radii[height].push_back((p - center).norm());
    }
    return radii;
}

TEST_CASE("Outside prints one line out to each tooth tip, as the hand-made reference does", "[SmallFeatures]")
{
    const auto reference = reference_tip_radii();
    REQUIRE(!reference.empty());
    double reference_radius = 0.;
    size_t reference_count  = 0;
    for (const auto &[height, radii] : reference) {
        CHECK(radii.size() == 22);
        for (double r : radii)
            reference_radius += r;
        reference_count += radii.size();
    }
    reference_radius /= double(reference_count);
    // The reference ends its 0.30 mm lines at r = 6.649, their bead 0.05 mm past the 6.749 mm tip.
    CHECK_THAT(reference_radius, Catch::Matchers::WithinAbs(6.649, 0.01));

    Model model;
    const auto print = slice_pulley({{"small_feature_policy", "outside"}}, model);
    size_t checked = 0;
    for (const Layer *layer : print->objects().front()->layers()) {
        if (layer->print_z < 1.2 || layer->print_z > 7.9)
            continue;
        CAPTURE(layer->print_z);
        const Point          center = get_extents(layer->lslices).center();
        const ExtrusionPaths lines  = layer_dead_ends(*layer);
        CHECK(lines.size() == 22);
        for (const ExtrusionPath &line : lines) {
            const Vec2d start = relative_mm(line.first_point(), center);
            const Vec2d end   = relative_mm(line.last_point(), center);
            CAPTURE(line.width, start.norm(), end.norm());
            // From the cusp where the wall turns, out to the tip.
            CHECK_THAT(start.norm(), Catch::Matchers::WithinAbs(6.32, 0.05));
            CHECK_THAT(end.norm(), Catch::Matchers::WithinAbs(reference_radius, 0.02));
            // Its bead reaches the default 0.05 mm tip overshoot past the tip.
            CHECK_THAT(end.norm() + 0.5 * line.width, Catch::Matchers::WithinAbs(6.749 + 0.05, 0.02));
            // Straight out along the tooth.
            const double angle_error = std::abs(std::atan2(start.x() * end.y() - start.y() * end.x(), start.dot(end)));
            CHECK(angle_error < 0.5 * M_PI / 180.);
        }
        ++checked;
    }
    CHECK(checked > 40);
}

TEST_CASE("Outside lines go out and back from the wall at the small feature speed, without retracting", "[SmallFeatures]")
{
    Model model;
    // Without the cooling slowdown, which scales every speed on these small layers.
    const auto print = slice_pulley({{"small_feature_policy", "outside"}, {"thin_wall_speed", 15}, {"slow_down_for_layer_cooling", "0"}}, model);
    const PrintObject &object = *print->objects().front();
    const Layer       &middle = *object.layers()[object.layers().size() / 2];
    const Vec2d        center = (get_extents(middle.lslices).center() + object.instances().front().shift).cast<double>() * SCALING_FACTOR;
    auto radius = [&center](double x, double y) { return (Vec2d(x, y) - center).norm(); };

    // On the tooth layers, each extrusion at the small feature speed is a line out to a tooth tip, followed by the
    // move back to the wall at the same speed, without extruding, and with no retraction in between.
    const double line_speed   = 15. * 60.;
    size_t       out_and_back = 0;
    bool         at_tip       = false;
    GCodeReader  reader;
    reader.parse_buffer(gcode(*print), [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (!line.cmd_is("G1") && !line.cmd_is("G0"))
            return;
        if (line.dist_XY(self) <= 0) {
            if (line.retracting(self))
                CHECK_FALSE(at_tip);
            return;
        }
        const double r = radius(line.new_X(self), line.new_Y(self));
        if (at_tip) {
            CAPTURE(line.raw(), r);
            CHECK_FALSE(line.extruding(self));
            CHECK_THAT(line.new_F(self), Catch::Matchers::WithinAbs(line_speed, 1.));
            CHECK_THAT(r, Catch::Matchers::WithinAbs(6.32, 0.05));
            ++out_and_back;
            at_tip = false;
        } else if (self.z() > 1.2 && self.z() < 7.9 && line.extruding(self) && std::abs(line.new_F(self) - line_speed) < 1.) {
            CAPTURE(line.raw(), r);
            CHECK(r > 6.55);
            at_tip = true;
        }
    });
    // 22 teeth on each of the tooth layers.
    CHECK(out_and_back >= 22 * 40);
}

TEST_CASE("Inside lines never cross the model's edge", "[SmallFeatures]")
{
    Model      model;
    const auto print   = slice_pulley({{"small_feature_policy", "inside"}}, model);
    size_t     checked = 0;
    for (const Layer *layer : print->objects().front()->layers()) {
        if (layer->print_z < 1.2 || layer->print_z > 7.9)
            continue;
        CAPTURE(layer->print_z);
        CHECK(!layer_dead_ends(*layer).empty());
        const Point center = get_extents(layer->lslices).center();
        for (const SmallFeatures::DeviationArea &area : SmallFeatures::layer_deviation(*layer).overshoot) {
            // No extrusion outside the model around the teeth.
            const double r = relative_mm(area.shape.contour.centroid(), center).norm();
            CAPTURE(r, area.magnitude);
            CHECK((r < 6.2 || r > 6.9));
        }
        ++checked;
    }
    CHECK(checked > 40);
}

// The direction of each tooth tip from the pulley's center: the middle of the outline points at the tip radius.
static std::vector<Vec2d> tip_directions(const Layer &layer, const Point &center)
{
    std::vector<Vec2d> sums;
    for (const ExPolygon &slice : layer.lslices)
        for (const Point &p : slice.contour.points) {
            const Vec2d at = relative_mm(p, center);
            if (at.norm() < 6.749 - 0.005)
                continue;
            const Vec2d direction = at.normalized();
            auto        tooth = std::find_if(sums.begin(), sums.end(), [&direction](const Vec2d &sum) { return sum.normalized().dot(direction) > std::cos(0.1); });
            if (tooth == sums.end())
                sums.push_back(direction);
            else
                *tooth += direction;
        }
    for (Vec2d &sum : sums)
        sum.normalize();
    return sums;
}

TEST_CASE("Outside prints out to every tooth tip with either wall generator", "[SmallFeatures]")
{
    const std::string generator = GENERATE(std::string("classic"), std::string("arachne"));
    CAPTURE(generator);
    Model      model;
    const auto print = slice_pulley({{"small_feature_policy", "outside"}, {"wall_generator", generator}}, model);
    size_t     checked = 0;
    for (const Layer *layer : print->objects().front()->layers()) {
        if (layer->print_z < 1.2 || layer->print_z > 7.9)
            continue;
        CAPTURE(layer->print_z);
        const Point    center  = get_extents(layer->lslices).center();
        const Polygons covered = SmallFeatures::printed_coverage(*layer);
        // The middle of each 0.27 mm wide flat tip, at r = 6.749, is printed to within the deviation tolerance: by a
        // small feature line or, where Arachne carries its own bead out that far, by the wall.
        const std::vector<Vec2d> tips = tip_directions(*layer, center);
        CHECK(tips.size() == 22);
        for (const Vec2d &direction : tips)
            CHECK(contains(covered, Point(center + (direction * scaled<double>(6.749 - SmallFeatures::kDeviationTolerance)).cast<coord_t>())));
        // What is left at the tips is their square corners, which a round bead end fills only when it spills well
        // down the tooth's sides.
        for (const SmallFeatures::DeviationArea &area : SmallFeatures::layer_deviation(*layer).undershoot) {
            const double r = relative_mm(area.shape.contour.centroid(), center).norm();
            CAPTURE(r, area.magnitude);
            if (r > 6.3 && r < 6.8)
                CHECK(area.magnitude < 0.03);
        }
        ++checked;
    }
    CHECK(checked > 40);
}

TEST_CASE("Outside prints a free-standing wall thinner than a line", "[SmallFeatures]")
{
    // 0.25 mm thick: too thin for even one classic wall loop, so nothing else prints it.
    Model model;
    Print print;
    init_print({make_cube(6, 0.25, 3)}, print, model,
               {{"nozzle_diameter", "0.4"}, {"layer_height", 0.2}, {"initial_layer_print_height", 0.2}, {"wall_generator", "classic"},
                {"small_feature_policy", "outside"}});
    print.process();
    size_t checked = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        CAPTURE(layer->print_z);
        double unprinted = 0.;
        for (const SmallFeatures::DeviationArea &area : SmallFeatures::layer_deviation(*layer).undershoot)
            unprinted += unscale<double>(unscale<double>(area.shape.area()));
        // Of its 1.5 mm2.
        CHECK(unprinted < 0.05);
        ++checked;
    }
    CHECK(checked == 15);
}

TEST_CASE("Small feature lines leave the corners of a block alone", "[SmallFeatures]")
{
    const std::string generator = GENERATE(std::string("classic"), std::string("arachne"));
    CAPTURE(generator);
    Model model;
    Print print;
    init_print({make_cube(20, 20, 5)}, print, model, {{"wall_generator", generator}, {"small_feature_policy", "outside"}});
    print.process();
    for (const Layer *layer : print.objects().front()->layers()) {
        CAPTURE(layer->print_z);
        // The walls round its corners off, but a 20 x 20 mm block has no part narrower than two walls.
        CHECK(layer_dead_ends(*layer).empty());
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->perimeters.entities)
                CHECK(dynamic_cast<const ExtrusionPath *>(entity) == nullptr);
    }
}

// FNV-1a over the G-code's moves only.
static uint64_t moves_hash(const std::string &gcode)
{
    uint64_t           hash = 1469598103934665603ull;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line))
        if (line.rfind("G0 ", 0) == 0 || line.rfind("G1 ", 0) == 0 || line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
            for (const unsigned char c : line.substr(0, line.find(';'))) {
                hash ^= c;
                hash *= 1099511628211ull;
            }
    return hash;
}

TEST_CASE("Drop prints exactly what the walls print without the small feature options", "[SmallFeatures]")
{
    Model      model_default, model_drop;
    const auto by_default = slice_pulley({}, model_default);
    // The other small feature settings do nothing while the policy is Drop.
    const auto with_drop = slice_pulley(
        {{"small_feature_policy", "drop"}, {"tip_overshoot", 0.2}, {"thin_wall_speed", 30}, {"thin_wall_flow_ratio", 1.3}}, model_drop);
    CHECK(moves_hash(gcode(*with_drop)) == moves_hash(gcode(*by_default)));
    for (const Layer *layer : with_drop->objects().front()->layers())
        CHECK(layer_dead_ends(*layer).empty());
}

// An 18 x 8 x 3 mm block with three slots cut 5.9 mm into one long side, 0.3, 0.5 and 0.8 mm wide (all narrower than
// two 0.42 mm lines), and 0.25 mm fins between them, thinner than one line. In mm from the block's corner.
static const BoundingBoxf kSlots[] = {{{8.00, 2.1}, {8.30, 8.}}, {{8.55, 2.1}, {9.05, 8.}}, {{9.30, 2.1}, {10.10, 8.}}};
static const BoundingBoxf kFins[]  = {{{8.30, 2.1}, {8.55, 8.}}, {{9.05, 2.1}, {9.30, 8.}}};

static std::unique_ptr<Print> slice_slotted_block(std::initializer_list<ConfigBase::SetDeserializeItem> extra, Model &model)
{
    TriangleMesh block = make_cube(18, 2.1, 3);
    auto add = [&block](double x0, double x1) {
        TriangleMesh part = make_cube(x1 - x0, 6, 3);
        part.translate(float(x0), 2.f, 0.f);
        block.merge(part);
    };
    add(0., 8.);
    add(8.30, 8.55);
    add(9.05, 9.30);
    add(10.10, 18.);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"nozzle_diameter", "0.4"}, {"layer_height", 0.2}, {"initial_layer_print_height", 0.2}});
    config.set_deserialize_strict(extra);
    auto print = std::make_unique<Print>();
    init_print({block}, *print, model, config);
    print->process();
    return print;
}

// The boxes (mm, from the block's corner) as polygons on the layer.
static Polygons on_layer(const Layer &layer, std::initializer_list<BoundingBoxf> boxes)
{
    const Point corner = get_extents(layer.lslices).min;
    Polygons    out;
    for (const BoundingBoxf &box : boxes)
        out.push_back(BoundingBox((corner + scaled(box.min)).cast<coord_t>(), (corner + scaled(box.max)).cast<coord_t>()).polygon());
    return out;
}

// The area (mm2) of the deviation areas inside `where`.
static double area_inside(const std::vector<SmallFeatures::DeviationArea> &areas, const Polygons &where)
{
    ExPolygons shapes;
    for (const SmallFeatures::DeviationArea &area : areas)
        shapes.push_back(area.shape);
    return unscale<double>(unscale<double>(area(intersection_ex(shapes, where))));
}

TEST_CASE("Arachne's beads in thin fins spill into the narrow slots beside them by default", "[SmallFeatures]")
{
    Model      model;
    const auto print = slice_slotted_block({{"wall_generator", "arachne"}}, model);
    double     spilled = 0.;
    for (const Layer *layer : print->objects().front()->layers())
        if (layer->print_z > 0.5)
            spilled += area_inside(SmallFeatures::layer_deviation(*layer).overshoot,
                                   on_layer(*layer, {kSlots[0], kSlots[1], kSlots[2]}));
    // Arachne prints a 0.25 mm fin no narrower than its minimum bead width, 85% of the nozzle: 0.34 mm.
    CHECK(spilled > 0.1);
}

TEST_CASE("Keeping narrow gaps open leaves every slot open and still prints the fins", "[SmallFeatures]")
{
    // Arachne gives each fin a bead of its own; classic walls skip a fin narrower than a line, and the outside small
    // feature policy prints it.
    const std::string generator = GENERATE(std::string("arachne"), std::string("classic"));
    CAPTURE(generator);
    Model      model;
    const auto print = slice_slotted_block({{"wall_generator", generator}, {"small_feature_policy", "outside"}, {"narrow_gap_policy", "keep_open"}},
                                           model);
    size_t checked = 0;
    for (const Layer *layer : print->objects().front()->layers()) {
        if (layer->print_z < 0.5)
            continue;
        CAPTURE(layer->print_z);
        const SmallFeatures::LayerDeviation deviation = SmallFeatures::layer_deviation(*layer);
        for (const BoundingBoxf &slot : kSlots) {
            CAPTURE(slot.size().x());
            CHECK_THAT(area_inside(deviation.overshoot, on_layer(*layer, {slot})), Catch::Matchers::WithinAbs(0., 1e-4));
        }
        // The fins, 0.25 mm and above the 0.24 mm small feature minimum width, are printed whole.
        for (const BoundingBoxf &fin : kFins)
            CHECK(area_inside(deviation.undershoot, on_layer(*layer, {fin})) < 0.05);
        // Narrowed walls still join up.
        std::function<void(const ExtrusionEntity &)> joined = [&joined](const ExtrusionEntity &entity) {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
                for (const ExtrusionEntity *child : collection->entities)
                    joined(*child);
            else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
                for (size_t i = 0; i < loop->paths.size(); ++i)
                    CHECK(loop->paths[i].first_point() == loop->paths[(i + loop->paths.size() - 1) % loop->paths.size()].last_point());
        };
        for (const LayerRegion *region : layer->regions())
            joined(region->perimeters);
        ++checked;
    }
    CHECK(checked > 10);
    // And it exports.
    CHECK(!gcode(*print).empty());
}
