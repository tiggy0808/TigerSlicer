// Phase 22: the advanced bridge planner. A bridge is the one feature where
// being optimistic produces a visible, unrecoverable mess, so the tests hold
// the planner to refusing rather than attempting: an unanchored bridge is
// refused, an unmeasured anchor stays unproven instead of collapsing into
// "fine", a tool that cannot cool or cannot come up to pressure is rejected by
// name, and a pinned tool is never quietly substituted. The one place it is
// allowed to be clever is choosing different tools for a hidden structural
// bridge and a visible surface one.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/BridgePlanner.hpp"

using namespace Slic3r;

namespace {

BridgeAnchor anchor(double width = 6., bool solid = true, bool measured = true)
{
    BridgeAnchor value;
    value.solid    = solid;
    value.width_mm = width;
    value.measured = measured;
    return value;
}

BridgeGeometry bridge(size_t id, double span, BridgeRole role = BridgeRole::Structural)
{
    BridgeGeometry value;
    value.id              = id;
    value.role            = role;
    value.span_mm         = span;
    value.direction_deg   = 0.;
    value.layer_height_mm = 0.2;
    value.visible         = role == BridgeRole::Surface;
    value.anchors         = { anchor(), anchor() };
    return value;
}

BridgeTool tool(int index, double nozzle, double max_span, double change_cost = 0.)
{
    BridgeTool value;
    value.tool                        = index;
    value.material                    = "PLA";
    value.nozzle_mm                   = nozzle;
    value.max_bridge_span_mm          = max_span;
    value.max_flow_mm3_s              = 15.;
    value.has_part_cooling_fan        = true;
    value.pressure_can_be_established = true;
    value.change_cost_s               = change_cost;
    return value;
}

std::vector<BridgeTool> machine() { return { tool(0, 0.4, 50.), tool(1, 0.8, 120., 12.) }; }

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("An unanchored bridge is refused, not attempted", "[bridge-planner]")
{
    BridgeGeometry floating = bridge(0, 20.);
    floating.anchors        = { anchor() };

    const BridgePlan plan = plan_bridges({ floating }, machine(), BridgePolicy {});
    REQUIRE(plan.decisions.size() == 1);
    CHECK(plan.decisions.front().anchors == AnchorState::Unanchored);
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
    CHECK(mentions(plan.decisions.front().reasons, "needs two ends"));
    CHECK_FALSE(plan.decisions.front().tool.has_value());
}

TEST_CASE("An unmeasured anchor is unproven, which is not the same as fine",
          "[bridge-planner]")
{
    BridgeGeometry unchecked = bridge(0, 20.);
    unchecked.anchors        = { anchor(), anchor(6., true, false) };

    SECTION("unproven by default") {
        const BridgePlan plan = plan_bridges({ unchecked }, machine(), BridgePolicy {});
        CHECK(plan.decisions.front().anchors == AnchorState::Unknown);
        CHECK(plan.decisions.front().verdict == BridgeVerdict::Unproven);
        CHECK(mentions(plan.decisions.front().reasons, "not the same as it being fine"));
        // It still gets a tool - unproven is a caveat on a plan, not a refusal.
        CHECK(plan.decisions.front().tool.has_value());
    }

    SECTION("planned when unmeasured anchors are explicitly accepted") {
        BridgePolicy policy;
        policy.allow_unmeasured_anchors = true;

        const BridgePlan plan = plan_bridges({ unchecked }, machine(), policy);
        CHECK(plan.decisions.front().anchors == AnchorState::Anchored);
        CHECK(plan.decisions.front().verdict == BridgeVerdict::Planned);
    }
}

TEST_CASE("A bridge landing on sparse infill asks for support first",
          "[bridge-planner]")
{
    BridgeGeometry weak = bridge(0, 20.);
    weak.anchors        = { anchor(), anchor(1., true) };

    const BridgePlan plan = plan_bridges({ weak }, machine(), BridgePolicy {});
    CHECK(plan.decisions.front().anchors == AnchorState::PartiallyAnchored);
    CHECK(plan.decisions.front().verdict == BridgeVerdict::NeedsSupport);
    CHECK(mentions(plan.decisions.front().reasons, "wants support under it"));
}

TEST_CASE("A span past every tool's proven reach is refused, and says which tool",
          "[bridge-planner]")
{
    const BridgePlan plan = plan_bridges({ bridge(0, 300.) }, machine(), BridgePolicy {});
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
    CHECK(mentions(plan.decisions.front().rejected_tools, "T0 has been shown to bridge"));
    CHECK(mentions(plan.decisions.front().rejected_tools, "T1 has been shown to bridge"));
    CHECK(mentions(plan.decisions.front().reasons, "no tool on this machine"));
}

TEST_CASE("A tool whose reach was never measured is used but not called proven",
          "[bridge-planner]")
{
    std::vector<BridgeTool> uncharacterized { tool(0, 0.4, 50.) };
    uncharacterized.front().max_bridge_span_mm.reset();

    const BridgePlan plan = plan_bridges({ bridge(0, 20.) }, uncharacterized, BridgePolicy {});
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Unproven);
    REQUIRE(plan.decisions.front().tool.has_value());
    CHECK(*plan.decisions.front().tool == 0);
    CHECK(mentions(plan.decisions.front().reasons, "never had its maximum bridge span"));
}

