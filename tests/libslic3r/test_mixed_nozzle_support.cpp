// Phase 9: mixed-nozzle support. The central property is that each part is
// generated with its assigned tool's real nozzle and width - supports are
// never narrowed to the finest nozzle on the machine - and that every
// constraint which fails names the part responsible instead of silently
// producing support that cannot be printed or removed.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/MixedNozzleSupport.hpp"

using namespace Slic3r;

namespace {

SupportToolProfile profile(int tool, double nozzle, const std::string &material = "PLA",
                           bool soluble = false)
{
    SupportToolProfile value;
    value.tool                = tool;
    value.nozzle_diameter_mm  = nozzle;
    value.extrusion_width_mm  = nozzle * 1.05;
    value.max_flow_mm3_s      = nozzle * 30.;
    value.min_layer_height_mm = nozzle * 0.1;
    value.max_layer_height_mm = nozzle * 0.75;
    value.material            = material;
    value.soluble             = soluble;
    return value;
}

// A coarse body tool, a fine interface tool, and a soluble option.
std::vector<SupportToolProfile> machine()
{
    return { profile(0, 0.4), profile(1, 0.25), profile(2, 0.8),
             profile(3, 0.4, "PVA", true) };
}

SupportRequest request()
{
    SupportRequest value;
    value.body_tool                = 2;    // the 0.8 nozzle
    value.interface_tool           = 1;    // the 0.25 nozzle
    value.body_layer_height_mm     = 0.4;
    value.interface_layer_height_mm = 0.1;
    value.contact_distance_mm      = 0.2;
    value.supported_area_mm2       = 400.;
    value.minimum_contact_width_mm = 1.0;
    value.minimum_clearance_mm     = 2.0;
    return value;
}

bool has_blocked(const SupportPlan &plan, const std::string &fragment)
{
    return std::any_of(plan.issues.begin(), plan.issues.end(),
                       [&](const SupportIssue &issue) {
                           return issue.severity == SupportIssueSeverity::Blocked &&
                                  issue.message.find(fragment) != std::string::npos;
                       });
}

bool has_message(const SupportPlan &plan, const std::string &fragment)
{
    return std::any_of(plan.issues.begin(), plan.issues.end(),
                       [&](const SupportIssue &issue) {
                           return issue.message.find(fragment) != std::string::npos;
                       });
}

} // namespace

TEST_CASE("Support is never forced to the finest nozzle on the machine",
          "[mixed-support]")
{
    const SupportPlan plan = plan_mixed_nozzle_support(request(), machine());
    REQUIRE(plan.is_valid());

    const SupportPartPlan *body = plan.part(SupportPart::Body);
    REQUIRE(body != nullptr);
    REQUIRE(body->generated);
    // The body keeps its own 0.8 nozzle even though a 0.25 exists and is in
    // use on the same support.
    CHECK(body->nozzle_diameter_mm == Catch::Approx(0.8));
    CHECK(body->extrusion_width_mm == Catch::Approx(0.84));

    const SupportPartPlan *interface_ = plan.part(SupportPart::Interface);
    REQUIRE(interface_ != nullptr);
    CHECK(interface_->nozzle_diameter_mm == Catch::Approx(0.25));
}

TEST_CASE("Body and interface keep independent layer heights", "[mixed-support]")
{
    const SupportPlan plan = plan_mixed_nozzle_support(request(), machine());
    REQUIRE(plan.is_valid());

    CHECK(plan.part(SupportPart::Body)->layer_height_mm == Catch::Approx(0.4));
    CHECK(plan.part(SupportPart::Interface)->layer_height_mm == Catch::Approx(0.1));
    // Entering and leaving the interface tool costs two changes per band.
    CHECK(plan.toolchanges_per_interface_band == 2);
}

TEST_CASE("Heights that share no plane have no valid transition", "[mixed-support]")
{
    SupportRequest bad = request();
    bad.interface_layer_height_mm = 0.15;   // 0.4 is not a whole multiple of 0.15

    const SupportPlan plan = plan_mixed_nozzle_support(bad, machine());
    CHECK_FALSE(plan.is_valid());
    CHECK(has_blocked(plan, "share no common plane"));
}

TEST_CASE("A layer height outside the assigned tool's range is blocked",
          "[mixed-support]")
{
    SupportRequest bad = request();
    bad.interface_layer_height_mm = 0.01;   // below the 0.25 nozzle's minimum

    const SupportPlan plan = plan_mixed_nozzle_support(bad, machine());
    CHECK_FALSE(plan.is_valid());
    CHECK(has_blocked(plan, "its minimum is"));
}

TEST_CASE("A zero contact distance requires a soluble interface", "[mixed-support]")
{
    SupportRequest fused = request();
    fused.contact_distance_mm = 0.;

    SECTION("a non-soluble interface fuses the support to the part") {
        const SupportPlan plan = plan_mixed_nozzle_support(fused, machine());
        CHECK_FALSE(plan.is_valid());
        CHECK(has_blocked(plan, "requires a soluble interface"));
    }

    SECTION("a soluble interface makes it legitimate") {
        fused.interface_tool            = 3;     // PVA
        fused.interface_layer_height_mm = 0.2;   // 0.4 / 0.2 synchronizes
        const SupportPlan plan = plan_mixed_nozzle_support(fused, machine());
        CHECK(plan.is_valid());
    }
}

TEST_CASE("A soluble interface with a gap is offered the better surface",
          "[mixed-support]")
{
    SupportRequest soluble = request();
    soluble.interface_tool            = 3;
    soluble.interface_layer_height_mm = 0.2;

    const SupportPlan plan = plan_mixed_nozzle_support(soluble, machine());
    REQUIRE(plan.is_valid());
    CHECK(has_message(plan, "could be closed for a better surface"));
}

