// Stage A gate: the named scenarios, run as one thing rather than as parts.
//
// Each case here exists to raise one acceptance criterion from
// CoveredByComponentTests to CoveredByScenario. What separates these from the
// suites they lean on is that the criterion's own model is built and driven
// end to end, not that a new decision is being tested.
#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/AdaptiveLayers.hpp"
#include "libslic3r/BridgePlanner.hpp"
#include "libslic3r/DependencyComponents.hpp"
#include "libslic3r/MixedNozzleSupport.hpp"
#include "libslic3r/FlowCurves.hpp"
#include "libslic3r/PrintManifest.hpp"
#include "libslic3r/TelemetryImport.hpp"
#include "libslic3r/TransitionMatrix.hpp"

using namespace Slic3r;

namespace {

// The four-tool FDM profile this project is built around: a 0.20 fine tool, a
// 0.25, a 0.40 and a 0.60. Nothing infers anything from the index.
struct Tool
{
    int         index;
    double      nozzle_mm;
    double      width_mm;
    std::string material;
    double      temperature_c;
};

const std::vector<Tool> &four_tools()
{
    static const std::vector<Tool> tools = {
        { 0, 0.20, 0.22, "PLA", 210. },
        { 1, 0.25, 0.28, "PLA", 210. },
        { 2, 0.40, 0.45, "PETG", 240. },
        { 3, 0.60, 0.68, "PLA", 215. },
    };
    return tools;
}

FlowCurve curve_for(const Tool &tool, double flow_at_low, double flow_at_high)
{
    FlowCurve curve;
    curve.tool               = tool.index;
    curve.nozzle_diameter_mm = tool.nozzle_mm;
    curve.material           = tool.material;
    curve.samples            = { { tool.temperature_c - 20., flow_at_low },
                                 { tool.temperature_c + 20., flow_at_high } };
    curve.layer_height_mm    = { 0.05, 0.60 };
    curve.extrusion_width_mm = { tool.nozzle_mm, tool.nozzle_mm * 2. };
    return curve;
}

FlowQuery query_for(const Tool &tool, double requested)
{
    FlowQuery query;
    query.tool                 = tool.index;
    query.nozzle_diameter_mm   = tool.nozzle_mm;
    query.material             = tool.material;
    query.temperature_c        = tool.temperature_c;
    query.layer_height_mm      = 0.20;
    query.extrusion_width_mm   = tool.width_mm;
    query.feature              = MultiRateFeature::Walls;
    query.requested_flow_mm3_s = requested;
    return query;
}

TransitionEndpoint at(int tool, const std::string &material, const std::string &color = "")
{
    TransitionEndpoint endpoint;
    endpoint.tool     = tool;
    endpoint.material = material;
    endpoint.color    = color;
    return endpoint;
}

TransitionRule rule(const TransitionEndpoint &from, const TransitionEndpoint &to, double purge,
                    double prime, TransitionCompatibility compatibility)
{
    TransitionRule value;
    value.from             = from;
    value.to               = to;
    value.compatibility    = compatibility;
    value.purge_volume_mm3 = purge;
    value.prime_volume_mm3 = prime;
    return value;
}

} // namespace

TEST_CASE("Stage A/10: every extrusion is limited by the tool actually printing it",
          "[stage-a]")
{
    // One calibration per tool. Each is measured over its own nozzle, its own
    // material and its own temperature range, so nothing one tool measured can
    // answer for another.
    std::vector<FlowCurve> curves;
    curves.push_back(curve_for(four_tools()[0], 2.0, 3.0));   // the 0.20 fine tool
    curves.push_back(curve_for(four_tools()[1], 4.0, 6.0));   // the 0.25
    curves.push_back(curve_for(four_tools()[2], 11.0, 15.0)); // the 0.40, in PETG
    // The 0.60 is deliberately left uncalibrated.

    FlowOptions options;
    options.constant_max_volumetric_speed_mm3_s = 12.;

    SECTION("each tool gets its own limit, from its own curve")
    {
        const FlowResult fine =
            resolve_flow_limit(query_for(four_tools()[0], 1.0), curves, options);
        // The samples sit either side of the working temperature, so the
        // answer is interpolated between them rather than read off one.
        CHECK(fine.source == FlowSource::CalibratedInterpolated);
        CHECK(fine.limit == FlowLimit::CalibratedCurve);
        CHECK(fine.max_flow_mm3_s == Catch::Approx(2.5));
        CHECK_FALSE(fine.exceeds_limit);

        const FlowResult middle =
            resolve_flow_limit(query_for(four_tools()[1], 1.0), curves, options);
        CHECK(middle.max_flow_mm3_s == Catch::Approx(5.0));

        // Every answer explains itself; none is an unattributed number.
        CHECK_FALSE(fine.explanation.empty());
        CHECK_FALSE(middle.explanation.empty());
    }

    SECTION("the configured constant stays the ceiling, whatever a curve says")
    {
        // The 0.40 tool's curve says 13 at its own temperature; the profile
        // allows 12. A calibration may lower the limit and never raise it.
        const FlowResult coarse =
            resolve_flow_limit(query_for(four_tools()[2], 1.0), curves, options);
        CHECK(coarse.max_flow_mm3_s == Catch::Approx(12.0));
        CHECK(coarse.limit == FlowLimit::ConstantMvs);
    }

    SECTION("an uncalibrated tool falls back to the constant and says which tool")
    {
        const FlowResult uncalibrated =
            resolve_flow_limit(query_for(four_tools()[3], 1.0), curves, options);
        CHECK(uncalibrated.source == FlowSource::ConstantMvs);
        CHECK(uncalibrated.limit == FlowLimit::NoCalibrationForTool);
        CHECK(uncalibrated.max_flow_mm3_s == Catch::Approx(12.0));
        CHECK_FALSE(uncalibrated.extrapolated);
    }

    SECTION("an extrusion wanting more than its tool can deliver is flagged, not clipped")
    {
        // The fine tool sustains 2.5; ask it for 4.
        const FlowResult over =
            resolve_flow_limit(query_for(four_tools()[0], 4.0), curves, options);
        CHECK(over.exceeds_limit);
        CHECK(over.utilization > 1.);
        // The limit is still reported as what the tool can do, not as what was
        // asked for.
        CHECK(over.max_flow_mm3_s == Catch::Approx(2.5));
    }

    SECTION("a width outside what a tool was calibrated over is not answered from that curve")
    {
        FlowQuery wide = query_for(four_tools()[0], 1.0);
        wide.extrusion_width_mm = 0.60; // far past the 0.20 tool's calibrated band
        const FlowResult result = resolve_flow_limit(wide, curves, options);
        CHECK(result.limit == FlowLimit::OutsideCalibratedWidth);
        CHECK(result.source == FlowSource::ConstantMvs);
    }

    SECTION("no curve is ever consulted for another tool's work")
    {
        // Ask on the fine tool's index but with the 0.40's nozzle and
        // material: nothing matches, and nothing is borrowed.
        FlowQuery mismatched     = query_for(four_tools()[0], 1.0);
        mismatched.nozzle_diameter_mm = 0.40;
        mismatched.material           = "PETG";
        const FlowResult result = resolve_flow_limit(mismatched, curves, options);
        CHECK(result.limit == FlowLimit::NoCalibrationForTool);
    }
}