TEST_CASE("A tool that cannot cool or cannot reach pressure is rejected by name",
          "[bridge-planner]")
{
    SECTION("no part cooling fan") {
        std::vector<BridgeTool> uncooled { tool(0, 0.4, 50.) };
        uncooled.front().has_part_cooling_fan = false;

        const BridgePlan plan = plan_bridges({ bridge(0, 20.) }, uncooled, BridgePolicy {});
        CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
        CHECK(mentions(plan.decisions.front().rejected_tools, "no part cooling fan"));
    }

    SECTION("pressure cannot be established before the bridge starts") {
        std::vector<BridgeTool> soft { tool(0, 0.4, 50.) };
        soft.front().pressure_can_be_established = false;

        const BridgePlan plan = plan_bridges({ bridge(0, 20.) }, soft, BridgePolicy {});
        CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
        CHECK(mentions(plan.decisions.front().rejected_tools, "begins under-pressured"));
    }
}

TEST_CASE("A bridge is never slowed down to fit inside a flow ceiling",
          "[bridge-planner]")
{
    std::vector<BridgeTool> restricted { tool(0, 0.4, 50.) };
    restricted.front().max_flow_mm3_s = 1.;

    const BridgePlan plan = plan_bridges({ bridge(0, 20.) }, restricted, BridgePolicy {});
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
    CHECK(mentions(plan.decisions.front().rejected_tools, "can only deliver"));
}

TEST_CASE("A layer height outside a tool's range rejects that tool",
          "[bridge-planner]")
{
    std::vector<BridgeTool> coarse { tool(0, 0.8, 120.) };
    coarse.front().min_layer_height_mm = 0.3;

    const BridgePlan plan = plan_bridges({ bridge(0, 20.) }, coarse, BridgePolicy {});
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
    CHECK(mentions(plan.decisions.front().rejected_tools, "as thin as"));
}

TEST_CASE("A visible bridge takes the finer nozzle, a hidden one the longer reach",
          "[bridge-planner]")
{
    const BridgePlan seen =
        plan_bridges({ bridge(0, 30., BridgeRole::Surface) }, machine(), BridgePolicy {});
    REQUIRE(seen.decisions.front().tool.has_value());
    CHECK(*seen.decisions.front().tool == 0);
    CHECK(mentions(seen.decisions.front().reasons, "the user will see"));

    const BridgePlan hidden =
        plan_bridges({ bridge(0, 30., BridgeRole::Structural) }, machine(), BridgePolicy {});
    REQUIRE(hidden.decisions.front().tool.has_value());
    CHECK(*hidden.decisions.front().tool == 1);
    CHECK(mentions(hidden.decisions.front().reasons, "hidden inside the part"));
}

TEST_CASE("A hidden and a visible bridge may use different tools when the order allows",
          "[bridge-planner]")
{
    BridgeGeometry structural = bridge(0, 30., BridgeRole::Structural);
    BridgeGeometry surface    = bridge(1, 30., BridgeRole::Surface);
    surface.depends_on        = { 0 };

    SECTION("split allowed") {
        const BridgePlan plan =
            plan_bridges({ structural, surface }, machine(), BridgePolicy {});
        REQUIRE(plan.decisions.size() == 2);
        CHECK(*plan.decisions[0].tool == 1);
        CHECK(*plan.decisions[1].tool == 0);
        CHECK(plan.decisions[1].synchronized);
        REQUIRE(plan.decisions[1].change_cost_s.has_value());
        CHECK(mentions(plan.decisions[1].reasons, "leaves room for the toolchange"));
    }

    SECTION("split refused keeps the dependant on its parent's tool") {
        BridgePolicy policy;
        policy.allow_split_tools = false;

        const BridgePlan plan = plan_bridges({ structural, surface }, machine(), policy);
        CHECK(*plan.decisions[0].tool == 1);
        CHECK(*plan.decisions[1].tool == 1);
        CHECK_FALSE(plan.decisions[1].change_cost_s.has_value());
    }
}

