// Phase 9S: the multi-material support interface engine, supplementing Phase 9's
// mixed-nozzle support planning with what the support is made of and how it
// comes off. The two properties worth most here are that an unmeasured
// material pair stays unknown instead of being assumed compatible, and that a support which cannot be got out of
// the part is refused before it is printed rather than discovered afterwards
// by whoever has to hold the part. Everything else - contact distance,
// interface layers, removal outcome, toolchange cost - follows from the
// strategy that was actually chosen, and any fall-back says it fell back.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/SupportInterface.hpp"

using namespace Slic3r;

namespace {

SupportTool part_tool()
{
    SupportTool value;
    value.tool          = 0;
    value.material      = "PLA";
    value.change_cost_s = 0.;
    return value;
}

SupportTool soluble_tool()
{
    SupportTool value;
    value.tool          = 1;
    value.material      = "PVA";
    value.soluble       = true;
    value.solvent       = "water";
    value.change_cost_s = 20.;
    return value;
}

SupportTool breakaway_tool()
{
    SupportTool value;
    value.tool          = 2;
    value.material      = "PETG";
    value.change_cost_s = 15.;
    return value;
}

std::vector<SupportTool> machine()
{
    return { part_tool(), soluble_tool(), breakaway_tool() };
}

MaterialPairing pairing(const std::string &a, const std::string &b,
                        std::optional<double> adhesion)
{
    MaterialPairing value;
    value.a        = a;
    value.b        = b;
    value.adhesion = adhesion;
    return value;
}

std::vector<MaterialPairing> measured()
{
    return { pairing("PLA", "PVA", 0.50), pairing("PLA", "PETG", 0.35) };
}

SupportedRegion region(size_t id = 0, double overhang = 45.)
{
    SupportedRegion value;
    value.id                 = id;
    value.part_material      = "PLA";
    value.part_tool          = 0;
    value.area_mm2           = 400.;
    value.overhang_angle_deg = overhang;
    value.layer_height_mm    = 0.2;
    return value;
}

bool mentions(const std::vector<std::string> &lines, const std::string &fragment)
{
    return std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(fragment) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Adhesion is looked up in either order", "[support-interface]")
{
    const std::vector<MaterialPairing> table = measured();
    REQUIRE(find_pairing(table, "PLA", "PVA") != nullptr);
    REQUIRE(find_pairing(table, "PVA", "PLA") != nullptr);
    CHECK(find_pairing(table, "PVA", "PLA")->adhesion == Catch::Approx(0.50));
    CHECK(find_pairing(table, "PLA", "Nylon") == nullptr);
}

TEST_CASE("A soluble interface touches the part and dissolves", "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Soluble;

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, machine(), measured(), policy);

    REQUIRE(plan.decisions.size() == 1);
    const InterfaceDecision &decision = plan.decisions.front();
    CHECK(decision.verdict == InterfaceVerdict::Planned);
    CHECK(decision.strategy == SupportStrategy::Soluble);
    REQUIRE(decision.interface_tool.has_value());
    CHECK(*decision.interface_tool == 1);
    REQUIRE(decision.contact_distance_mm.has_value());
    CHECK(*decision.contact_distance_mm == Catch::Approx(0.));
    CHECK(decision.removal == RemovalOutcome::Dissolves);
    CHECK(mentions(decision.reasons, "removed in water"));
}

TEST_CASE("A soluble material with no recorded solvent is unproven",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Soluble;

    std::vector<SupportTool> tools = machine();
    tools[1].solvent.clear();

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, tools, measured(), policy);
    CHECK(plan.decisions.front().verdict == InterfaceVerdict::Unproven);
    CHECK(mentions(plan.decisions.front().reasons, "no solvent is recorded"));
}

TEST_CASE("Falling back from soluble to mechanical says it fell back",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Soluble;

    const std::vector<SupportTool> tools { part_tool(), breakaway_tool() };

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, tools, measured(), policy);
    CHECK(plan.decisions.front().verdict == InterfaceVerdict::Compromise);
    CHECK(plan.decisions.front().strategy == SupportStrategy::Breakaway);
    CHECK(mentions(plan.decisions.front().reasons, "no soluble material is loaded"));
}