TEST_CASE("Stage A/18: a four-material print is priced transition by transition, in the "
          "direction each one runs",
          "[stage-a]")
{
    TransitionMatrix matrix;
    matrix.timing.purge_rate_mm3_s = 5.;
    matrix.timing.heating_rate_c_s = 2.;
    matrix.timing.cooling_rate_c_s = 1.;

    // PLA into PETG is cheap; PETG back into PLA is not, because PETG residue
    // shows in PLA. Both directions are declared, and separately.
    matrix.rules.push_back(rule(at(-1, "PLA"), at(-1, "PETG"), 60., 10.,
                                TransitionCompatibility::Compatible));
    matrix.rules.push_back(rule(at(-1, "PETG"), at(-1, "PLA"), 180., 10.,
                                TransitionCompatibility::Compatible));
    // A dark colour into a light one on the fine tool costs more still.
    TransitionRule fussy = rule(at(-1, "PLA", "black"), at(0, "PLA", "white"), 120., 10.,
                                TransitionCompatibility::Compatible);
    fussy.contamination_sensitivity = 1.;
    matrix.rules.push_back(fussy);
    // PLA into PLA still costs something when the colour changes; the rule
    // above is more specific and wins where it applies.
    matrix.rules.push_back(rule(at(-1, "PLA"), at(-1, "PLA"), 30., 5.,
                                TransitionCompatibility::Compatible));

    const std::vector<TransitionEndpoint> sequence = {
        at(0, "PLA", "white"), at(2, "PETG", "black"), at(0, "PLA", "white"),
        at(3, "PLA", "black"), at(0, "PLA", "white"),
    };
    const std::vector<double> temperatures = { 210., 240., 210., 215., 210. };

    TransitionPolicy policy;

    SECTION("the whole sequence prices, and each leg costs what its own direction says")
    {
        const TransitionValidation result =
            validate_transitions(matrix, sequence, temperatures, policy);
        CHECK(result.may_export());
        REQUIRE(result.costs.size() == 4);

        // PLA -> PETG, then PETG -> PLA. The return trip is the expensive one,
        // and nothing inferred it from the outbound rule.
        CHECK(result.costs[0].purge_volume_mm3 == Catch::Approx(60.));
        CHECK(result.costs[1].purge_volume_mm3 == Catch::Approx(180.));
        CHECK(result.costs[0].purge_volume_mm3 < result.costs[1].purge_volume_mm3);

        // The black-to-white leg onto the fine tool takes the more specific
        // rule, scaled by how much contamination that destination tolerates.
        CHECK(result.costs[3].purge_volume_mm3 > 120.);
        CHECK_FALSE(result.costs[3].provenance.empty());

        // Totals accumulate, and the time total exists because every leg could
        // be priced.
        CHECK(result.total_purge_mm3 > 400.);
        CHECK(result.total_time_s.has_value());
    }

    SECTION("removing one direction does not remove the other, and is not inferred back")
    {
        TransitionMatrix one_way = matrix;
        one_way.rules.erase(one_way.rules.begin() + 1); // drop PETG -> PLA

        const TransitionValidation result =
            validate_transitions(one_way, sequence, temperatures, policy);
        // The outbound leg is still priced; the return is uncharacterized and
        // blocks export rather than borrowing the outbound number.
        CHECK(result.costs[0].compatibility == TransitionCompatibility::Compatible);
        CHECK(result.costs[1].compatibility == TransitionCompatibility::Unknown);
        CHECK_FALSE(result.may_export());
        CHECK_FALSE(result.blocking.empty());
    }

    SECTION("an uncharacterized transition may be allowed, but only out loud")
    {
        TransitionMatrix one_way = matrix;
        one_way.rules.erase(one_way.rules.begin() + 1);

        TransitionPolicy permissive;
        permissive.allow_uncharacterized = true;
        const TransitionValidation result =
            validate_transitions(one_way, sequence, temperatures, permissive);
        CHECK(result.may_export());
        CHECK_FALSE(result.warnings.empty());
    }

    SECTION("an incompatible pair blocks the print and names what is responsible")
    {
        TransitionMatrix hostile = matrix;
        TransitionRule   refuse  = rule(at(-1, "PETG"), at(-1, "PLA"), 0., 0.,
                                        TransitionCompatibility::Incompatible);
        refuse.reason = "PETG residue does not clear from this hot end at PLA temperatures";
        hostile.rules[1] = refuse;

        const TransitionValidation result =
            validate_transitions(hostile, sequence, temperatures, policy);
        CHECK_FALSE(result.may_export());
        bool named = false;
        for (const std::string &blocking : result.blocking)
            if (blocking.find("does not clear") != std::string::npos)
                named = true;
        CHECK(named);
    }

    SECTION("one unpriced leg withholds the whole time total rather than understating it")
    {
        TransitionMatrix unpriced = matrix;
        unpriced.timing.purge_rate_mm3_s.reset();
        const TransitionValidation result =
            validate_transitions(unpriced, sequence, temperatures, policy);
        CHECK_FALSE(result.total_time_s.has_value());
        // The material totals are still there: those were measured.
        CHECK(result.total_purge_mm3 > 0.);
    }
}