TEST_CASE("A bridge landing on a refused bridge is refused too", "[bridge-planner]")
{
    BridgeGeometry floating = bridge(0, 20.);
    floating.anchors        = { anchor() };

    BridgeGeometry above = bridge(1, 20.);
    above.depends_on     = { 0 };

    const BridgePlan plan = plan_bridges({ floating, above }, machine(), BridgePolicy {});
    REQUIRE(plan.decisions.size() == 2);
    CHECK(plan.decisions[1].verdict == BridgeVerdict::Refused);
    CHECK_FALSE(plan.decisions[1].synchronized);
    CHECK(mentions(plan.decisions[1].reasons, "nothing for it to land on"));
}

TEST_CASE("A dependency that has not been planned yet leaves the order unproven",
          "[bridge-planner]")
{
    BridgeGeometry above = bridge(0, 20.);
    above.depends_on     = { 9 };

    const BridgePlan plan = plan_bridges({ above }, machine(), BridgePolicy {});
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Unproven);
    CHECK_FALSE(plan.decisions.front().synchronized);
    CHECK(mentions(plan.decisions.front().reasons, "order could not be checked"));
}

TEST_CASE("A pinned tool that cannot bridge is refused, never substituted",
          "[bridge-planner]")
{
    BridgeGeometry pinned = bridge(0, 80.);
    pinned.required_tool  = 0; // T0 is only proven to 50 mm; T1 could do it.

    const BridgePlan plan = plan_bridges({ pinned }, machine(), BridgePolicy {});
    CHECK(plan.decisions.front().verdict == BridgeVerdict::Refused);
    CHECK_FALSE(plan.decisions.front().tool.has_value());
    CHECK(mentions(plan.decisions.front().reasons, "no other tool was substituted"));
}

TEST_CASE("Two tools that score identically are reported as a tie",
          "[bridge-planner]")
{
    const std::vector<BridgeTool> twins { tool(0, 0.4, 50.), tool(1, 0.4, 50.) };

    const BridgePlan plan =
        plan_bridges({ bridge(0, 20., BridgeRole::Surface) }, twins, BridgePolicy {});
    CHECK(mentions(plan.decisions.front().reasons, "scored identically"));
}

TEST_CASE("The total is only reported when every bridge could be priced",
          "[bridge-planner]")
{
    SECTION("all planned") {
        const BridgePlan plan =
            plan_bridges({ bridge(0, 30.), bridge(1, 60.) }, machine(), BridgePolicy {});
        REQUIRE(plan.total_time_s.has_value());
        CHECK(*plan.total_time_s == Catch::Approx((30. + 60.) / 60.));
    }

    SECTION("one refused withholds the total") {
        BridgeGeometry floating = bridge(1, 20.);
        floating.anchors        = { anchor() };

        const BridgePlan plan =
            plan_bridges({ bridge(0, 30.), floating }, machine(), BridgePolicy {});
        CHECK_FALSE(plan.total_time_s.has_value());
        CHECK(mentions(plan.notes, "no total bridging time is reported"));
    }
}

TEST_CASE("A capped plan says it stopped rather than looking complete",
          "[bridge-planner]")
{
    BridgePolicy policy;
    policy.max_bridges = 2;

    std::vector<BridgeGeometry> many;
    for (size_t index = 0; index < 5; ++index)
        many.push_back(bridge(index, 20.));

    const BridgePlan plan = plan_bridges(many, machine(), policy);
    CHECK(plan.decisions.size() == 2);
    CHECK(mentions(plan.notes, "the rest were not examined at all"));
}

TEST_CASE("Nothing to bridge and nothing to bridge with are both errors",
          "[bridge-planner]")
{
    CHECK_FALSE(plan_bridges({}, machine(), BridgePolicy {}).error.empty());
    CHECK_FALSE(plan_bridges({ bridge(0, 20.) }, {}, BridgePolicy {}).error.empty());
}
