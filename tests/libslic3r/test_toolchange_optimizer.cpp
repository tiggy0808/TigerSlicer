// Phase 4: toolchange-aware global optimization. The tests concentrate on the
// properties that make an optimizer safe to trust rather than on any particular
// ordering: it never reassigns a tool, it never breaks a dependency or the Z
// order, it never claims an improvement it did not make, and every change it
// keeps explains which constraint kept it.
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/ToolchangeOptimizer.hpp"

using namespace Slic3r;

namespace {

MultiRateOperation op(size_t id, unsigned int tool, coordf_t bottom_z, coordf_t print_z)
{
    MultiRateOperation operation;
    operation.id               = id;
    operation.tool_id          = tool;
    operation.bottom_z         = bottom_z;
    operation.print_z          = print_z;
    operation.extrusion_height = print_z - bottom_z;
    return operation;
}

ToolThermalState warm_tool(int tool)
{
    ToolThermalState state;
    state.tool                  = tool;
    state.active_temperature_c  = 215.;
    state.standby_temperature_c = 170.;
    state.idle_temperature_c    = 100.;
    state.heat_rate_c_s         = 1.5;   // 45 degrees of deficit is a 30 s heat
    state.cool_rate_c_s         = 0.5;
    state.current_temperature_c = 170.;
    return state;
}

OptimizerInputs inputs_for(size_t operations, size_t tools = 3)
{
    OptimizerInputs inputs;
    inputs.extrusion_s.assign(operations, 10.);
    inputs.explicitly_assigned.assign(operations, false);

    inputs.contract.park_s       = 2.;
    inputs.contract.pickup_s     = 3.;
    inputs.contract.brush_pass_s = 0.5;
    inputs.contract.prime_s      = 1.5;
    inputs.contract.probe_s      = 4.;

    inputs.tool_count = tools;
    for (size_t tool = 0; tool < tools; ++tool)
        inputs.thermal.push_back(warm_tool(int(tool)));

    inputs.dock_travel_mm    = 300.;
    inputs.travel_speed_mm_s = 150.;
    inputs.purge_flow_mm3_s  = 12.;
    inputs.brush_passes      = 4;

    inputs.purge_volume_mm3.assign(tools * tools, 100.);
    for (size_t tool = 0; tool < tools; ++tool)
        inputs.purge_volume_mm3[tool * tools + tool] = 0.;

    inputs.initial_tool = 0;
    return inputs;
}

// The case where a local rule loses and lookahead wins.
//
// Layer 1 holds T1, T2, T1; layer 2 holds only T1. Taking the lowest id first
// leaves the layer on T2 and forces a return change for layer 2. Doing the T2
// work first ends layer 1 on T1, which layer 2 continues for free.
MultiRateSchedule lookahead_schedule()
{
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 1u, 0., 0.2));
    schedule.add_operation(op(1, 2u, 0., 0.2));
    schedule.add_operation(op(2, 1u, 0., 0.2));
    schedule.add_operation(op(3, 1u, 0.2, 0.4));
    return schedule;
}

std::vector<unsigned int> tools_in_order(const MultiRateSchedule &schedule,
                                         const std::vector<size_t> &order)
{
    std::vector<unsigned int> tools;
    for (size_t id : order)
        tools.push_back(schedule.operations()[id].tool_id);
    return tools;
}

} // namespace

TEST_CASE("Optimization only reorders; it never reassigns a tool", "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4), OptimizationPolicy::FewestChanges);
    REQUIRE(result.error.empty());

    // Same operations, same tools, only the sequence differs. An explicit
    // colour therefore cannot be altered by optimization at all.
    std::vector<size_t> sorted = result.optimized.order;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == std::vector<size_t> { 0, 1, 2, 3 });

    std::vector<unsigned int> optimized = tools_in_order(schedule, result.optimized.order);
    std::vector<unsigned int> baseline  = tools_in_order(schedule, result.baseline.order);
    std::sort(optimized.begin(), optimized.end());
    std::sort(baseline.begin(), baseline.end());
    CHECK(optimized == baseline);
}