// --- an imported job carried through to a manifest --------------------------

namespace {

TelemetryEvent event(double at_s, TelemetryEventKind kind, int tool = -1,
                     std::optional<double> value = std::nullopt)
{
    TelemetryEvent e;
    e.at_s  = at_s;
    e.kind  = kind;
    e.tool  = tool;
    e.value = value;
    return e;
}

// One finished job on the machine the manifest names, with three toolchanges
// so a proposal has something to stand on.
TelemetryJob finished_job(const std::string &manifest_id, double toolchange_s)
{
    TelemetryJob job;
    job.version                  = telemetry_document_version();
    job.source                   = TelemetrySource::TigerSlicerManifest;
    job.identity.machine_id      = "tiger-4t-001";
    job.identity.manifest_id     = manifest_id;
    job.identity.profile_version = std::string("print:fine@2");
    job.completion               = JobCompletion::Completed;
    job.filament_used_mm3        = 1500.;

    double t = 0.;
    job.events.push_back(event(t, TelemetryEventKind::JobStart));
    for (int change = 0; change < 3; ++change) {
        // All on one tool, so the samples accumulate against one setting.
        const int tool = 1;
        job.events.push_back(event(t += 10., TelemetryEventKind::ToolchangeStart, tool));
        job.events.push_back(event(t + toolchange_s, TelemetryEventKind::ToolchangeEnd, tool));
        t += toolchange_s;
        job.events.push_back(event(t, TelemetryEventKind::PurgeStart, tool));
        job.events.push_back(event(t += 4., TelemetryEventKind::PurgeEnd, tool, 90.));
    }
    job.events.push_back(event(t + 10., TelemetryEventKind::JobEnd));
    return job;
}

PrintManifest printed_manifest()
{
    PrintManifest manifest;
    manifest.tigerslicer_version = "2.9.4-tiger.30";
    manifest.tigerslicer_commit  = "0f775c9";
    manifest.project_name        = "two towers";
    manifest.project_hash        = "proj-1";
    manifest.models              = { { "towers.stl", "mesh-1" } };
    manifest.profiles            = { { ProfileKind::Printer, "tiger-4t", 1 },
                                     { ProfileKind::Print, "fine", 2 } };

    ToolMapping t0;
    t0.index       = 0;
    t0.material    = "PLA";
    t0.nozzle_mm   = 0.20;
    t0.offset_x_mm = 0.;
    t0.offset_y_mm = 0.;
    t0.offset_z_mm = 0.;
    t0.calibration = ProfileVersionKey { ProfileKind::Calibration, "T0-flow", 1 };
    manifest.tools = { t0 };

    manifest.calibration           = { { "T0-flow", "0.98" } };
    manifest.transition            = { { "PLA->PLA", "30 mm3" } };
    manifest.grids                 = { { "towers", "grid-1" } };
    manifest.ownership             = { { "walls", 0, "painted" } };
    manifest.scheduler_policy      = { { "mode", "bounded-batch" } };
    manifest.cost_model            = { { "toolchange_s", "25" } };
    manifest.thermal_model         = { { "heat_rate_c_per_s", "2" } };
    manifest.validation            = { { "descent", "proven" } };
    manifest.verifier              = { { "gcode-twin", "acceptable" } };
    manifest.estimates             = { { "time_s", "315" } };
    manifest.firmware_capabilities = { { "klipper", "0.12" } };
    manifest.macro_contracts       = { { "TOOLCHANGE", "selects and purges", true } };
    return manifest;
}

} // namespace

