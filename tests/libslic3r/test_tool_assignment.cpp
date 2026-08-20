// Phase 5: automatic nozzle/tool assignment. The tests concentrate on the
// boundary between proposing and deciding: manual ownership stays
// authoritative, an unmeasured property produces a request for measurement
// rather than a default, a tie is reported rather than broken by tool index,
// and nothing here can move geometry to another tool on its own.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/ToolAssignment.hpp"

using namespace Slic3r;

namespace {

ToolCapability tool(int id, double nozzle, double flow = 15., const std::string &material = "PLA")
{
    ToolCapability capability;
    capability.tool                = id;
    capability.nozzle_diameter_mm  = nozzle;
    capability.min_layer_height_mm = nozzle * 0.1;
    capability.max_layer_height_mm = nozzle * 0.75;
    capability.max_flow_mm3_s      = flow;
    capability.material            = material;
    return capability;
}

// A four-tool toolchanger: two ordinary 0.4 nozzles, one fine, one coarse.
// The tool indices carry no meaning; the rules only ever read the nozzle.
std::vector<ToolCapability> four_tool_profile()
{
    return { tool(0, 0.4, 15.), tool(1, 0.25, 8.), tool(2, 0.6, 24.), tool(3, 0.8, 30.) };
}

FeatureAssignmentState feature(size_t id,
                               MultiRateFeature type,
                               Visibility visibility,
                               int current_tool = 0)
{
    FeatureAssignmentState state;
    state.geometry.id         = id;
    state.geometry.feature    = type;
    state.geometry.visibility = visibility;
    state.current_tool        = current_tool;
    return state;
}

const ToolProposal &only(const std::vector<ToolProposal> &proposals)
{
    REQUIRE(proposals.size() == 1);
    return proposals.front();
}

} // namespace

TEST_CASE("Visible detail goes to the finest capable nozzle", "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::Walls, Visibility::Visible);
    state.geometry.carries_text_or_detail   = true;
    state.geometry.minimum_feature_width_mm = 0.3;

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));

    // 0.25 is the only nozzle that resolves a 0.3 mm detail at all.
    CHECK(proposal.outcome == AssignmentOutcome::Proposed);
    CHECK(proposal.proposed_tool == 1);
    CHECK_FALSE(proposal.reasons.empty());
}

TEST_CASE("Hidden bulk goes to the coarsest capable nozzle", "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::SparseInfill, Visibility::Hidden);
    state.geometry.layer_height_mm = 0.2;

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));

    CHECK(proposal.outcome == AssignmentOutcome::Proposed);
    CHECK(proposal.proposed_tool == 3);   // 0.8, the widest that can print 0.2 mm layers
}

TEST_CASE("Ordinary visible work stays on a mid-range nozzle", "[tool-assignment]")
{
    // No detail is declared, so the fine nozzle is not warranted: sending every
    // visible wall to the smallest nozzle would be slow without being better.
    // The measured 0.45 mm requirement rules out the 0.6 and 0.8 nozzles, which
    // leaves the ordinary 0.4.
    FeatureAssignmentState state = feature(0, MultiRateFeature::Walls, Visibility::Visible);
    state.geometry.minimum_feature_width_mm = 0.45;

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));

    CHECK(proposal.outcome == AssignmentOutcome::Unchanged);
    CHECK(proposal.proposed_tool == 0);
}

TEST_CASE("A manual assignment is authoritative", "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::SparseInfill, Visibility::Hidden, 1);
    state.manually_assigned        = true;
    state.geometry.layer_height_mm = 0.2;

    SECTION("remapping disabled") {
        const ToolProposal proposal =
            only(propose_tool_assignments({ state }, four_tool_profile(), {}));
        CHECK(proposal.outcome == AssignmentOutcome::KeptManual);
        CHECK(proposal.proposed_tool == 1);
        CHECK_FALSE(proposal.changes_assignment());
        CHECK(proposal.reasons.front().find("manually") != std::string::npos);
    }

    SECTION("remapping explicitly enabled still asks first") {
        AssignmentOptions options;
        options.allow_remapping_manual = true;
        const ToolProposal proposal =
            only(propose_tool_assignments({ state }, four_tool_profile(), options));
        CHECK(proposal.outcome == AssignmentOutcome::Proposed);
        CHECK(proposal.proposed_tool == 3);
        // Enabling remapping lets the proposal exist. It does not let it apply
        // unseen.
        CHECK(proposal.requires_confirmation);
    }
}

TEST_CASE("An unmeasured property is reported, not defaulted", "[tool-assignment]")
{
    // Text detail whose finest feature was never measured. Choosing a nozzle
    // here would be a guess presented as a decision.
    FeatureAssignmentState state = feature(0, MultiRateFeature::Walls, Visibility::Visible);
    state.geometry.carries_text_or_detail = true;

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));

    CHECK(proposal.outcome == AssignmentOutcome::NeedsMeasurement);
    CHECK(proposal.proposed_tool == proposal.current_tool);
    CHECK(proposal.reasons.front().find("never measured") != std::string::npos);
}