TEST_CASE("Lookahead removes a return change the local rule cannot see",
          "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4), OptimizationPolicy::FewestChanges);
    REQUIRE(result.error.empty());

    CHECK(result.baseline.toolchange_count() == 3);
    CHECK(result.optimized.toolchange_count() == 2);
    CHECK(result.improved);
    CHECK(result.optimized.total_s() < result.baseline.total_s());

    // The T2 work is pulled to the front of the layer so the layer ends on T1.
    REQUIRE_FALSE(result.optimized.order.empty());
    CHECK(result.optimized.order.front() == 1);
}

TEST_CASE("Removed changes are reported by transition", "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4), OptimizationPolicy::FewestChanges);
    REQUIRE(result.error.empty());

    // Two transitions disappear and one appears: three changes become two.
    // Reporting only the removals would overstate what was achieved.
    REQUIRE(result.eliminated.size() == 2);
    CHECK(result.eliminated[0].find("T0 -> T1") != std::string::npos);
    CHECK(result.eliminated[1].find("T1 -> T2") != std::string::npos);
    REQUIRE(result.introduced.size() == 1);
    CHECK(result.introduced.front().find("T0 -> T2") != std::string::npos);
    CHECK(result.introduced.front().find("introduced") != std::string::npos);

    // Every change that survived still explains itself.
    CHECK(result.unavoidable.size() == result.optimized.toolchange_count());
    for (const std::string &line : result.unavoidable)
        CHECK_FALSE(line.empty());
}

TEST_CASE("Z order is never violated by reordering", "[toolchange-optimizer]")
{
    // Layer 2 is cheaper to reach on the current tool, but climbing early
    // would strand layer 1: the optimizer must not take it.
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 1u, 0., 0.2));
    schedule.add_operation(op(1, 0u, 0.2, 0.4));

    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(2), OptimizationPolicy::Fastest);
    REQUIRE(result.error.empty());
    CHECK(result.optimized.order == std::vector<size_t> { 0, 1 });
}

TEST_CASE("Dependencies are never violated by reordering", "[toolchange-optimizer]")
{
    MultiRateSchedule schedule;
    for (size_t id = 0; id < 6; ++id)
        schedule.add_operation(op(id, (id % 2 == 0) ? 0u : 1u, coordf_t(id) * 0.2,
                                  coordf_t(id + 1) * 0.2));
    std::string error;
    for (size_t id = 0; id + 1 < 6; ++id)
        REQUIRE(schedule.add_dependency(id, id + 1, MultiRateDependencyReason::MonotonicZ, &error));

    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(6, 2), OptimizationPolicy::FewestChanges);
    REQUIRE(result.error.empty());

    CHECK(result.optimized.order == std::vector<size_t> { 0, 1, 2, 3, 4, 5 });
    CHECK(result.optimized.toolchange_count() == 5);
    // Nothing could be improved, so nothing is claimed.
    CHECK_FALSE(result.improved);
    CHECK(result.eliminated.empty());
}

TEST_CASE("A change the graph forces names the responsible constraint",
          "[toolchange-optimizer]")
{
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 0u, 0., 0.2));   // T0
    schedule.add_operation(op(1, 1u, 0.2, 0.4));  // T1
    schedule.add_operation(op(2, 0u, 0.4, 0.6));  // T0, but only after the T1 work
    std::string error;
    REQUIRE(schedule.add_dependency(0, 1, MultiRateDependencyReason::MonotonicZ, &error));
    REQUIRE(schedule.add_dependency(1, 2, MultiRateDependencyReason::MaterialInterface, &error));

    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(3, 2), OptimizationPolicy::Fastest);
    REQUIRE(result.error.empty());
    REQUIRE(result.optimized.toolchanges.size() == 2);

    const ToolchangeEvent &forced = result.optimized.toolchanges.front();
    CHECK(forced.necessity == ToolchangeNecessity::DependencyForced);
    CHECK(forced.explanation.find("material interface") != std::string::npos);
}