TEST_CASE("Stage A/17: an imported job is identified, filtered, and carried into the next "
          "manifest",
          "[stage-a]")
{
    const PrintManifest manifest = printed_manifest();
    const std::string   id       = manifest_digest(manifest);
    REQUIRE(audit_manifest(manifest).reproducible());

    JobIdentity expected;
    expected.machine_id      = "tiger-4t-001";
    expected.manifest_id     = id;
    expected.profile_version = std::string("print:fine@2");

    SECTION("a job is matched to the print it came from, never assumed into it")
    {
        CHECK(match_identity(finished_job(id, 40.).identity, expected) == IdentityMatch::Match);

        // Another machine's job for the same manifest is not this print.
        TelemetryJob elsewhere        = finished_job(id, 40.);
        elsewhere.identity.machine_id = "tiger-4t-002";
        CHECK(match_identity(elsewhere.identity, expected) == IdentityMatch::MachineMismatch);

        // A job for a manifest one character different is a different print.
        const TelemetryJob other = finished_job(id.substr(0, id.size() - 1) + "0", 40.);
        CHECK(match_identity(other.identity, expected) == IdentityMatch::ManifestMismatch);

        // And a log that never said is Unidentified, not an optimistic match.
        TelemetryJob anonymous = finished_job(id, 40.);
        anonymous.identity     = JobIdentity {};
        CHECK(match_identity(anonymous.identity, expected) == IdentityMatch::Unidentified);
    }

    SECTION("the job survives a round trip through its own document form")
    {
        const TelemetryJob original = finished_job(id, 40.);
        TelemetryJob       reread;
        std::string        error;
        REQUIRE(parse_telemetry_document(serialize_telemetry_document(original), &reread,
                                         &error));
        CHECK(error.empty());
        CHECK(reread.identity.manifest_id == id);
        CHECK(reread.events.size() == original.events.size());
        CHECK(reread.completion == JobCompletion::Completed);
    }

    SECTION("an interrupted job is kept but never mixed into the evidence")
    {
        // Three finished jobs all say the toolchange takes 40 s, not the 25 s
        // the cost model assumed. One interrupted job claims 5 s.
        std::vector<ObservationSet> sets;
        for (int i = 0; i < 3; ++i)
            sets.push_back(extract_observations(finished_job(id, 40.)));

        TelemetryJob abandoned = finished_job(id, 5.);
        abandoned.completion   = JobCompletion::Interrupted;
        sets.push_back(extract_observations(abandoned));

        const std::vector<CurrentSetting> settings = { { kMetricToolchangeTimeS, 1, 25. } };
        const TuningReport report = propose_tuning(sets, settings, TuningPolicy {});

        // The interrupted set is excluded by name, not silently averaged in.
        CHECK_FALSE(report.excluded.empty());

        // The proposal has to actually exist, or the checks below would pass
        // over an empty list and prove nothing.
        const TuningProposal *toolchange = nullptr;
        for (const TuningProposal &candidate : report.proposals)
            if (candidate.setting == kMetricToolchangeTimeS)
                toolchange = &candidate;
        REQUIRE(toolchange != nullptr);
        CHECK(toolchange->samples == 9);

        // Whatever is proposed rests only on the finished jobs, so it moves
        // towards 40 rather than being pulled down by the 5 s that never was.
        for (const TuningProposal &proposal : report.proposals)
            if (proposal.setting == kMetricToolchangeTimeS) {
                CHECK(proposal.proposed_value > 25.);
                CHECK_FALSE(proposal.evidence.empty());
            }
    }

    SECTION("only accepted proposals reach the next manifest, and it is a different print")
    {
        std::vector<ObservationSet> sets;
        for (int i = 0; i < 3; ++i)
            sets.push_back(extract_observations(finished_job(id, 40.)));

        std::vector<CurrentSetting> settings = { { kMetricToolchangeTimeS, 1, 25. } };
        const TuningReport          report = propose_tuning(sets, settings, TuningPolicy {});

        // Refusing everything changes nothing at all.
        const std::vector<bool> refused(report.proposals.size(), false);
        std::string             error;
        REQUIRE(apply_tuning_proposals(report.proposals, refused, &settings, &error));
        CHECK(settings[0].value == 25.);

        REQUIRE_FALSE(report.proposals.empty());
        {
            const std::vector<bool> accepted(report.proposals.size(), true);
            REQUIRE(apply_tuning_proposals(report.proposals, accepted, &settings, &error));
            CHECK(settings[0].value > 25.);

            // The next print records the changed cost model, and is therefore
            // a different print with a different manifest.
            PrintManifest next = manifest;
            next.cost_model    = { { "toolchange_s", std::to_string(int(settings[0].value)) } };
            CHECK(manifest_digest(next) != id);
            CHECK(audit_manifest(next).reproducible());
        }
    }

    SECTION("the machine's report is checked against what the manifest claimed")
    {
        const ManifestComparison agrees = compare_to_telemetry(
            manifest, { { "version", "2.9.4-tiger.30" }, { "tool.0.material", "PLA" } });
        CHECK(agrees.verdict == ComparisonVerdict::Match);

        const ManifestComparison disagrees =
            compare_to_telemetry(manifest, { { "tool.0.material", "PETG" } });
        CHECK(disagrees.verdict == ComparisonVerdict::Mismatch);
    }
}

// --- solid and top layers over coarse infill --------------------------------

namespace {

// The four-tool machine again, as bridging tools: the 0.20 fine tool for
// visible surfaces, the 0.60 for hidden bulk.
std::vector<BridgeTool> bridging_tools()
{
    BridgeTool fine;
    fine.tool                        = 0;
    fine.material                    = "PLA";
    fine.nozzle_mm                   = 0.20;
    fine.max_bridge_span_mm          = 25.;
    fine.max_flow_mm3_s              = 2.5;
    fine.bridge_flow_ratio           = 0.9;
    fine.min_layer_height_mm         = 0.05;
    fine.max_layer_height_mm         = 0.16;
    fine.has_part_cooling_fan        = true;
    fine.pressure_can_be_established = true;
    fine.change_cost_s               = 25.;

    BridgeTool coarse                  = fine;
    coarse.tool                        = 3;
    coarse.nozzle_mm                   = 0.60;
    coarse.max_bridge_span_mm          = 60.;
    coarse.max_flow_mm3_s              = 12.;
    coarse.min_layer_height_mm         = 0.20;
    coarse.max_layer_height_mm         = 0.48;
    coarse.change_cost_s               = 25.;

    return { fine, coarse };
}

BridgeAnchor anchor(bool solid, double width_mm, bool measured = true)
{
    BridgeAnchor value;
    value.solid    = solid;
    value.width_mm = width_mm;
    value.measured = measured;
    return value;
}

} // namespace

