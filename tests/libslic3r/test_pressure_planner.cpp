// Phase 17: per-tool pressure-transient planning. Two properties carry the
// phase. Preparation extrusion never lands on a visible surface - a lead-in
// that would be seen is a block, not a compromise. And a lead-in that does not
// fit is reported rather than shortened, because a short lead-in under-extrudes
// silently. Around those: an unresolved pressure advance never falls back to
// zero, since zero is a real setting meaning "no compensation".
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_approx.hpp>

#include <algorithm>

#include "libslic3r/PressurePlanner.hpp"

using namespace Slic3r;

namespace {

PressureAdvanceCurve constant_curve(int tool, double advance)
{
    PressureAdvanceCurve curve;
    curve.tool     = tool;
    curve.material = "ABS";
    curve.points   = { { 0., advance } };
    return curve;
}

PressureAdvanceCurve flow_curve(int tool)
{
    PressureAdvanceCurve curve;
    curve.tool     = tool;
    curve.material = "ABS";
    curve.points   = { { 4., 0.030 }, { 12., 0.050 }, { 20., 0.070 } };
    return curve;
}

PressureSegment segment(size_t index, int tool, PressureEventKind kind, double flow)
{
    PressureSegment value;
    value.index                = index;
    value.tool                 = tool;
    value.kind                 = kind;
    value.material             = "ABS";
    value.temperature_c        = 250.;
    value.flow_mm3_s           = flow;
    value.extrusion_width_mm   = 0.45;
    value.layer_height_mm      = 0.2;
    value.available_lead_in_mm = 10.;
    return value;
}

PressureCapabilities capable()
{
    PressureCapabilities capabilities;
    capabilities.can_set_pressure_advance = true;
    capabilities.set_advance_gcode        = "SET_PRESSURE_ADVANCE EXTRUDER=[tool] ADVANCE=[advance]";
    return capabilities;
}

bool has_blocking(const PressurePlan &plan, const std::string &fragment)
{
    return std::any_of(plan.notes.begin(), plan.notes.end(), [&](const PressureNote &note) {
        return note.severity == PressureSeverity::Blocked &&
               note.message.find(fragment) != std::string::npos;
    });
}

const PressureAction *first_of(const PressurePlan &plan, PressureActionKind kind)
{
    for (const PressureAction &action : plan.actions)
        if (action.kind == kind)
            return &action;
    return nullptr;
}

} // namespace

TEST_CASE("A curve interpolates inside its measured range", "[pressure-planner]")
{
    const std::vector<PressureAdvanceCurve> curves { flow_curve(0) };
    std::string                             reason;

    const std::optional<double> middle =
        resolve_pressure_advance(curves, 0, "ABS", 250., 8., &reason);
    REQUIRE(middle.has_value());
    CHECK(*middle == Catch::Approx(0.040));

    CHECK(*resolve_pressure_advance(curves, 0, "ABS", 250., 4., &reason) ==
          Catch::Approx(0.030));
    CHECK(*resolve_pressure_advance(curves, 0, "ABS", 250., 20., &reason) ==
          Catch::Approx(0.070));
}

TEST_CASE("A curve is never extrapolated past what was measured",
          "[pressure-planner]")
{
    const std::vector<PressureAdvanceCurve> curves { flow_curve(0) };
    std::string                             reason;

    CHECK_FALSE(resolve_pressure_advance(curves, 0, "ABS", 250., 40., &reason).has_value());
    CHECK(reason.find("no pressure advance curve covers") != std::string::npos);
    CHECK_FALSE(resolve_pressure_advance(curves, 0, "ABS", 250., 1., &reason).has_value());
}

TEST_CASE("A curve for another tool or material does not answer",
          "[pressure-planner]")
{
    const std::vector<PressureAdvanceCurve> curves { flow_curve(0) };
    std::string                             reason;

    CHECK_FALSE(resolve_pressure_advance(curves, 1, "ABS", 250., 8., &reason).has_value());
    CHECK_FALSE(resolve_pressure_advance(curves, 0, "PLA", 250., 8., &reason).has_value());
}

TEST_CASE("A temperature-specific curve does not apply at an unknown temperature",
          "[pressure-planner]")
{
    PressureAdvanceCurve curve = flow_curve(0);
    curve.temperature_c        = 250.;
    curve.temperature_tolerance_c = 5.;

    std::string reason;
    CHECK(resolve_pressure_advance({ curve }, 0, "ABS", 252., 8., &reason).has_value());
    CHECK_FALSE(resolve_pressure_advance({ curve }, 0, "ABS", 280., 8., &reason).has_value());
    // Not knowing the temperature is not the same as being in range.
    CHECK_FALSE(
        resolve_pressure_advance({ curve }, 0, "ABS", std::nullopt, 8., &reason).has_value());
}

TEST_CASE("The more specific curve wins", "[pressure-planner]")
{
    PressureAdvanceCurve general;
    general.tool   = 0;
    general.points = { { 0., 0.020 } };

    PressureAdvanceCurve specific = constant_curve(0, 0.055);
    specific.temperature_c        = 250.;

    std::string reason;
    const std::optional<double> resolved =
        resolve_pressure_advance({ general, specific }, 0, "ABS", 250., 8., &reason);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == Catch::Approx(0.055));
}

