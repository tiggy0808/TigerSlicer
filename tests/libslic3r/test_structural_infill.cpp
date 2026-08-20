// Phase 20: the heuristic structural infill planner. This is the phase most
// likely to be mistaken for FEA, so the tests hold it to saying what it is:
// the disclaimer leads every proposal's reasons and cannot be read past. The
// other properties are that no strength setting is ever reduced silently, an
// unstated requirement is reported rather than guessed, reinforcement is only
// proposed from a material whose stiffness is actually recorded, and a plan
// that cannot be priced says so instead of reporting zero.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/StructuralInfill.hpp"

using namespace Slic3r;

namespace {

LoadDirection load(double x, double y, std::optional<double> magnitude = 100.)
{
    LoadDirection value;
    value.name        = "applied";
    value.x           = x;
    value.y           = y;
    value.magnitude_n = magnitude;
    return value;
}

StructuralSettings settings()
{
    StructuralSettings value;
    value.infill_density      = 0.15;
    value.infill_angle_deg    = 45.;
    value.wall_count          = 2;
    value.solid_top_layers    = 4;
    value.solid_bottom_layers = 4;
    return value;
}

std::vector<StructuralTool> machine()
{
    StructuralTool pla;
    pla.tool             = 0;
    pla.material         = "PLA";
    pla.stiffness_factor = 1.0;
    pla.throughput_mm3_s = 15.;

    StructuralTool cf;
    cf.tool             = 1;
    cf.material         = "PA-CF";
    cf.stiffness_factor = 2.4;
    cf.throughput_mm3_s = 8.;
    return { pla, cf };
}

const StructuralChange *change_of(const StructuralPlan &plan, StructuralChangeKind kind)
{
    for (const StructuralChange &change : plan.changes)
        if (change.kind == kind)
            return &change;
    return nullptr;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Every proposal leads with the heuristic disclaimer",
          "[structural-infill]")
{
    StructuralOptions options;
    options.priority = StructuralPriority::Stiffness;

    const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {}, settings(),
                                                       machine(), options);

    REQUIRE_FALSE(plan.changes.empty());
    for (const StructuralChange &change : plan.changes)
        CHECK(change.reasons.front() == std::string(structural_disclaimer()));
    CHECK(std::string(structural_disclaimer()).find("not a certified") != std::string::npos);
    CHECK(mentions(plan.notes, structural_disclaimer()));
}

TEST_CASE("Infill is aligned with the dominant load direction", "[structural-infill]")
{
    const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {}, settings(),
                                                       machine(), StructuralOptions {});

    const StructuralChange *direction =
        change_of(plan, StructuralChangeKind::InfillDirection);
    REQUIRE(direction != nullptr);
    CHECK(direction->proposed_value == Catch::Approx(0.).margin(1e-6));
    CHECK(mentions(direction->reasons, "along the path the force takes"));
}

TEST_CASE("Loads that cancel produce no direction, and say so",
          "[structural-infill]")
{
    const StructuralPlan plan = plan_structural_infill({ load(1., 0.), load(-1., 0.) }, {}, {},
                                                       settings(), machine(),
                                                       StructuralOptions {});
    CHECK(change_of(plan, StructuralChangeKind::InfillDirection) == nullptr);
    CHECK(mentions(plan.notes, "cancel out in plane"));
}

TEST_CASE("A load with no magnitude is counted but the roughness is stated",
          "[structural-infill]")
{
    const StructuralPlan plan = plan_structural_infill({ load(0., 1., std::nullopt) }, {}, {},
                                                       settings(), machine(),
                                                       StructuralOptions {});
    CHECK(mentions(plan.notes, "counted as unit weight"));
}

TEST_CASE("Walls are increased before density, for the reason they are",
          "[structural-infill]")
{
    StructuralOptions options;
    options.priority = StructuralPriority::Stiffness;
    options.safety   = SafetyPreference::Conservative;

    const StructuralPlan plan = plan_structural_infill({ load(1., 1.) }, {}, {}, settings(),
                                                       machine(), options);

    const StructuralChange *walls = change_of(plan, StructuralChangeKind::WallCount);
    REQUIRE(walls != nullptr);
    CHECK(walls->proposed_value == Catch::Approx(4.));
    CHECK(mentions(walls->reasons, "carry far more load than infill of the same mass"));

    const StructuralChange *density = change_of(plan, StructuralChangeKind::InfillDensity);
    REQUIRE(density != nullptr);
    CHECK(density->proposed_value == Catch::Approx(0.40));
}

TEST_CASE("A conservative preference asks for more material than a nominal one",
          "[structural-infill]")
{
    StructuralOptions conservative;
    conservative.priority = StructuralPriority::Stiffness;
    conservative.safety   = SafetyPreference::Conservative;

    StructuralOptions nominal = conservative;
    nominal.safety            = SafetyPreference::Nominal;

    const double with_care =
        change_of(plan_structural_infill({ load(1., 0.) }, {}, {}, settings(), machine(),
                                         conservative),
                  StructuralChangeKind::InfillDensity)
            ->proposed_value;
    const double without =
        change_of(plan_structural_infill({ load(1., 0.) }, {}, {}, settings(), machine(),
                                         nominal),
                  StructuralChangeKind::InfillDensity)
            ->proposed_value;

    CHECK(with_care > without);
}

