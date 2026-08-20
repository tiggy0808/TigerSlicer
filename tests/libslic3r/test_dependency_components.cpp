// Phase 25A: dependency-component decomposition, the gate in front of the
// experimental bounded-Z batching scheduler. The rule the whole phase rests on
// is that visual separation is never proof of independence: two regions are
// only split apart when the geometry says so, and a region nobody analyzed
// leaves its component unproven, which blocks batching rather than being
// assumed safe. Object identity is not evidence either way, in either
// direction - one object's regions stay together unless splitting was asked
// for, and two objects that touch stay together regardless.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/DependencyComponents.hpp"

using namespace Slic3r;

namespace {

PrintableRegion tower(size_t id, int tool, double x0, size_t parent,
                      double z0 = 0., double z1 = 100.)
{
    PrintableRegion value;
    value.id            = id;
    value.kind          = RegionKind::Object;
    value.parent_object = parent;
    value.tool          = tool;
    value.min_x         = x0;
    value.max_x         = x0 + 20.;
    value.min_y         = 0.;
    value.max_y         = 20.;
    value.min_z         = z0;
    value.max_z         = z1;
    return value;
}

const DependencyComponent *component_of(const ComponentDecomposition &result, size_t region)
{
    for (const DependencyComponent &component : result.components)
        if (std::find(component.regions.begin(), component.regions.end(), region) !=
            component.regions.end())
            return &component;
    return nullptr;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Two separated towers on different tools are independent components",
          "[dependency-components]")
{
    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 100., 1) }, {}, ComponentOptions {});

    REQUIRE(result.components.size() == 2);
    CHECK(result.batching_eligible);
    CHECK(result.components[0].verdict == IndependenceVerdict::Independent);
    CHECK(result.components[0].tools == std::vector<int> { 0 });
    CHECK(result.components[1].tools == std::vector<int> { 1 });
}

TEST_CASE("Regions closer than the separation margin are not independent",
          "[dependency-components]")
{
    ComponentOptions options;
    options.separation_margin_mm = 0.5;

    // The second tower starts 0.2 mm after the first one ends.
    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 20.2, 1) }, {}, options);

    REQUIRE(result.components.size() == 1);
    CHECK_FALSE(result.batching_eligible);
    CHECK(mentions(result.components.front().reasons, "come within"));
}

TEST_CASE("A region above another is printed onto it, not beside it",
          "[dependency-components]")
{
    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0, 0., 50.), tower(1, 1, 0., 1, 50., 100.) }, {},
        ComponentOptions {});

    REQUIRE(result.components.size() == 1);
    CHECK(mentions(result.components.front().reasons, "one above the other"));
}

TEST_CASE("A support linking two towers keeps them in one component",
          "[dependency-components]")
{
    RegionLink support;
    support.a           = 0;
    support.b           = 1;
    support.kind        = LinkKind::Supports;
    support.description = "the raft under both towers is continuous";

    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 100., 1) }, { support }, ComponentOptions {});

    REQUIRE(result.components.size() == 1);
    CHECK_FALSE(result.batching_eligible);
    CHECK(mentions(result.components.front().reasons, "supports"));
    CHECK(mentions(result.components.front().reasons, "raft under both towers"));
    CHECK(mentions(result.notes, "nothing to batch between"));
}

TEST_CASE("A relationship of an unrecorded kind leaves the component unproven",
          "[dependency-components]")
{
    RegionLink vague;
    vague.a    = 0;
    vague.b    = 1;
    vague.kind = LinkKind::Unknown;

    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 100., 1) }, { vague }, ComponentOptions {});

    REQUIRE(result.components.size() == 1);
    CHECK(result.components.front().verdict == IndependenceVerdict::Unproven);
    CHECK(mentions(result.components.front().reasons, "never recorded"));
}