TEST_CASE("The advance is set once and only reset when it actually changes",
          "[pressure-planner]")
{
    const std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Steady, 8.),
        segment(1, 0, PressureEventKind::Steady, 8.),
        segment(2, 0, PressureEventKind::FlowChange, 16.),
    };

    const PressurePlan plan =
        plan_pressure(segments, { flow_curve(0) }, capable(), PressurePolicy {});

    std::vector<const PressureAction *> sets;
    for (const PressureAction &action : plan.actions)
        if (action.kind == PressureActionKind::SetAdvance)
            sets.push_back(&action);

    REQUIRE(sets.size() == 2);
    CHECK(sets[0]->value == Catch::Approx(0.040));
    CHECK(sets[1]->value == Catch::Approx(0.060));
    CHECK(sets[0]->emitted);
    CHECK(sets[0]->gcode == "SET_PRESSURE_ADVANCE EXTRUDER=0 ADVANCE=0.0400");
}

TEST_CASE("An unresolvable advance leaves the current one alone rather than zeroing it",
          "[pressure-planner]")
{
    const std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Steady, 40.),
    };

    const PressurePlan plan =
        plan_pressure(segments, { flow_curve(0) }, capable(), PressurePolicy {});

    CHECK(first_of(plan, PressureActionKind::SetAdvance) == nullptr);
    REQUIRE(plan.notes.size() >= 1);
    CHECK(plan.notes.front().severity == PressureSeverity::Warning);
    CHECK(plan.notes.front().message.find("left unchanged") != std::string::npos);
}

TEST_CASE("A declared capability without a command emits nothing", "[pressure-planner]")
{
    PressureCapabilities capabilities = capable();
    capabilities.set_advance_gcode.clear();

    const PressurePlan plan = plan_pressure({ segment(0, 0, PressureEventKind::Steady, 8.) },
                                            { flow_curve(0) }, capabilities,
                                            PressurePolicy {});

    const PressureAction *set = first_of(plan, PressureActionKind::SetAdvance);
    REQUIRE(set != nullptr);
    // The action is still planned; only the command is absent.
    CHECK_FALSE(set->emitted);
    CHECK(set->gcode.empty());
    CHECK(plan.limitations.size() == 1);
}

TEST_CASE("Parking releases pressure and printing again restores it",
          "[pressure-planner]")
{
    const std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Steady, 8.),
        segment(1, 0, PressureEventKind::Park, 0.),
        segment(2, 0, PressureEventKind::Steady, 8.),
    };

    const PressurePlan plan =
        plan_pressure(segments, { flow_curve(0) }, capable(), PressurePolicy {});

    CHECK(plan.conditions[1] == PressureCondition::Released);
    CHECK(plan.conditions[2] == PressureCondition::Established);

    const PressureAction *release = first_of(plan, PressureActionKind::Release);
    REQUIRE(release != nullptr);
    CHECK(release->reason.find("parking") != std::string::npos);

    // Two lead-ins: the first printing segment starts from an unknown pressure
    // state, and segment 2 follows the park.
    std::vector<size_t> lead_ins;
    for (const PressureAction &action : plan.actions)
        if (action.kind == PressureActionKind::LeadIn)
            lead_ins.push_back(action.at_segment);
    CHECK(lead_ins == std::vector<size_t> { 0, 2 });
}

TEST_CASE("A lead-in is never placed on a visible surface", "[pressure-planner]")
{
    std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Park, 0.),
        segment(1, 0, PressureEventKind::Steady, 8.),
    };
    segments[1].on_visible_surface = true;

    const PressurePlan plan =
        plan_pressure(segments, { flow_curve(0) }, capable(), PressurePolicy {});

    CHECK(plan.has_blocking_note());
    CHECK(has_blocking(plan, "the lead-in would be seen"));
    CHECK(first_of(plan, PressureActionKind::LeadIn) == nullptr);
}

TEST_CASE("A lead-in that does not fit is reported, not shortened",
          "[pressure-planner]")
{
    std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Park, 0.),
        segment(1, 0, PressureEventKind::BridgeEntry, 8.),
    };
    segments[1].available_lead_in_mm = 0.5;

    const PressurePlan plan =
        plan_pressure(segments, { flow_curve(0) }, capable(), PressurePolicy {});

    CHECK(plan.has_blocking_note());
    CHECK(has_blocking(plan, "only 0.50 mm is available"));
    CHECK(first_of(plan, PressureActionKind::LeadIn) == nullptr);
}

TEST_CASE("A bridge entered at full pressure says so", "[pressure-planner]")
{
    const std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Steady, 8.),
        segment(1, 0, PressureEventKind::BridgeEntry, 8.),
    };

    const PressurePlan plan =
        plan_pressure(segments, { flow_curve(0) }, capable(), PressurePolicy {});
    CHECK_FALSE(plan.has_blocking_note());
    CHECK(std::any_of(plan.notes.begin(), plan.notes.end(), [](const PressureNote &note) {
        return note.message.find("starts at full pressure") != std::string::npos;
    }));
}

TEST_CASE("Pressure state is tracked per tool", "[pressure-planner]")
{
    const std::vector<PressureSegment> segments {
        segment(0, 0, PressureEventKind::Steady, 8.),
        segment(1, 1, PressureEventKind::Park, 0.),
        segment(2, 0, PressureEventKind::Steady, 8.),
    };

    const PressurePlan plan = plan_pressure(
        segments, { flow_curve(0), flow_curve(1) }, capable(), PressurePolicy {});

    // Parking T1 must not make T0 need a lead-in.
    CHECK(plan.conditions[2] == PressureCondition::Established);
    const PressureAction *lead_in = first_of(plan, PressureActionKind::LeadIn);
    REQUIRE(lead_in != nullptr);
    CHECK(lead_in->at_segment == 0);
}

TEST_CASE("An empty print is an error, not an empty plan", "[pressure-planner]")
{
    const PressurePlan plan = plan_pressure({}, {}, capable(), PressurePolicy {});
    CHECK_FALSE(plan.error.empty());
}