TEST_CASE("A nozzle that does not fit the clearance is blocked", "[mixed-support]")
{
    SupportRequest tight = request();
    tight.minimum_clearance_mm = 0.5;   // the 0.8 body nozzle cannot reach in

    const SupportPlan plan = plan_mixed_nozzle_support(tight, machine());
    CHECK_FALSE(plan.is_valid());
    CHECK(has_blocked(plan, "does not fit the"));
}

TEST_CASE("An unchecked clearance is stated, not assumed fine", "[mixed-support]")
{
    SupportRequest unchecked = request();
    unchecked.minimum_clearance_mm.reset();

    const SupportPlan plan = plan_mixed_nozzle_support(unchecked, machine());
    CHECK(plan.is_valid());
    CHECK(has_message(plan, "collision could not be checked"));
}

TEST_CASE("An interface too coarse for the contact it makes warns",
          "[mixed-support]")
{
    SupportRequest fine = request();
    fine.minimum_contact_width_mm = 0.1;   // finer than the 0.25 interface extrudes

    const SupportPlan plan = plan_mixed_nozzle_support(fine, machine());
    CHECK(plan.is_valid());   // a warning, not a block
    CHECK(has_message(plan, "wider than the"));
}

TEST_CASE("A required finished surface needs an interface", "[mixed-support]")
{
    SupportRequest surface = request();
    surface.surface_support_required = true;

    SECTION("with a finer interface it is satisfied") {
        const SupportPlan plan = plan_mixed_nozzle_support(surface, machine());
        CHECK(plan.is_valid());
    }

    SECTION("with no interface at all it is blocked") {
        surface.interface_tool.reset();
        surface.interface_layer_height_mm = 0.;
        const SupportPlan plan = plan_mixed_nozzle_support(surface, machine());
        CHECK_FALSE(plan.is_valid());
        CHECK(has_blocked(plan, "no interface is generated"));
    }

    SECTION("an interface no finer than the body is unlikely to help") {
        surface.interface_tool            = 2;   // the same 0.8 nozzle as the body
        surface.interface_layer_height_mm = 0.4;
        const SupportPlan plan = plan_mixed_nozzle_support(surface, machine());
        CHECK(has_message(plan, "no finer than the body"));
    }
}

TEST_CASE("A material transition between body and interface is costed",
          "[mixed-support]")
{
    SupportRequest soluble = request();
    soluble.interface_tool            = 3;   // PVA against the body's PLA
    soluble.interface_layer_height_mm = 0.2;

    const SupportPlan plan = plan_mixed_nozzle_support(soluble, machine());
    REQUIRE(plan.is_valid());
    CHECK(has_message(plan, "PLA -> PVA"));
}

TEST_CASE("An unreported material leaves the transition unchecked, and says so",
          "[mixed-support]")
{
    std::vector<SupportToolProfile> tools = machine();
    tools[1].material.clear();

    const SupportPlan plan = plan_mixed_nozzle_support(request(), tools);
    CHECK(has_message(plan, "could not be checked"));
}

TEST_CASE("A raft that does not synchronize with the body is blocked",
          "[mixed-support]")
{
    SupportRequest with_raft = request();
    with_raft.raft_tool             = 0;
    with_raft.raft_layer_height_mm  = 0.3;   // 0.4 is not a whole multiple of 0.3

    const SupportPlan plan = plan_mixed_nozzle_support(with_raft, machine());
    CHECK_FALSE(plan.is_valid());
    CHECK(has_blocked(plan, "the raft at"));
}

TEST_CASE("A raft on its own tool and height is generated", "[mixed-support]")
{
    SupportRequest with_raft = request();
    with_raft.raft_tool            = 0;
    with_raft.raft_layer_height_mm = 0.2;

    const SupportPlan plan = plan_mixed_nozzle_support(with_raft, machine());
    REQUIRE(plan.is_valid());
    const SupportPartPlan *raft = plan.part(SupportPart::Raft);
    REQUIRE(raft != nullptr);
    CHECK(raft->generated);
    CHECK(raft->tool == 0);
    CHECK(raft->layer_height_mm == Catch::Approx(0.2));
}

TEST_CASE("Organic supports warn when the nozzle cannot form a tip",
          "[mixed-support]")
{
    SupportRequest organic = request();
    organic.style                 = SupportStyle::Organic;
    organic.minimum_clearance_mm  = 1.0;   // the 0.8 nozzle is over half of it

    const SupportPlan plan = plan_mixed_nozzle_support(organic, machine());
    CHECK(has_message(plan, "cannot form"));
}

TEST_CASE("A part assigned to a tool with no profile is blocked",
          "[mixed-support]")
{
    SupportRequest missing = request();
    missing.body_tool = 9;

    const SupportPlan plan = plan_mixed_nozzle_support(missing, machine());
    CHECK_FALSE(plan.is_valid());
    CHECK(has_blocked(plan, "which has no profile"));
}

TEST_CASE("Nothing to plan is refused", "[mixed-support]")
{
    SECTION("no tools") {
        const SupportPlan plan = plan_mixed_nozzle_support(request(), {});
        CHECK(plan.error.find("no tool profiles") != std::string::npos);
    }

    SECTION("no part assigned") {
        SupportRequest empty;
        const SupportPlan plan = plan_mixed_nozzle_support(empty, machine());
        CHECK(plan.error.find("no support part") != std::string::npos);
    }
}