TEST_CASE("A measurement that could not change the answer is not demanded",
          "[tool-assignment]")
{
    // Every nozzle is the same size, so the missing measurement decides
    // nothing and asking for it would be noise.
    FeatureAssignmentState state = feature(0, MultiRateFeature::Walls, Visibility::Visible);
    state.geometry.carries_text_or_detail = true;

    const std::vector<ToolCapability> uniform { tool(0, 0.4), tool(1, 0.4) };
    const ToolProposal proposal = only(propose_tool_assignments({ state }, uniform, {}));
    CHECK(proposal.outcome != AssignmentOutcome::NeedsMeasurement);
}

TEST_CASE("A tie is reported, never broken by tool index", "[tool-assignment]")
{
    // Two identical fine nozzles, and the feature sits on neither of them.
    const std::vector<ToolCapability> twins { tool(0, 0.6), tool(1, 0.25), tool(2, 0.25) };

    FeatureAssignmentState state = feature(0, MultiRateFeature::TopSurface, Visibility::Visible, 0);
    state.geometry.minimum_feature_width_mm = 0.3;

    const ToolProposal proposal = only(propose_tool_assignments({ state }, twins, {}));
    CHECK(proposal.outcome == AssignmentOutcome::Ambiguous);
    CHECK(proposal.tied_tools == std::vector<int> { 1, 2 });
    // No decision was made, so no tool moved.
    CHECK(proposal.proposed_tool == proposal.current_tool);
}

TEST_CASE("A tie the current assignment already resolves is not ambiguous",
          "[tool-assignment]")
{
    // Same twins, but the work is already on one of them: staying put is a
    // real saving and a legitimate tie-break.
    const std::vector<ToolCapability> twins { tool(0, 0.6), tool(1, 0.25), tool(2, 0.25) };

    FeatureAssignmentState state = feature(0, MultiRateFeature::TopSurface, Visibility::Visible, 2);
    state.geometry.minimum_feature_width_mm = 0.3;

    const ToolProposal proposal = only(propose_tool_assignments({ state }, twins, {}));
    CHECK(proposal.outcome == AssignmentOutcome::Unchanged);
    CHECK(proposal.proposed_tool == 2);
}

TEST_CASE("Work no tool can produce is refused with the reasons", "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::Walls, Visibility::Visible);
    state.geometry.minimum_feature_width_mm = 0.1;   // finer than every nozzle

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));

    CHECK(proposal.outcome == AssignmentOutcome::NoCompatibleTool);
    CHECK(proposal.proposed_tool == proposal.current_tool);
    // Each tool says why it was ruled out.
    CHECK(proposal.reasons.size() >= 4);
    CHECK(proposal.reasons.front().find("wider than") != std::string::npos);
}

TEST_CASE("Material requirements are honoured and unknown loads are not assumed",
          "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::SparseInfill, Visibility::Hidden);
    state.geometry.required_material = "PETG";

    SECTION("only one tool carries the material") {
        std::vector<ToolCapability> tools = four_tool_profile();
        tools[2].material = "PETG";
        const ToolProposal proposal = only(propose_tool_assignments({ state }, tools, {}));
        CHECK(proposal.outcome == AssignmentOutcome::Proposed);
        CHECK(proposal.proposed_tool == 2);
    }

    SECTION("an unreported load is not treated as compatible") {
        std::vector<ToolCapability> tools = four_tool_profile();
        for (ToolCapability &capability : tools)
            capability.material.clear();
        const ToolProposal proposal = only(propose_tool_assignments({ state }, tools, {}));
        CHECK(proposal.outcome == AssignmentOutcome::NoCompatibleTool);
        CHECK(proposal.reasons.front().find("not reported") != std::string::npos);
    }
}

TEST_CASE("A layer height outside a tool's range rules it out", "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::SparseInfill, Visibility::Hidden);
    state.geometry.layer_height_mm = 0.07;   // below the 0.8 nozzle's 0.08 minimum

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));
    CHECK(proposal.outcome == AssignmentOutcome::Proposed);
    // 0.6 is the coarsest nozzle that can still lay a 0.07 mm layer.
    CHECK(proposal.proposed_tool == 2);
}

TEST_CASE("Flow breaks a tie between equally coarse nozzles", "[tool-assignment]")
{
    const std::vector<ToolCapability> tools { tool(0, 0.4, 15.), tool(1, 0.8, 20.),
                                              tool(2, 0.8, 30.) };
    FeatureAssignmentState state = feature(0, MultiRateFeature::SparseInfill, Visibility::Hidden, 0);
    state.geometry.layer_height_mm = 0.3;

    const ToolProposal proposal = only(propose_tool_assignments({ state }, tools, {}));
    CHECK(proposal.outcome == AssignmentOutcome::Proposed);
    CHECK(proposal.proposed_tool == 2);
}