TEST_CASE("A breakaway interface keeps a gap and peels off", "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Breakaway;

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, machine(), measured(), policy);

    const InterfaceDecision &decision = plan.decisions.front();
    CHECK(decision.verdict == InterfaceVerdict::Planned);
    // PETG holds and releases, and is the cheaper change of the two candidates.
    REQUIRE(decision.interface_tool.has_value());
    CHECK(*decision.interface_tool == 2);
    CHECK(decision.adhesion == AdhesionClass::Removable);
    CHECK(decision.removal == RemovalOutcome::PeelsAway);
    REQUIRE(decision.contact_distance_mm.has_value());
    CHECK(*decision.contact_distance_mm == Catch::Approx(0.2));
}

TEST_CASE("An unmeasured material pair stays unknown, not compatible",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Breakaway;

    const std::vector<SupportTool> tools { part_tool(), breakaway_tool() };

    SECTION("rejected by default, and the fall-back is reported") {
        const SupportInterfacePlan plan =
            plan_support_interfaces({ region() }, tools, {}, policy);
        const InterfaceDecision &decision = plan.decisions.front();
        CHECK(decision.verdict == InterfaceVerdict::Compromise);
        CHECK(decision.strategy == SupportStrategy::SameMaterial);
        CHECK(mentions(decision.rejected_tools, "assumed compatible"));
    }

    SECTION("used only when unmeasured pairs are explicitly permitted") {
        policy.allow_unmeasured_pairs = true;

        const SupportInterfacePlan plan =
            plan_support_interfaces({ region() }, tools, {}, policy);
        const InterfaceDecision &decision = plan.decisions.front();
        CHECK(decision.verdict == InterfaceVerdict::Unproven);
        CHECK(*decision.interface_tool == 2);
        CHECK(decision.adhesion == AdhesionClass::Unknown);
        CHECK(decision.removal == RemovalOutcome::Unknown);
        CHECK(mentions(decision.reasons, "not known whether the support will hold"));
    }
}

TEST_CASE("A material that will not hold and one that will weld are both rejected by name",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Breakaway;

    const std::vector<SupportTool> tools { part_tool(), breakaway_tool() };

    SECTION("too weak to stay attached") {
        const SupportInterfacePlan plan = plan_support_interfaces(
            { region() }, tools, { pairing("PLA", "PETG", 0.02) }, policy);
        CHECK(mentions(plan.decisions.front().rejected_tools, "come loose while it printed"));
        CHECK(plan.decisions.front().strategy == SupportStrategy::SameMaterial);
    }

    SECTION("welds to the part") {
        const SupportInterfacePlan plan = plan_support_interfaces(
            { region() }, tools, { pairing("PLA", "PETG", 0.95) }, policy);
        CHECK(mentions(plan.decisions.front().rejected_tools, "weld to the part"));
    }
}

TEST_CASE("Same material welds to itself, so the gap is all there is",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::SameMaterial;

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, machine(), measured(), policy);

    const InterfaceDecision &decision = plan.decisions.front();
    CHECK(decision.strategy == SupportStrategy::SameMaterial);
    CHECK(*decision.interface_tool == 0);
    CHECK(decision.adhesion == AdhesionClass::Bonded);
    CHECK(decision.removal == RemovalOutcome::RequiresForce);
    CHECK(mentions(decision.reasons, "relies entirely on the"));
    CHECK_FALSE(decision.change_cost_s.has_value());
}

TEST_CASE("A support sealed in a cavity with no solvent is refused before printing",
          "[support-interface]")
{
    SupportedRegion cavity = region();
    cavity.enclosed        = true;

    const std::vector<SupportTool> tools { part_tool(), breakaway_tool() };

    SECTION("refused by default") {
        const SupportInterfacePlan plan =
            plan_support_interfaces({ cavity }, tools, measured(), SupportPolicy {});
        const InterfaceDecision &decision = plan.decisions.front();
        CHECK(decision.verdict == InterfaceVerdict::Refused);
        CHECK(decision.removal == RemovalOutcome::NotRemovable);
        CHECK(mentions(decision.reasons, "trapped in the part forever"));
        CHECK_FALSE(plan.total_change_cost_s.has_value());
        CHECK(mentions(plan.notes, "no total toolchange cost is reported"));
    }

    SECTION("a soluble material makes the cavity printable") {
        const SupportInterfacePlan plan = plan_support_interfaces(
            { cavity }, machine(), measured(), SupportPolicy {});
        const InterfaceDecision &decision = plan.decisions.front();
        CHECK(decision.verdict == InterfaceVerdict::Planned);
        CHECK(decision.strategy == SupportStrategy::Soluble);
        CHECK(decision.removal == RemovalOutcome::Dissolves);
    }

    SECTION("trapping it anyway is possible but never silent") {
        SupportPolicy policy;
        policy.allow_trapped_support = true;

        const SupportInterfacePlan plan =
            plan_support_interfaces({ cavity }, tools, measured(), policy);
        const InterfaceDecision &decision = plan.decisions.front();
        CHECK(decision.verdict == InterfaceVerdict::Compromise);
        CHECK(decision.removal == RemovalOutcome::NotRemovable);
        CHECK(mentions(decision.reasons, "cannot be removed"));
    }
}