TEST_CASE("Stage A/7: a solid layer over coarse sparse infill is treated as the span it is",
          "[stage-a]")
{
    BridgePolicy policy;

    SECTION("landing on sparse infill asks for support before it is attempted")
    {
        // The first solid layer over the 0.60 tool's sparse infill: held at the
        // perimeter, unsupported in the middle.
        BridgeGeometry solid;
        solid.id              = 1;
        solid.role            = BridgeRole::Structural;
        solid.span_mm         = 20.;
        solid.layer_height_mm = 0.20;
        solid.visible         = false;
        solid.anchors         = { anchor(true, 4.), anchor(false, 4.) };

        const BridgePlan plan = plan_bridges({ solid }, bridging_tools(), policy);
        REQUIRE(plan.error.empty());
        REQUIRE(plan.decisions.size() == 1);
        CHECK(plan.decisions[0].anchors == AnchorState::PartiallyAnchored);
        CHECK(plan.decisions[0].verdict == BridgeVerdict::NeedsSupport);
        CHECK_FALSE(plan.decisions[0].reasons.empty());
    }

    SECTION("over solid material beneath, the same span is simply planned")
    {
        BridgeGeometry solid;
        solid.id              = 1;
        solid.role            = BridgeRole::Structural;
        solid.span_mm         = 20.;
        solid.layer_height_mm = 0.20;
        solid.visible         = false;
        solid.anchors         = { anchor(true, 4.), anchor(true, 4.) };

        const BridgePlan plan = plan_bridges({ solid }, bridging_tools(), policy);
        REQUIRE(plan.decisions.size() == 1);
        CHECK(plan.decisions[0].anchors == AnchorState::Anchored);
        CHECK(plan.decisions[0].verdict == BridgeVerdict::Planned);
        // The hidden structural layer goes to the coarse tool; the fine tool
        // cannot print a 0.20 layer at all.
        REQUIRE(plan.decisions[0].tool.has_value());
        CHECK(plan.decisions[0].tool.value() == 3);
    }

    SECTION("the top surface above it takes the fine tool, at a height that tool can print")
    {
        BridgeGeometry structural;
        structural.id              = 1;
        structural.role            = BridgeRole::Structural;
        structural.span_mm         = 20.;
        structural.layer_height_mm = 0.20;
        structural.visible         = false;
        structural.anchors         = { anchor(true, 4.), anchor(true, 4.) };

        BridgeGeometry top;
        top.id              = 2;
        top.role            = BridgeRole::Surface;
        top.span_mm         = 20.;
        top.layer_height_mm = 0.10;
        top.visible         = true;
        top.anchors         = { anchor(true, 4.), anchor(true, 4.) };
        top.depends_on      = { 1 };

        const BridgePlan plan = plan_bridges({ structural, top }, bridging_tools(), policy);
        REQUIRE(plan.decisions.size() == 2);
        CHECK(plan.decisions[0].tool.value() == 3);
        CHECK(plan.decisions[1].tool.value() == 0);
        // The top layer knows the layer under it is being printed, and pays
        // for the toolchange it needs.
        CHECK(plan.decisions[1].synchronized);
        CHECK(plan.decisions[1].change_cost_s.has_value());
    }

    SECTION("a top surface landing on a layer that was refused is refused too")
    {
        BridgeGeometry structural;
        structural.id              = 1;
        structural.role            = BridgeRole::Structural;
        structural.span_mm         = 200.; // past every tool's proven reach
        structural.layer_height_mm = 0.20;
        structural.anchors         = { anchor(true, 4.), anchor(true, 4.) };

        BridgeGeometry top;
        top.id              = 2;
        top.role            = BridgeRole::Surface;
        top.span_mm         = 20.;
        top.layer_height_mm = 0.10;
        top.visible         = true;
        top.anchors         = { anchor(true, 4.), anchor(true, 4.) };
        top.depends_on      = { 1 };

        const BridgePlan plan = plan_bridges({ structural, top }, bridging_tools(), policy);
        REQUIRE(plan.decisions.size() == 2);
        CHECK(plan.decisions[0].verdict == BridgeVerdict::Refused);
        CHECK_FALSE(plan.decisions[0].rejected_tools.empty());
        CHECK(plan.decisions[1].verdict == BridgeVerdict::Refused);
    }

    SECTION("an anchor nobody measured is unproven, which is not the same as fine")
    {
        BridgeGeometry solid;
        solid.id              = 1;
        solid.role            = BridgeRole::Structural;
        solid.span_mm         = 20.;
        solid.layer_height_mm = 0.20;
        solid.anchors         = { anchor(true, 4.), anchor(true, 4., false) };

        const BridgePlan plan = plan_bridges({ solid }, bridging_tools(), policy);
        REQUIRE(plan.decisions.size() == 1);
        CHECK(plan.decisions[0].verdict == BridgeVerdict::Unproven);
        CHECK(plan.decisions[0].anchors == AnchorState::Unknown);

        // Trusting it is possible, but only by saying so.
        BridgePolicy trusting            = policy;
        trusting.allow_unmeasured_anchors = true;
        const BridgePlan trusted = plan_bridges({ solid }, bridging_tools(), trusting);
        CHECK(trusted.decisions[0].verdict == BridgeVerdict::Planned);
    }
}

// --- traditional, organic, soluble and mixed-tool supports -------------------

namespace {

std::vector<SupportToolProfile> support_tools()
{
    SupportToolProfile fine;
    fine.tool                = 0;
    fine.nozzle_diameter_mm  = 0.20;
    fine.extrusion_width_mm  = 0.22;
    fine.max_flow_mm3_s      = 2.5;
    fine.min_layer_height_mm = 0.05;
    fine.max_layer_height_mm = 0.16;
    fine.material            = "PLA";

    SupportToolProfile coarse   = fine;
    coarse.tool                 = 3;
    coarse.nozzle_diameter_mm   = 0.60;
    coarse.extrusion_width_mm   = 0.68;
    coarse.max_flow_mm3_s       = 12.;
    coarse.min_layer_height_mm  = 0.20;
    coarse.max_layer_height_mm  = 0.48;

    SupportToolProfile soluble  = fine;
    soluble.tool                = 2;
    soluble.nozzle_diameter_mm  = 0.40;
    soluble.extrusion_width_mm  = 0.45;
    soluble.max_flow_mm3_s      = 8.;
    soluble.min_layer_height_mm = 0.10;
    soluble.max_layer_height_mm = 0.30;
    soluble.material            = "PVA";
    soluble.soluble             = true;

    return { fine, coarse, soluble };
}

// A support whose bulk is on the coarse tool and whose contact layer is on
// something finer - which is the whole point of a mixed-nozzle support.
SupportRequest mixed_support(SupportStyle style, int interface_tool,
                             double interface_height_mm)
{
    SupportRequest request;
    request.style                    = style;
    request.body_tool                = 3;
    request.interface_tool           = interface_tool;
    request.body_layer_height_mm     = 0.40;
    request.interface_layer_height_mm = interface_height_mm;
    request.contact_distance_mm      = 0.20;
    request.supported_area_mm2       = 900.;
    request.minimum_contact_width_mm = 2.0;
    return request;
}

bool blocked(const SupportPlan &plan)
{
    for (const SupportIssue &issue : plan.issues)
        if (issue.severity == SupportIssueSeverity::Blocked)
            return true;
    return false;
}

} // namespace

