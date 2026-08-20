// Phase 19: automatic detail decomposition. This layer proposes tool-owned
// regions and never decides. The properties: an explicit colour or tool
// assignment is the user's and survives untouched unless remapping was
// enabled, an unmeasured accessibility list does not read as "every tool can
// reach it", a tie between equally suitable tools is reported rather than
// broken by index, and a move that would not repay its toolchange is not
// proposed. Boundaries become requirements handed to the synchronization
// engine, not decisions taken here.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/DetailDecomposition.hpp"

using namespace Slic3r;

namespace {

DecompositionTool tool(int index, double nozzle, double throughput)
{
    DecompositionTool value;
    value.tool               = index;
    value.nozzle_diameter_mm = nozzle;
    value.min_resolvable_mm  = nozzle;
    value.grid_height_mm     = nozzle / 2.;
    value.throughput_mm3_s   = throughput;
    value.accuracy_mm        = 0.05;
    return value;
}

std::vector<DecompositionTool> machine()
{
    // A coarse fast tool and a fine slow one.
    return { tool(0, 0.6, 20.), tool(1, 0.25, 6.) };
}

RegionGeometry region(size_t id, bool visible, double curvature)
{
    RegionGeometry value;
    value.id                  = id;
    value.visible             = visible;
    value.curvature           = curvature;
    value.min_feature_mm      = 1.2;
    value.wall_thickness_mm   = 1.2;
    value.volume_mm3          = 2000.;
    value.reachable_tools     = { 0, 1 };
    value.accessibility_known = true;
    value.current_tool        = 0;
    return value;
}

const RegionProposal *proposal_for(const DecompositionResult &result, size_t id)
{
    for (const RegionProposal &proposal : result.proposals)
        if (proposal.region == id)
            return &proposal;
    return nullptr;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Ordinary work stays on the coarsest tool that can do it",
          "[detail-decomposition]")
{
    const DecompositionResult result =
        decompose_details({ region(0, true, 0.1) }, machine(), {}, DecompositionOptions {});

    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::Unchanged);
    CHECK(*proposal->proposed_tool == 0);
    // Being visible is not by itself a reason for the finest nozzle.
    CHECK(mentions(proposal->reasons, "coarsest nozzle its requirements permit"));
}

TEST_CASE("Detail takes the finest capable nozzle", "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].min_feature_mm = 0.3;

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});

    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::Proposed);
    CHECK(*proposal->proposed_tool == 1);
    CHECK(proposal->editable);
    CHECK(mentions(proposal->reasons, "carries detail"));
}

TEST_CASE("Text counts as detail even on a flat face", "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.) };
    regions[0].carries_text   = true;
    regions[0].min_feature_mm = 0.3;

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    CHECK(*proposal_for(result, 0)->proposed_tool == 1);
}

TEST_CASE("An explicit assignment is preserved", "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].min_feature_mm = 0.3;
    regions[0].explicit_tool  = 0;

    SECTION("remapping disabled") {
        const DecompositionResult result =
            decompose_details(regions, machine(), {}, DecompositionOptions {});
        const RegionProposal *proposal = proposal_for(result, 0);
        REQUIRE(proposal != nullptr);
        CHECK(proposal->outcome == RegionOutcome::KeptExplicit);
        CHECK(*proposal->proposed_tool == 0);
        CHECK(mentions(proposal->reasons, "the assignment stands"));
    }

    SECTION("remapping enabled") {
        DecompositionOptions options;
        options.allow_color_remap = true;
        const DecompositionResult result =
            decompose_details(regions, machine(), {}, options);
        CHECK(proposal_for(result, 0)->outcome == RegionOutcome::Proposed);
    }
}

TEST_CASE("An explicit colour is preserved just as an explicit tool is",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].min_feature_mm = 0.3;
    regions[0].explicit_color = "#F6A5C1";

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::KeptExplicit);
    CHECK(mentions(proposal->reasons, "#F6A5C1"));
}

TEST_CASE("Unmeasured accessibility does not mean every tool can reach it",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].accessibility_known = false;
    regions[0].reachable_tools.clear();

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::NeedsMeasurement);
    CHECK_FALSE(proposal->proposed_tool.has_value());
}

TEST_CASE("Detail with no measured feature size cannot be assigned",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].min_feature_mm.reset();

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    CHECK(proposal_for(result, 0)->outcome == RegionOutcome::NeedsMeasurement);
}