TEST_CASE("Work that might be seen is not given the coarsest nozzle on a guess",
          "[tool-assignment]")
{
    // Taking the coarsest nozzle that fits is only safe when something says
    // what has to fit. Solid infill of unknown visibility says neither.
    FeatureAssignmentState state = feature(0, MultiRateFeature::SolidInfill, Visibility::Unknown);
    state.geometry.layer_height_mm = 0.2;

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));
    CHECK(proposal.outcome == AssignmentOutcome::NeedsMeasurement);
    CHECK(proposal.proposed_tool == proposal.current_tool);
    CHECK(proposal.reasons.front().find("visibility is unknown") != std::string::npos);

    // Measuring the requirement is enough to decide it. A 0.5 mm accuracy plus
    // a 0.2 mm layer leaves only the 0.4 nozzle, which is where it already is.
    state.geometry.required_accuracy_mm = 0.5;
    const ToolProposal measured =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));
    CHECK(measured.outcome == AssignmentOutcome::Unchanged);
    CHECK(measured.proposed_tool == 0);
}

TEST_CASE("A visible surface with no measured requirement is not guessed at",
          "[tool-assignment]")
{
    // A top surface is visible by construction, whatever visibility says.
    FeatureAssignmentState state = feature(0, MultiRateFeature::TopSurface, Visibility::Unknown);
    state.geometry.layer_height_mm = 0.2;

    const ToolProposal proposal =
        only(propose_tool_assignments({ state }, four_tool_profile(), {}));
    CHECK(proposal.outcome == AssignmentOutcome::NeedsMeasurement);
    CHECK(proposal.reasons.front().find("visible") != std::string::npos);
}

TEST_CASE("Proposals are previewed and applied separately", "[tool-assignment]")
{
    std::vector<FeatureAssignmentState> features {
        feature(0, MultiRateFeature::SparseInfill, Visibility::Hidden, 0),
        feature(1, MultiRateFeature::SparseInfill, Visibility::Hidden, 0),
    };
    for (FeatureAssignmentState &state : features)
        state.geometry.layer_height_mm = 0.2;

    const std::vector<ToolProposal> proposals =
        propose_tool_assignments(features, four_tool_profile(), {});
    REQUIRE(proposals.size() == 2);
    REQUIRE(proposals[0].outcome == AssignmentOutcome::Proposed);

    std::vector<int> resolved;
    std::string      error;

    SECTION("nothing accepted leaves every tool alone") {
        REQUIRE(apply_tool_proposals(proposals, {}, &resolved, &error));
        CHECK(error.empty());
        CHECK(resolved == std::vector<int> { 0, 0 });
    }

    SECTION("accepting one applies only that one") {
        REQUIRE(apply_tool_proposals(proposals, { true, false }, &resolved, &error));
        CHECK(resolved == std::vector<int> { 3, 0 });
    }
}

TEST_CASE("Accepting a proposal that carries no decision is refused",
          "[tool-assignment]")
{
    FeatureAssignmentState state = feature(0, MultiRateFeature::SolidInfill, Visibility::Unknown);
    state.geometry.layer_height_mm = 0.2;

    const std::vector<ToolProposal> proposals =
        propose_tool_assignments({ state }, four_tool_profile(), {});
    REQUIRE(proposals.front().outcome == AssignmentOutcome::NeedsMeasurement);

    std::vector<int> resolved;
    std::string      error;
    CHECK_FALSE(apply_tool_proposals(proposals, { true }, &resolved, &error));
    CHECK(error.find("needs measurement") != std::string::npos);
    // Nothing partial is left behind.
    CHECK(resolved.empty());
}

TEST_CASE("Mismatched acceptance is refused rather than truncated",
          "[tool-assignment]")
{
    const std::vector<ToolProposal> proposals =
        propose_tool_assignments({ feature(0, MultiRateFeature::Walls, Visibility::Visible) },
                                 four_tool_profile(), {});
    std::vector<int> resolved;
    std::string      error;
    CHECK_FALSE(apply_tool_proposals(proposals, { true, true }, &resolved, &error));
    CHECK(error.find("acceptance was supplied") != std::string::npos);
}

TEST_CASE("A machine with no tools proposes nothing", "[tool-assignment]")
{
    const ToolProposal proposal = only(
        propose_tool_assignments({ feature(0, MultiRateFeature::Walls, Visibility::Visible) },
                                 {}, {}));
    CHECK(proposal.outcome == AssignmentOutcome::NoCompatibleTool);
    CHECK(proposal.reasons.front().find("no tools") != std::string::npos);
}