TEST_CASE("Stage A/8: traditional, organic, soluble and mixed-tool supports each keep their "
          "own rules",
          "[stage-a]")
{
    SECTION("a traditional support puts its bulk on the coarse tool and its contact on the fine")
    {
        const SupportPlan plan =
            plan_mixed_nozzle_support(mixed_support(SupportStyle::Traditional, 0, 0.10),
                                      support_tools());
        REQUIRE(plan.error.empty());
        REQUIRE(plan.part(SupportPart::Body) != nullptr);
        REQUIRE(plan.part(SupportPart::Interface) != nullptr);

        // Each part is generated with its own tool's real nozzle, not forced
        // to the smallest on the machine.
        CHECK(plan.part(SupportPart::Body)->tool == 3);
        CHECK(plan.part(SupportPart::Body)->nozzle_diameter_mm == 0.60);
        CHECK(plan.part(SupportPart::Body)->layer_height_mm == 0.40);
        CHECK(plan.part(SupportPart::Interface)->tool == 0);
        CHECK(plan.part(SupportPart::Interface)->nozzle_diameter_mm == 0.20);
        CHECK(plan.part(SupportPart::Interface)->layer_height_mm == 0.10);

        // The raft was not asked for. It is still listed - so a caller can see
        // it was considered - and marked as not generated.
        REQUIRE(plan.part(SupportPart::Raft) != nullptr);
        CHECK_FALSE(plan.part(SupportPart::Raft)->generated);
        CHECK(plan.part(SupportPart::Body)->generated);

        // What this support costs in toolchanges is visible now rather than
        // discovered at export.
        CHECK(plan.toolchanges_per_interface_band > 0);
        CHECK(plan.is_valid());
    }

    SECTION("an organic support is planned by the same rules, and says which style it is")
    {
        const SupportPlan traditional =
            plan_mixed_nozzle_support(mixed_support(SupportStyle::Traditional, 0, 0.10),
                                      support_tools());
        const SupportPlan organic =
            plan_mixed_nozzle_support(mixed_support(SupportStyle::Organic, 0, 0.10),
                                      support_tools());
        REQUIRE(organic.error.empty());
        CHECK(organic.is_valid());

        // The style does not change which tool prints which part, nor the
        // heights: an organic support is a different geometry, not a different
        // set of tool rules.
        REQUIRE(organic.part(SupportPart::Body) != nullptr);
        CHECK(organic.part(SupportPart::Body)->tool ==
              traditional.part(SupportPart::Body)->tool);
        CHECK(organic.part(SupportPart::Interface)->layer_height_mm ==
              traditional.part(SupportPart::Interface)->layer_height_mm);
        CHECK(std::string(support_style_name(SupportStyle::Organic)) !=
              std::string(support_style_name(SupportStyle::Traditional)));
    }

    SECTION("a zero contact distance is only valid with a soluble interface")
    {
        SupportRequest touching  = mixed_support(SupportStyle::Organic, 0, 0.10);
        touching.contact_distance_mm = 0.;
        const SupportPlan mechanical =
            plan_mixed_nozzle_support(touching, support_tools());
        CHECK(blocked(mechanical));

        // The same support with the PVA tool on the interface is fine, because
        // it dissolves rather than being pulled off.
        touching.interface_tool           = 2;
        touching.interface_layer_height_mm = 0.20;
        const SupportPlan soluble = plan_mixed_nozzle_support(touching, support_tools());
        REQUIRE(soluble.error.empty());
        CHECK_FALSE(blocked(soluble));
        REQUIRE(soluble.part(SupportPart::Interface) != nullptr);
        CHECK(soluble.part(SupportPart::Interface)->material == "PVA");
    }

    SECTION("a part cannot be printed at a height its tool does not have")
    {
        // The 0.60 tool cannot print the 0.10 interface, and the plan says so
        // rather than quietly moving the work.
        SupportRequest impossible = mixed_support(SupportStyle::Organic, 3, 0.10);
        const SupportPlan plan = plan_mixed_nozzle_support(impossible, support_tools());
        CHECK(blocked(plan));
        bool named = false;
        for (const SupportIssue &issue : plan.issues)
            if (issue.part.has_value() && *issue.part == SupportPart::Interface)
                named = true;
        CHECK(named);
    }

    SECTION("a tool that is not on the machine is refused, never substituted")
    {
        SupportRequest absent  = mixed_support(SupportStyle::Organic, 7, 0.10);
        const SupportPlan plan = plan_mixed_nozzle_support(absent, support_tools());
        CHECK_FALSE(plan.is_valid());
        // Nothing was silently printed on some other tool instead.
        const SupportPartPlan *interface_plan = plan.part(SupportPart::Interface);
        if (interface_plan != nullptr && interface_plan->generated)
            CHECK(interface_plan->tool != 0);
    }
}

// --- non-touching objects with independent grids -----------------------------

namespace {

PrintableRegion object_at(size_t id, size_t parent, int tool, double x0, double max_z)
{
    PrintableRegion region;
    region.id            = id;
    region.kind          = RegionKind::Object;
    region.parent_object = parent;
    region.tool          = tool;
    region.min_x         = x0;
    region.max_x         = x0 + 20.;
    region.min_y         = 0.;
    region.max_y         = 20.;
    region.min_z         = 0.;
    region.max_z         = max_z;
    return region;
}

// The grid one object gets, given the tools that print it. Two objects on
// different tools have no reason to share a grid, and this is the arithmetic
// that says what each one's is: the finest height among its own tools.
double grid_for(const std::vector<double> &tool_heights)
{
    double finest = tool_heights.front();
    for (double height : tool_heights)
        finest = std::min(finest, height);
    return finest;
}

} // namespace