TEST_CASE("A tool that is not loaded is rejected by name", "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Soluble;

    std::vector<SupportTool> tools = machine();
    tools[1].loaded                = false;

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, tools, measured(), policy);
    CHECK(mentions(plan.decisions.front().rejected_tools, "is not loaded"));
    CHECK(*plan.decisions.front().interface_tool == 2);
}

TEST_CASE("A near-horizontal overhang gets an extra interface layer",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred           = SupportStrategy::Breakaway;
    policy.min_interface_layers = 2;

    const SupportInterfacePlan steep =
        plan_support_interfaces({ region(0, 60.) }, machine(), measured(), policy);
    CHECK(steep.decisions.front().interface_layers == 2);

    const SupportInterfacePlan ceiling =
        plan_support_interfaces({ region(0, 5.) }, machine(), measured(), policy);
    CHECK(ceiling.decisions.front().interface_layers == 3);
    CHECK(mentions(ceiling.decisions.front().reasons, "rests on the interface"));
}

TEST_CASE("A visible contact surface is warned about when the support is mechanical",
          "[support-interface]")
{
    SupportedRegion shown       = region();
    shown.visible_after_removal = true;

    SupportPolicy mechanical;
    mechanical.preferred = SupportStrategy::Breakaway;

    const SupportInterfacePlan marked =
        plan_support_interfaces({ shown }, machine(), measured(), mechanical);
    CHECK(mentions(marked.decisions.front().reasons, "leave a mark on it"));

    SupportPolicy dissolved;
    dissolved.preferred = SupportStrategy::Soluble;

    const SupportInterfacePlan clean =
        plan_support_interfaces({ shown }, machine(), measured(), dissolved);
    CHECK_FALSE(mentions(clean.decisions.front().reasons, "leave a mark on it"));
}

TEST_CASE("The toolchange cost is charged only when the tool actually changes",
          "[support-interface]")
{
    SupportPolicy soluble;
    soluble.preferred = SupportStrategy::Soluble;

    const SupportInterfacePlan changed =
        plan_support_interfaces({ region() }, machine(), measured(), soluble);
    REQUIRE(changed.decisions.front().change_cost_s.has_value());
    CHECK(*changed.decisions.front().change_cost_s == Catch::Approx(20.));
    REQUIRE(changed.total_change_cost_s.has_value());
    CHECK(*changed.total_change_cost_s == Catch::Approx(20.));

    SupportPolicy same;
    same.preferred = SupportStrategy::SameMaterial;

    const SupportInterfacePlan unchanged =
        plan_support_interfaces({ region() }, machine(), measured(), same);
    REQUIRE(unchanged.total_change_cost_s.has_value());
    CHECK(*unchanged.total_change_cost_s == Catch::Approx(0.));
}

TEST_CASE("A fall-back to a part tool that is not available is refused",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.preferred = SupportStrategy::Breakaway;

    // The part's own tool is not on the machine, and nothing breaks away.
    const std::vector<SupportTool> tools { breakaway_tool() };

    const SupportInterfacePlan plan =
        plan_support_interfaces({ region() }, tools, {}, policy);
    CHECK(plan.decisions.front().verdict == InterfaceVerdict::Refused);
    CHECK(mentions(plan.decisions.front().reasons, "which is not available"));
}

TEST_CASE("A capped plan says it stopped rather than looking complete",
          "[support-interface]")
{
    SupportPolicy policy;
    policy.max_regions = 2;

    std::vector<SupportedRegion> many;
    for (size_t index = 0; index < 5; ++index)
        many.push_back(region(index));

    const SupportInterfacePlan plan =
        plan_support_interfaces(many, machine(), measured(), policy);
    CHECK(plan.decisions.size() == 2);
    CHECK(mentions(plan.notes, "the rest were not examined at all"));
}

TEST_CASE("Nothing to support and nothing to support it with are both errors",
          "[support-interface]")
{
    CHECK_FALSE(plan_support_interfaces({}, machine(), measured(), SupportPolicy {})
                    .error.empty());
    CHECK_FALSE(plan_support_interfaces({ region() }, {}, measured(), SupportPolicy {})
                    .error.empty());
}