TEST_CASE("A region no tool can produce says which tool failed and why",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].min_feature_mm = 0.1;

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::NoCapableTool);
    CHECK(mentions(proposal->reasons, "too coarse for a 0.10 mm feature"));
    CHECK(proposal->reasons.size() == 2);
}

TEST_CASE("A tool with no measured accuracy cannot be shown to meet a requirement",
          "[detail-decomposition]")
{
    std::vector<DecompositionTool> tools = machine();
    tools[1].accuracy_mm.reset();

    std::vector<RegionGeometry> regions { region(0, true, 0.9) };
    regions[0].min_feature_mm       = 0.3;
    regions[0].required_accuracy_mm = 0.08;

    const DecompositionResult result =
        decompose_details(regions, tools, {}, DecompositionOptions {});
    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::NoCapableTool);
    CHECK(mentions(proposal->reasons, "no measured accuracy"));
}

TEST_CASE("A tie between equally suitable tools is reported, not broken",
          "[detail-decomposition]")
{
    // Two identical 0.4 tools.
    const std::vector<DecompositionTool> twins { tool(0, 0.4, 12.), tool(1, 0.4, 12.) };

    const DecompositionResult result =
        decompose_details({ region(0, true, 0.1) }, twins, {}, DecompositionOptions {});

    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::Ambiguous);
    CHECK(proposal->tied_tools == std::vector<int> { 0, 1 });
    CHECK_FALSE(proposal->proposed_tool.has_value());
    CHECK(mentions(proposal->reasons, "equally suitable here; choose one"));
}

TEST_CASE("A move that does not repay its toolchange is not proposed",
          "[detail-decomposition]")
{
    // A tiny region: the throughput gain cannot cover a toolchange.
    std::vector<RegionGeometry> regions { region(0, false, 0.1) };
    regions[0].volume_mm3   = 20.;
    regions[0].current_tool = 1;

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::Unchanged);
    CHECK(*proposal->proposed_tool == 1);
    CHECK(mentions(proposal->reasons, "does not repay itself"));
}

TEST_CASE("A move that does repay its toolchange is proposed",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, false, 0.1) };
    regions[0].volume_mm3   = 5000.;
    regions[0].current_tool = 1;

    const DecompositionResult result =
        decompose_details(regions, machine(), {}, DecompositionOptions {});
    const RegionProposal *proposal = proposal_for(result, 0);
    REQUIRE(proposal != nullptr);
    CHECK(proposal->outcome == RegionOutcome::Proposed);
    CHECK(*proposal->proposed_tool == 0);
}

TEST_CASE("A boundary between differently owned regions becomes a requirement",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.1), region(1, true, 0.9) };
    regions[1].min_feature_mm = 0.3;
    regions[1].print_z        = 4.;

    const DecompositionResult result =
        decompose_details(regions, machine(), { { 0, 1 } }, DecompositionOptions {});

    REQUIRE(result.interfaces.size() == 1);
    const InterfaceRequirement &requirement = result.interfaces.front();
    CHECK(requirement.tool_a == 0);
    CHECK(requirement.tool_b == 1);
    REQUIRE(requirement.synchronization_height_mm.has_value());
    // 0.30 and 0.125 do not divide evenly, so the boundary is handed on rather
    // than settled here.
    CHECK(mentions(requirement.reasons, "needs the synchronization engine"));
}

TEST_CASE("Regions on the same tool need no interface", "[detail-decomposition]")
{
    const std::vector<RegionGeometry> regions { region(0, true, 0.1), region(1, true, 0.1) };
    const DecompositionResult         result =
        decompose_details(regions, machine(), { { 0, 1 } }, DecompositionOptions {});
    CHECK(result.interfaces.empty());
}

TEST_CASE("A boundary with an unowned side is reported, not assumed",
          "[detail-decomposition]")
{
    std::vector<RegionGeometry> regions { region(0, true, 0.1), region(1, true, 0.9) };
    regions[1].accessibility_known = false;
    regions[1].reachable_tools.clear();
    regions[1].current_tool.reset();

    const DecompositionResult result =
        decompose_details(regions, machine(), { { 0, 1 } }, DecompositionOptions {});
    CHECK(result.interfaces.empty());
    CHECK(mentions(result.notes, "one side has no owner yet"));
}

TEST_CASE("Nothing to decompose is an error, not an empty result",
          "[detail-decomposition]")
{
    CHECK_FALSE(decompose_details({}, machine(), {}, DecompositionOptions {}).error.empty());
    CHECK_FALSE(
        decompose_details({ region(0, true, 0.1) }, {}, {}, DecompositionOptions {}).error.empty());
}