TEST_CASE("A change made because a tool ran out says so", "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4), OptimizationPolicy::FewestChanges);
    REQUIRE(result.error.empty());
    REQUIRE_FALSE(result.optimized.toolchanges.empty());
    // T0 is loaded at the start and has no work anywhere in the schedule.
    CHECK(result.optimized.toolchanges.front().necessity == ToolchangeNecessity::ToolExhausted);
    CHECK(result.optimized.toolchanges.front().explanation.find("no remaining work") !=
          std::string::npos);
}

TEST_CASE("Heating overlaps with the batch that precedes it", "[toolchange-optimizer]")
{
    // Four operations on one layer: three on T0, then one on T1.
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 0u, 0., 0.2));
    schedule.add_operation(op(1, 0u, 0., 0.2));
    schedule.add_operation(op(2, 0u, 0., 0.2));
    schedule.add_operation(op(3, 1u, 0., 0.2));
    const OptimizerInputs inputs = inputs_for(4, 2);

    std::string error;
    // 30 s of T0 work precedes the change, which exactly covers the 30 s heat.
    const ScheduleEvaluation late =
        evaluate_schedule(schedule, { 0, 1, 2, 3 }, inputs, &error);
    REQUIRE(error.empty());
    REQUIRE(late.toolchanges.size() == 1);
    CHECK(late.toolchanges.front().cost.heating_wait_s == Catch::Approx(0.));

    // Changing first leaves no lead time at all, so the whole heat is a wait.
    // (That order also has to change back to T0, which is why it is slower.)
    const ScheduleEvaluation early =
        evaluate_schedule(schedule, { 3, 0, 1, 2 }, inputs, &error);
    REQUIRE(error.empty());
    REQUIRE(early.toolchanges.size() == 2);
    CHECK(early.toolchanges.front().cost.heating_wait_s == Catch::Approx(30.));
    CHECK(early.total_s() > late.total_s());
}

TEST_CASE("Least waste and fewest changes are different objectives",
          "[toolchange-optimizer]")
{
    // T1 is cheap to purge into, T2 is expensive.
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 1u, 0., 0.2));
    schedule.add_operation(op(1, 2u, 0., 0.2));

    OptimizerInputs inputs = inputs_for(2, 3);
    auto purge = [&](size_t from, size_t to, double volume) {
        inputs.purge_volume_mm3[from * 3 + to] = volume;
    };
    purge(0, 1, 10.);
    purge(0, 2, 400.);
    purge(1, 2, 400.);
    purge(2, 1, 20.);

    const OptimizationResult waste =
        optimize_toolchanges(schedule, inputs, OptimizationPolicy::LeastWaste);
    REQUIRE(waste.error.empty());

    // T0 -> T2 -> T1 purges 420; T0 -> T1 -> T2 purges 410.
    CHECK(waste.optimized.purge_volume_mm3 <= waste.baseline.purge_volume_mm3);
    REQUIRE_FALSE(waste.optimized.toolchanges.empty());
    CHECK(waste.optimized.toolchanges.front().to_tool == 1);
}