TEST_CASE("Stage A/5: two objects that do not touch get grids of their own", "[stage-a]")
{
    ComponentOptions options;
    options.separation_margin_mm = 30.;

    SECTION("far apart, they are two components and each may keep its own grid")
    {
        // 20 mm wide each, 80 mm apart: well past the margin.
        const std::vector<PrintableRegion> regions = { object_at(0, 0, 0, 0., 2.),
                                                       object_at(1, 1, 3, 100., 2.) };
        const ComponentDecomposition decomposition =
            decompose_components(regions, {}, options);
        REQUIRE(decomposition.error.empty());
        REQUIRE(decomposition.components.size() == 2);
        for (const DependencyComponent &component : decomposition.components)
            CHECK(component.verdict == IndependenceVerdict::Independent);
        CHECK(decomposition.batching_eligible);

        // Nothing forces the two to share a layer height: the object printed
        // only by the 0.60 tool keeps a 0.40 grid while the one printed by the
        // 0.20 keeps 0.10.
        CHECK(grid_for({ 0.10 }) == Catch::Approx(0.10));
        CHECK(grid_for({ 0.40 }) == Catch::Approx(0.40));
        // And an object printed by both is driven by its finer tool, not by
        // the other object's.
        CHECK(grid_for({ 0.40, 0.10 }) == Catch::Approx(0.10));
    }

    SECTION("close together, they are one component and the grids are not independent")
    {
        const std::vector<PrintableRegion> regions = { object_at(0, 0, 0, 0., 2.),
                                                       object_at(1, 1, 3, 25., 2.) };
        const ComponentDecomposition decomposition =
            decompose_components(regions, {}, options);
        REQUIRE(decomposition.components.size() == 1);
        CHECK_FALSE(decomposition.batching_eligible);
    }

    SECTION("looking separate is not being separate")
    {
        std::vector<PrintableRegion> regions = { object_at(0, 0, 0, 0., 2.),
                                                 object_at(1, 1, 3, 100., 2.) };
        // The second object's geometry was never actually examined - only its
        // bounding box. It is far away and it still does not count as proven.
        regions[1].geometry_analyzed = false;

        const ComponentDecomposition decomposition =
            decompose_components(regions, {}, options);
        REQUIRE(decomposition.components.size() == 2);
        bool unproven = false;
        for (const DependencyComponent &component : decomposition.components)
            if (component.verdict == IndependenceVerdict::Unproven)
                unproven = true;
        CHECK(unproven);
        CHECK_FALSE(decomposition.batching_eligible);
    }

    SECTION("a declared link between them keeps them together however far apart they are")
    {
        const std::vector<PrintableRegion> regions = { object_at(0, 0, 0, 0., 2.),
                                                       object_at(1, 1, 3, 100., 2.) };
        RegionLink bridge;
        bridge.a           = 0;
        bridge.b           = 1;
        bridge.kind        = LinkKind::Declared;
        bridge.description = "a support arch spans the two";

        const ComponentDecomposition decomposition =
            decompose_components(regions, { bridge }, options);
        REQUIRE(decomposition.components.size() == 1);
        CHECK_FALSE(decomposition.batching_eligible);
    }

    SECTION("the same two objects decompose the same way every time")
    {
        const std::vector<PrintableRegion> regions = { object_at(0, 0, 0, 0., 2.),
                                                       object_at(1, 1, 3, 100., 2.) };
        const ComponentDecomposition first  = decompose_components(regions, {}, options);
        const std::vector<PrintableRegion> reversed(regions.rbegin(), regions.rend());
        const ComponentDecomposition second = decompose_components(reversed, {}, options);

        REQUIRE(first.components.size() == second.components.size());
        for (size_t i = 0; i < first.components.size(); ++i) {
            CHECK(first.components[i].id == second.components[i].id);
            CHECK(first.components[i].regions == second.components[i].regions);
        }
    }
}

// --- curved and sloped painted boundaries -----------------------------------

namespace {

SurfaceBand band(double z0, double z1, std::optional<double> slope_deg, double curvature,
                 bool visible = true)
{
    SurfaceBand value;
    value.z_bottom_mm = z0;
    value.z_top_mm    = z1;
    value.slope_deg   = slope_deg;
    value.curvature   = curvature;
    value.visible     = visible;
    return value;
}

ToolLayerGrid grid(int tool, double nozzle_mm, double min_mm, double max_mm)
{
    ToolLayerGrid value;
    value.tool                = tool;
    value.nozzle_mm           = nozzle_mm;
    value.min_layer_height_mm = min_mm;
    value.max_layer_height_mm = max_mm;
    value.change_cost_s       = 25.;
    return value;
}

// Whether any layer of any schedule ends exactly on this z.
bool lands_on(const AdaptiveLayerPlan &plan, double z_mm, double tolerance = 1e-6)
{
    for (const ToolSchedule &schedule : plan.schedules)
        for (const PlannedLayer &layer : schedule.layers)
            if (std::abs(layer.z_top_mm - z_mm) <= tolerance)
                return true;
    return false;
}

} // namespace