TEST_CASE("A change that weakens the part is withheld unless it was asked for",
          "[structural-infill]")
{
    StructuralOptions weight;
    weight.priority = StructuralPriority::Weight;

    SECTION("withheld by default") {
        const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {},
                                                           settings(), machine(), weight);
        CHECK(change_of(plan, StructuralChangeKind::InfillDensity) == nullptr);
        REQUIRE(plan.withheld.size() == 1);
        CHECK(mentions(plan.withheld, "leaves the part weaker"));
        CHECK(mentions(plan.withheld, "never reduced without being asked for"));
    }

    SECTION("made when reductions are permitted") {
        weight.allow_strength_reduction = true;
        const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {},
                                                           settings(), machine(), weight);
        const StructuralChange *density = change_of(plan, StructuralChangeKind::InfillDensity);
        REQUIRE(density != nullptr);
        CHECK(density->reduces_strength);
        CHECK(density->proposed_value == Catch::Approx(0.10));
        CHECK(plan.withheld.empty());
    }
}

TEST_CASE("Fixed mounting surfaces get solid material under them",
          "[structural-infill]")
{
    MountingSurface mount;
    mount.id    = 1;
    mount.fixed = true;

    const StructuralPlan plan = plan_structural_infill({}, { mount }, {}, settings(),
                                                       machine(), StructuralOptions {});
    const StructuralChange *solid = change_of(plan, StructuralChangeKind::SolidLayers);
    REQUIRE(solid != nullptr);
    CHECK(solid->proposed_value == Catch::Approx(6.));
    CHECK(mentions(solid->reasons, "spread the fastener load"));
}

TEST_CASE("A critical hole with no stated requirement is reported, not guessed",
          "[structural-infill]")
{
    CriticalHole vague;
    vague.id          = 7;
    vague.diameter_mm = 5.;

    CriticalHole stated  = vague;
    stated.id            = 8;
    stated.required_wall_mm = 3.;

    const StructuralPlan plan = plan_structural_infill({}, {}, { vague, stated }, settings(),
                                                       machine(), StructuralOptions {});

    CHECK(mentions(plan.notes, "no required wall thickness was given"));
    const StructuralChange *rib = change_of(plan, StructuralChangeKind::Rib);
    REQUIRE(rib != nullptr);
    CHECK(rib->proposed_value == Catch::Approx(3.));
}

TEST_CASE("Reinforcement comes only from a material whose stiffness is recorded",
          "[structural-infill]")
{
    StructuralOptions options;
    options.priority = StructuralPriority::Stiffness;

    SECTION("a characterized material is proposed") {
        const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {},
                                                           settings(), machine(), options);
        const StructuralChange *reinforcement =
            change_of(plan, StructuralChangeKind::Reinforcement);
        REQUIRE(reinforcement != nullptr);
        REQUIRE(reinforcement->tool.has_value());
        CHECK(*reinforcement->tool == 1);
        CHECK(mentions(reinforcement->reasons, "PA-CF"));
    }

    SECTION("uncharacterized materials propose nothing") {
        std::vector<StructuralTool> unmeasured = machine();
        for (StructuralTool &tool : unmeasured)
            tool.stiffness_factor.reset();

        const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {},
                                                           settings(), unmeasured, options);
        CHECK(change_of(plan, StructuralChangeKind::Reinforcement) == nullptr);
        CHECK(mentions(plan.notes, "no tool has a recorded stiffness factor"));
    }
}

TEST_CASE("The material and time effect is reported when it can be",
          "[structural-infill]")
{
    StructuralOptions options;
    options.priority        = StructuralPriority::Stiffness;
    options.part_volume_mm3 = 100000.;

    const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {}, settings(),
                                                       machine(), options);

    REQUIRE(plan.material_delta_mm3.has_value());
    // 0.15 -> 0.40 of 100000 mm3.
    CHECK(*plan.material_delta_mm3 == Catch::Approx(25000.));
    REQUIRE(plan.time_delta_s.has_value());
    CHECK(*plan.time_delta_s == Catch::Approx(25000. / 15.));
}

TEST_CASE("A plan that cannot be priced says so rather than reporting zero",
          "[structural-infill]")
{
    StructuralOptions options;
    options.priority = StructuralPriority::Stiffness;

    const StructuralPlan plan = plan_structural_infill({ load(1., 0.) }, {}, {}, settings(),
                                                       machine(), options);
    CHECK_FALSE(plan.material_delta_mm3.has_value());
    CHECK(mentions(plan.notes, "no part volume was supplied"));
}

TEST_CASE("Nothing described is an error, not an empty plan", "[structural-infill]")
{
    const StructuralPlan plan =
        plan_structural_infill({}, {}, {}, settings(), machine(), StructuralOptions {});
    CHECK_FALSE(plan.error.empty());
}