TEST_CASE("Best detail may accept an extra change to bring quality work forward",
          "[toolchange-optimizer]")
{
    // Operation 2 on T1 is the predecessor of a material-interface edge.
    // Nothing forces it early, so only a policy would pay a change for it.
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 0u, 0., 0.2));
    schedule.add_operation(op(1, 0u, 0., 0.2));
    schedule.add_operation(op(2, 1u, 0., 0.2));
    schedule.add_operation(op(3, 0u, 0.2, 0.4));
    std::string error;
    REQUIRE(schedule.add_dependency(2, 3, MultiRateDependencyReason::MaterialInterface, &error));

    const OptimizationResult detail =
        optimize_toolchanges(schedule, inputs_for(4, 2), OptimizationPolicy::BestDetail);
    REQUIRE(detail.error.empty());

    REQUIRE_FALSE(detail.optimized.order.empty());
    CHECK(detail.optimized.order.front() == 2);
    // The change it required is attributed to the policy, not to a constraint
    // that does not exist.
    REQUIRE_FALSE(detail.optimized.toolchanges.empty());
    CHECK(detail.optimized.toolchanges.front().necessity == ToolchangeNecessity::PolicyChosen);
    CHECK(detail.optimized.toolchanges.front().explanation.find("ready work remained") !=
          std::string::npos);

    // Fastest, given the same schedule, does not pay that change first.
    const OptimizationResult fastest =
        optimize_toolchanges(schedule, inputs_for(4, 2), OptimizationPolicy::Fastest);
    REQUIRE(fastest.error.empty());
    CHECK(fastest.optimized.order.front() != 2);
}

TEST_CASE("Explicitly assigned work keeps its relative order", "[toolchange-optimizer]")
{
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 1u, 0., 0.2));
    schedule.add_operation(op(1, 2u, 0., 0.2));
    schedule.add_operation(op(2, 1u, 0., 0.2));
    schedule.add_operation(op(3, 1u, 0.2, 0.4));

    OptimizerInputs inputs = inputs_for(4);
    inputs.explicitly_assigned[0] = true;
    inputs.explicitly_assigned[1] = true;

    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs, OptimizationPolicy::PreserveExplicitAssignment);
    REQUIRE(result.error.empty());

    const auto &order = result.optimized.order;
    const auto first  = std::find(order.begin(), order.end(), size_t(0));
    const auto second = std::find(order.begin(), order.end(), size_t(1));
    REQUIRE(first != order.end());
    REQUIRE(second != order.end());
    // Operation 1 would have gone first under any other policy; the promise to
    // preserve the explicit order outranks the saving.
    CHECK(first < second);
}

TEST_CASE("An improvement is never claimed when there is none", "[toolchange-optimizer]")
{
    MultiRateSchedule schedule;
    for (size_t id = 0; id < 4; ++id)
        schedule.add_operation(op(id, 0u, 0., 0.2));

    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4, 1), OptimizationPolicy::Fastest);
    REQUIRE(result.error.empty());

    CHECK_FALSE(result.improved);
    CHECK(result.optimized.order == result.baseline.order);
    CHECK(result.optimized.toolchange_count() == 0);
    CHECK(result.eliminated.empty());
}

TEST_CASE("Evaluating an order that breaks a dependency is refused",
          "[toolchange-optimizer]")
{
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 0u, 0., 0.2));
    schedule.add_operation(op(1, 1u, 0., 0.2));
    std::string error;
    REQUIRE(schedule.add_dependency(0, 1, MultiRateDependencyReason::VerticalSupport, &error));

    std::string evaluation_error;
    const ScheduleEvaluation evaluation =
        evaluate_schedule(schedule, { 1, 0 }, inputs_for(2, 2), &evaluation_error);
    CHECK(evaluation.order.empty());
    CHECK(evaluation_error.find("vertical support") != std::string::npos);
}

TEST_CASE("Evaluating an order that moves down in Z is refused", "[toolchange-optimizer]")
{
    // No dependency says so; the Z order is a constraint in its own right.
    MultiRateSchedule schedule;
    schedule.add_operation(op(0, 0u, 0., 0.2));
    schedule.add_operation(op(1, 0u, 0.2, 0.4));

    std::string evaluation_error;
    const ScheduleEvaluation evaluation =
        evaluate_schedule(schedule, { 1, 0 }, inputs_for(2, 1), &evaluation_error);
    CHECK(evaluation.order.empty());
    CHECK(evaluation_error.find("downward in Z") != std::string::npos);
}