TEST_CASE("Looking separate is not proof of being separate",
          "[dependency-components]")
{
    PrintableRegion unexamined  = tower(1, 1, 100., 1);
    unexamined.geometry_analyzed = false;

    SECTION("unproven, and batching is not offered") {
        const ComponentDecomposition result =
            decompose_components({ tower(0, 0, 0., 0), unexamined }, {}, ComponentOptions {});

        REQUIRE(result.components.size() == 2);
        CHECK_FALSE(result.batching_eligible);
        CHECK(component_of(result, 1)->verdict == IndependenceVerdict::Unproven);
        CHECK(mentions(component_of(result, 1)->reasons, "not proof of being separate"));
        CHECK(mentions(result.notes, "could not be shown to be independent"));
    }

    SECTION("resting on the bounding box alone is possible, and stated") {
        ComponentOptions options;
        options.require_geometry_analysis = false;

        const ComponentDecomposition result =
            decompose_components({ tower(0, 0, 0., 0), unexamined }, {}, options);

        CHECK(result.batching_eligible);
        CHECK(mentions(result.notes, "rests on its bounding box alone"));
    }
}

TEST_CASE("Two disconnected regions inside one object stay together unless asked",
          "[dependency-components]")
{
    PrintableRegion left  = tower(0, 0, 0., 7);
    PrintableRegion right = tower(1, 1, 100., 7);
    left.kind             = RegionKind::PaintedRegion;
    right.kind            = RegionKind::PaintedRegion;

    SECTION("kept together by default") {
        const ComponentDecomposition result =
            decompose_components({ left, right }, {}, ComponentOptions {});
        REQUIRE(result.components.size() == 1);
        CHECK(mentions(result.components.front().reasons, "parts of the same object"));
    }

    SECTION("split when the caller enables it") {
        ComponentOptions options;
        options.allow_intra_object_split = true;

        const ComponentDecomposition result =
            decompose_components({ left, right }, {}, options);
        CHECK(result.components.size() == 2);
        CHECK(result.batching_eligible);
    }
}

TEST_CASE("Four towers on different tools give four components",
          "[dependency-components]")
{
    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 100., 1), tower(2, 2, 200., 2),
          tower(3, 3, 300., 3) },
        {}, ComponentOptions {});

    CHECK(result.components.size() == 4);
    CHECK(result.batching_eligible);
}

TEST_CASE("The same input always decomposes the same way", "[dependency-components]")
{
    const std::vector<PrintableRegion> regions {
        tower(4, 0, 0., 0), tower(1, 1, 100., 1), tower(9, 2, 200., 2)
    };

    const ComponentDecomposition first  = decompose_components(regions, {},
                                                               ComponentOptions {});
    const ComponentDecomposition second = decompose_components(regions, {},
                                                               ComponentOptions {});

    REQUIRE(first.components.size() == second.components.size());
    for (size_t index = 0; index < first.components.size(); ++index) {
        CHECK(first.components[index].id == second.components[index].id);
        CHECK(first.components[index].regions == second.components[index].regions);
    }
}

TEST_CASE("A link naming a region that is not here is reported, not ignored",
          "[dependency-components]")
{
    RegionLink dangling;
    dangling.a = 0;
    dangling.b = 42;

    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 100., 1) }, { dangling }, ComponentOptions {});

    CHECK(mentions(result.notes, "not in this print"));
}

TEST_CASE("A capped decomposition does not offer batching",
          "[dependency-components]")
{
    ComponentOptions options;
    options.max_regions = 2;

    const ComponentDecomposition result = decompose_components(
        { tower(0, 0, 0., 0), tower(1, 1, 100., 1), tower(2, 2, 200., 2) }, {}, options);

    CHECK(result.components.size() == 2);
    CHECK_FALSE(result.batching_eligible);
    CHECK(mentions(result.notes, "were not decomposed at all"));
}

TEST_CASE("Nothing to decompose is an error", "[dependency-components]")
{
    CHECK_FALSE(decompose_components({}, {}, ComponentOptions {}).error.empty());
}