TEST_CASE("Stage A/6: a painted boundary on a curved, sloped surface is followed, not "
          "approximated",
          "[stage-a]")
{
    // A dome: near-vertical at the bottom, shallowing towards the top, with a
    // painted colour change partway up. The band edges are placed where the
    // fine tool can actually land - a band edge is itself a boundary the
    // schedule has to reach, and one that cannot be reached refuses the
    // schedule rather than being nudged.
    const std::vector<SurfaceBand> bands = {
        band(0.00, 0.96, 85., 0.1),  // almost a wall
        band(0.96, 1.44, 40., 0.6),  // curving over
        band(1.44, 2.40, 12., 0.9),  // nearly flat, tightly curved
    };

    // 1.20 is not a multiple of the tool's preferred height; the schedule has
    // to reach it anyway.
    LayerConstraint painted;
    painted.z_mm        = 1.20;
    painted.kind        = LayerConstraintKind::ColorBoundary;
    painted.description = "the painted line between the two colours";

    const std::vector<ToolLayerGrid> fine_only = { grid(0, 0.20, 0.06, 0.16) };
    AdaptiveLayerOptions             options;

    SECTION("a layer ends exactly on the painted boundary, whatever height the tool prefers")
    {
        const AdaptiveLayerPlan plan =
            plan_adaptive_layers(bands, fine_only, { painted }, options);
        REQUIRE(plan.error.empty());
        REQUIRE(plan.schedules.size() == 1);
        CHECK(plan.schedules[0].verdict == LayerVerdict::Planned);
        CHECK(lands_on(plan, 1.20));

        // And the layer that lands there says it did so because of the
        // constraint, not because the arithmetic happened to work out.
        bool declared = false;
        for (const PlannedLayer &layer : plan.schedules[0].layers)
            if (std::abs(layer.z_top_mm - 1.20) <= 1e-6 && layer.at_constraint)
                declared = true;
        CHECK(declared);

        // The band edges are landed on too.
        CHECK(lands_on(plan, 0.96));
        CHECK(lands_on(plan, 1.44));
    }

    SECTION("the shallow, tightly curved band gets finer layers than the near-vertical one")
    {
        const AdaptiveLayerPlan plan =
            plan_adaptive_layers(bands, fine_only, { painted }, options);
        REQUIRE(plan.schedules.size() == 1);

        double tallest_on_wall = 0., tallest_on_dome = 0.;
        for (const PlannedLayer &layer : plan.schedules[0].layers) {
            if (layer.z_top_mm <= 0.96 + 1e-9)
                tallest_on_wall = std::max(tallest_on_wall, layer.height_mm);
            else if (layer.z_bottom_mm >= 1.44 - 1e-9)
                tallest_on_dome = std::max(tallest_on_dome, layer.height_mm);
        }
        REQUIRE(tallest_on_wall > 0.);
        REQUIRE(tallest_on_dome > 0.);
        // A vertical wall does not stair-step; a shallow curved dome does.
        CHECK(tallest_on_dome < tallest_on_wall);
    }

    SECTION("a band whose slope was never measured claims no surface error")
    {
        std::vector<SurfaceBand> unmeasured = bands;
        unmeasured[1].slope_deg.reset();

        CHECK_FALSE(estimate_surface_error(unmeasured[1], 0.10).has_value());

        const AdaptiveLayerPlan plan =
            plan_adaptive_layers(unmeasured, fine_only, { painted }, options);
        bool unclaimed = false;
        for (const PlannedLayer &layer : plan.schedules[0].layers)
            if (layer.z_bottom_mm >= 0.96 - 1e-9 && layer.z_top_mm <= 1.44 + 1e-9 &&
                ! layer.estimated_surface_error_mm.has_value())
                unclaimed = true;
        CHECK(unclaimed);
        // The painted boundary is still honoured; not knowing the slope is not
        // a reason to lose the colour change.
        CHECK(lands_on(plan, 1.20));
    }

    SECTION("a hidden curved surface is not refined for looks")
    {
        std::vector<SurfaceBand> hidden = bands;
        for (SurfaceBand &b : hidden)
            b.visible = false;

        const AdaptiveLayerPlan visible_plan =
            plan_adaptive_layers(bands, fine_only, { painted }, options);
        const AdaptiveLayerPlan hidden_plan =
            plan_adaptive_layers(hidden, fine_only, { painted }, options);

        CHECK(hidden_plan.schedules[0].layers.size() <
              visible_plan.schedules[0].layers.size());
        // Even hidden, the painted boundary is a boundary.
        CHECK(lands_on(hidden_plan, 1.20));
    }

    SECTION("two boundaries close together are both landed on, not merged")
    {
        LayerConstraint second;
        second.z_mm        = 1.28;
        second.kind        = LayerConstraintKind::ColorBoundary;
        second.description = "a second painted line just above the first";

        const AdaptiveLayerPlan plan =
            plan_adaptive_layers(bands, fine_only, { painted, second }, options);
        CHECK(lands_on(plan, 1.20));
        CHECK(lands_on(plan, 1.28));
    }

    SECTION("two boundaries a coarse tool cannot separate: it says which one it gave up")
    {
        // The 0.60 tool cannot put a 0.08 mm layer between 1.20 and 1.28. Two
        // boundaries that close cannot both be layer ends on this tool, and
        // the schedule names the one it gave up rather than reporting that
        // both landed.
        LayerConstraint second;
        second.z_mm = 1.28;
        second.kind = LayerConstraintKind::ColorBoundary;

        const std::vector<ToolLayerGrid> coarse_only = { grid(3, 0.60, 0.20, 0.48) };
        const AdaptiveLayerPlan          plan =
            plan_adaptive_layers(bands, coarse_only, { painted, second }, options);
        REQUIRE(plan.schedules.size() == 1);

        // The upper boundary is the one kept; the lower is given up, by name.
        CHECK_FALSE(lands_on(plan, 1.20));
        bool named = false;
        for (const std::string &note : plan.schedules[0].notes)
            if (note.find("1.20") != std::string::npos &&
                note.find("no longer a layer end") != std::string::npos)
                named = true;
        CHECK(named);

        // No layer claims to end on a boundary it does not end on.
        for (const PlannedLayer &layer : plan.schedules[0].layers)
            if (layer.reason.find("ends exactly on the boundary at 1.20") != std::string::npos)
                CHECK(std::abs(layer.z_top_mm - 1.20) <= 1e-6);
    }

    SECTION("the same surface plans the same way every time")
    {
        const AdaptiveLayerPlan first =
            plan_adaptive_layers(bands, fine_only, { painted }, options);
        const AdaptiveLayerPlan second =
            plan_adaptive_layers(bands, fine_only, { painted }, options);
        REQUIRE(first.schedules.size() == second.schedules.size());
        for (size_t i = 0; i < first.schedules.size(); ++i) {
            REQUIRE(first.schedules[i].layers.size() == second.schedules[i].layers.size());
            for (size_t j = 0; j < first.schedules[i].layers.size(); ++j)
                CHECK(first.schedules[i].layers[j].z_top_mm ==
                      second.schedules[i].layers[j].z_top_mm);
        }
    }
}