TEST_CASE("Uncharacterized inputs are refused rather than assumed",
          "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();

    SECTION("missing extrusion times") {
        OptimizerInputs inputs = inputs_for(4);
        inputs.extrusion_s.resize(2);
        const OptimizationResult result =
            optimize_toolchanges(schedule, inputs, OptimizationPolicy::Fastest);
        CHECK(result.error.find("extrusion times") != std::string::npos);
        CHECK(result.optimized.order.empty());
    }

    SECTION("purge volumes never characterized") {
        OptimizerInputs inputs = inputs_for(4);
        inputs.purge_volume_mm3.clear();
        const OptimizationResult result =
            optimize_toolchanges(schedule, inputs, OptimizationPolicy::LeastWaste);
        // Absent purge data must not be read as a waste-free machine.
        CHECK(result.error.find("purge volumes") != std::string::npos);
    }

    SECTION("thermal state missing for a tool") {
        OptimizerInputs inputs = inputs_for(4);
        inputs.thermal.pop_back();
        const OptimizationResult result =
            optimize_toolchanges(schedule, inputs, OptimizationPolicy::Fastest);
        CHECK(result.error.find("thermal state") != std::string::npos);
    }

    SECTION("a tool the schedule uses was never declared") {
        OptimizerInputs inputs = inputs_for(4, 2);
        const OptimizationResult result =
            optimize_toolchanges(schedule, inputs, OptimizationPolicy::Fastest);
        CHECK(result.error.find("T2") != std::string::npos);
    }
}

TEST_CASE("The saving is in change overhead, not in extrusion",
          "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4), OptimizationPolicy::FewestChanges);
    REQUIRE(result.error.empty());

    // Four operations at 10 s each, whatever the ordering: optimization must
    // never appear to make the extrusion itself cheaper.
    CHECK(result.optimized.breakdown.extrusion_s == Catch::Approx(40.));
    CHECK(result.baseline.breakdown.extrusion_s == Catch::Approx(40.));
    CHECK(result.optimized.breakdown.toolchange_motion_s <
          result.baseline.breakdown.toolchange_motion_s);
    CHECK(result.optimized.purge_volume_mm3 < result.baseline.purge_volume_mm3);
}

TEST_CASE("Every policy produces a legal schedule of the same work",
          "[toolchange-optimizer]")
{
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizerInputs inputs = inputs_for(4);

    for (OptimizationPolicy policy : { OptimizationPolicy::Fastest,
                                       OptimizationPolicy::Balanced,
                                       OptimizationPolicy::BestDetail,
                                       OptimizationPolicy::FewestChanges,
                                       OptimizationPolicy::LeastWaste,
                                       OptimizationPolicy::PreserveExplicitAssignment }) {
        const OptimizationResult result = optimize_toolchanges(schedule, inputs, policy);
        INFO(optimization_policy_name(policy));
        REQUIRE(result.error.empty());
        REQUIRE(result.optimized.order.size() == 4);

        // Re-evaluating the result must succeed, which re-checks the graph and
        // the Z order independently of how the order was produced.
        std::string revalidation;
        const ScheduleEvaluation again =
            evaluate_schedule(schedule, result.optimized.order, inputs, &revalidation);
        CHECK(revalidation.empty());
        CHECK(again.toolchange_count() == result.optimized.toolchange_count());

        // No policy is ever allowed to return something worse than what it
        // started from, measured on its own objective.
        if (!result.improved)
            CHECK(result.optimized.order == result.baseline.order);
    }
}

TEST_CASE("Search limits are reported, not applied silently", "[toolchange-optimizer]")
{
    // Well under the cap, so nothing is limited and nothing is claimed to be.
    const MultiRateSchedule schedule = lookahead_schedule();
    const OptimizationResult result =
        optimize_toolchanges(schedule, inputs_for(4), OptimizationPolicy::Fastest);
    REQUIRE(result.error.empty());
    CHECK(result.notes.empty());
}
