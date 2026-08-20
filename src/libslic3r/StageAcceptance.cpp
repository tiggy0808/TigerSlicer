///|/ TigerSlicer Stage A gate: the cross-phase acceptance criteria.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "StageAcceptance.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace Slic3r {

const char *acceptance_criterion_name(AcceptanceCriterion criterion)
{
    switch (criterion) {
    case AcceptanceCriterion::WallsAndInfillAcrossTools:
        return "T0 0.20 mm walls with T3 0.40 mm infill";
    case AcceptanceCriterion::AdjacentFineAndCoarseInterfaces:
        return "T0 0.20 beside T1 0.10 interfaces";
    case AcceptanceCriterion::CoarseInfillWithRatioSchedule:
        return "T3 0.60 infill with a 3:1 schedule";
    case AcceptanceCriterion::FourToolOwnershipAndOverrides:
        return "four-colour and four-tool ownership with feature overrides";
    case AcceptanceCriterion::IndependentGridsForSeparateObjects:
        return "non-touching objects with independent grids";
    case AcceptanceCriterion::CurvedPaintedBoundaries:
        return "curved and sloped painted boundaries";
    case AcceptanceCriterion::SolidLayersOverCoarseInfill:
        return "solid and top layers over coarse infill";
    case AcceptanceCriterion::SupportKinds:
        return "traditional, organic, soluble and mixed-tool supports";
    case AcceptanceCriterion::NeverAboveAToolsMaximumHeight:
        return "no layer height above a tool's maximum";
    case AcceptanceCriterion::ActiveToolWidthsAndFlowLimits:
        return "correct active-tool widths and flow limits";
    case AcceptanceCriterion::ReducedChangeCountWhenValid:
        return "a reduced toolchange count when economically valid";
    case AcceptanceCriterion::SafeRejectionWhenNotProvable:
        return "safe rejection when optimization is not worthwhile or not provable";
    case AcceptanceCriterion::NoUnsafeDownwardMotion:
        return "no unsafe downward motion or occupied-volume collision";
    case AcceptanceCriterion::DeterministicDependencyScheduling:
        return "deterministic dependency scheduling";
    case AcceptanceCriterion::SaveReloadAndOldProjects:
        return "save, reload, and backward-compatible old projects";
    case AcceptanceCriterion::DisabledFeatureLegacyBehaviour:
        return "legacy behaviour when the feature is disabled";
    case AcceptanceCriterion::TelemetryIdentityAndIncompleteJobs:
        return "telemetry identity and incomplete-job handling";
    case AcceptanceCriterion::DirectionalMaterialTransitions:
        return "directional material transitions";
    case AcceptanceCriterion::CalibrationVersioningAndRollback:
        return "calibration versioning and rollback";
    case AcceptanceCriterion::DigitalVerificationOfBadGcode:
        return "digital verification of invalid feedrate and wrong-tool cases";
    case AcceptanceCriterion::CachedVersusCleanEquivalence:
        return "cached versus clean equivalence";
    }
    return "unknown criterion";
}

const std::vector<AcceptanceCriterion> &all_criteria()
{
    static const std::vector<AcceptanceCriterion> criteria = {
        AcceptanceCriterion::WallsAndInfillAcrossTools,
        AcceptanceCriterion::AdjacentFineAndCoarseInterfaces,
        AcceptanceCriterion::CoarseInfillWithRatioSchedule,
        AcceptanceCriterion::FourToolOwnershipAndOverrides,
        AcceptanceCriterion::IndependentGridsForSeparateObjects,
        AcceptanceCriterion::CurvedPaintedBoundaries,
        AcceptanceCriterion::SolidLayersOverCoarseInfill,
        AcceptanceCriterion::SupportKinds,
        AcceptanceCriterion::NeverAboveAToolsMaximumHeight,
        AcceptanceCriterion::ActiveToolWidthsAndFlowLimits,
        AcceptanceCriterion::ReducedChangeCountWhenValid,
        AcceptanceCriterion::SafeRejectionWhenNotProvable,
        AcceptanceCriterion::NoUnsafeDownwardMotion,
        AcceptanceCriterion::DeterministicDependencyScheduling,
        AcceptanceCriterion::SaveReloadAndOldProjects,
        AcceptanceCriterion::DisabledFeatureLegacyBehaviour,
        AcceptanceCriterion::TelemetryIdentityAndIncompleteJobs,
        AcceptanceCriterion::DirectionalMaterialTransitions,
        AcceptanceCriterion::CalibrationVersioningAndRollback,
        AcceptanceCriterion::DigitalVerificationOfBadGcode,
        AcceptanceCriterion::CachedVersusCleanEquivalence,
    };
    return criteria;
}

const char *coverage_status_name(CoverageStatus status)
{
    switch (status) {
    case CoverageStatus::CoveredByScenario:      return "covered by scenario";
    case CoverageStatus::CoveredByComponentTests:
        return "covered by component tests only";
    case CoverageStatus::CoveredByConstruction:  return "covered by construction";
    case CoverageStatus::Blocked:               return "blocked";
    case CoverageStatus::Unexamined:            return "unexamined";
    }
    return "unexamined";
}

const char *gate_verdict_name(GateVerdict verdict)
{
    switch (verdict) {
    case GateVerdict::Passed:             return "passed";
    case GateVerdict::PassedWithBlockers: return "passed with blockers";
    case GateVerdict::Partial:            return "partial";
    case GateVerdict::Failed:             return "failed";
    }
    return "failed";
}

GateReport judge_stage_a(const std::vector<CriterionCoverage> &coverage)
{
    GateReport report;

    std::map<AcceptanceCriterion, size_t> counts;
    for (const CriterionCoverage &entry : coverage)
        ++counts[entry.criterion];

    std::set<AcceptanceCriterion> known(all_criteria().begin(), all_criteria().end());
    for (const auto &pair : counts) {
        if (! known.count(pair.first))
            continue;
        if (pair.second > 1)
            report.duplicated.push_back(pair.first);
    }

    for (AcceptanceCriterion criterion : all_criteria())
        if (! counts.count(criterion))
            report.missing.push_back(criterion);

    for (const CriterionCoverage &entry : coverage) {
        if (! known.count(entry.criterion))
            continue;

        switch (entry.status) {
        case CoverageStatus::CoveredByScenario: ++report.covered_by_scenario; break;
        case CoverageStatus::CoveredByComponentTests:
            ++report.covered_by_component_tests;
            break;
        case CoverageStatus::CoveredByConstruction: ++report.covered_by_construction; break;
        case CoverageStatus::Blocked:
            ++report.blocked;
            report.blockers.push_back(std::string(acceptance_criterion_name(entry.criterion)) +
                                      ": " +
                                      (entry.note.empty() ? "no reason recorded" : entry.note));
            break;
        case CoverageStatus::Unexamined: ++report.unexamined; break;
        }

        const bool claims_coverage = entry.status == CoverageStatus::CoveredByScenario ||
                                     entry.status == CoverageStatus::CoveredByComponentTests ||
                                     entry.status == CoverageStatus::CoveredByConstruction;
        if (claims_coverage && (entry.tags.empty() || entry.note.empty()))
            report.unevidenced.push_back(entry.criterion);

        if (entry.hardware_verified)
            report.any_hardware_verified = true;
    }

    std::sort(report.duplicated.begin(), report.duplicated.end());
    std::sort(report.unevidenced.begin(), report.unevidenced.end());
    report.unevidenced.erase(std::unique(report.unevidenced.begin(), report.unevidenced.end()),
                             report.unevidenced.end());

    const bool structurally_sound = report.missing.empty() && report.duplicated.empty() &&
                                    report.unevidenced.empty() && report.unexamined == 0;

    if (! structurally_sound) {
        report.verdict = GateVerdict::Failed;
        report.summary = "the gate does not pass: ";
        if (! report.missing.empty())
            report.summary += std::to_string(report.missing.size()) +
                              " criterion(s) were not reported on; ";
        if (! report.duplicated.empty())
            report.summary += std::to_string(report.duplicated.size()) +
                              " criterion(s) were reported more than once; ";
        if (! report.unevidenced.empty())
            report.summary += std::to_string(report.unevidenced.size()) +
                              " criterion(s) claim coverage without naming the evidence; ";
        if (report.unexamined > 0)
            report.summary +=
                std::to_string(report.unexamined) + " criterion(s) have not been examined; ";
        report.summary.erase(report.summary.find_last_not_of("; ") + 1);
    } else if (report.covered_by_component_tests > 0) {
        report.verdict = GateVerdict::Partial;
        report.summary = std::to_string(report.covered_by_component_tests) +
                         " criterion(s) rest on their parts being tested separately rather than "
                         "on the named scenario having been run as one thing; the gate is not "
                         "passed";
        if (report.blocked > 0)
            report.summary += ", and " + std::to_string(report.blocked) +
                              " are blocked besides";
    } else if (report.blocked > 0) {
        report.verdict = GateVerdict::PassedWithBlockers;
        report.summary = "everything that can be shown in this environment has been, but " +
                         std::to_string(report.blocked) +
                         " criterion(s) are blocked and are not claimed to be met";
    } else {
        report.verdict = GateVerdict::Passed;
        report.summary = "all " + std::to_string(all_criteria().size()) +
                         " criteria are covered with evidence named";
    }

    if (! report.any_hardware_verified)
        report.summary += ". No criterion has been verified against a physical printer";

    return report;
}

std::vector<CriterionCoverage> recorded_stage_a_coverage()
{
    // What is actually true of this repository. Each entry names the Catch2
    // tags its evidence lives under, so a reader can go and run it. The
    // difference between CoveredByScenario and CoveredByComponentTests is the
    // difference between having run the criterion's own scenario and having
    // tested each decision it depends on somewhere else, and it is not
    // smoothed over here.
    return {
        { AcceptanceCriterion::WallsAndInfillAcrossTools,
          CoverageStatus::CoveredByScenario,
          { "[independent-tool-layers]", "[tool-assignment]", "[flow-curves]" },
          "walls on tool 0 at 0.20 and sparse infill on tool 3 at 0.40 are scheduled together "
          "as one two-to-one run, producing the synchronization planes the coarse runs need and "
          "a deterministic order over both tools",
          false },
        { AcceptanceCriterion::AdjacentFineAndCoarseInterfaces,
          CoverageStatus::CoveredByScenario,
          { "[independent-tool-layers]", "[detail-decomposition]", "[support-interface]" },
          "tool 0 at 0.20 beside tool 1 at 0.10 synchronizes to 0.10 with the ratio checked for "
          "integrality rather than assumed, and the boundary between the two owners becomes an "
          "explicit interface requirement",
          false },
        { AcceptanceCriterion::CoarseInfillWithRatioSchedule,
          CoverageStatus::CoveredByScenario,
          { "[independent-tool-layers]", "[adaptive-layers]" },
          "tool 3 on a three-to-one ratio over a 0.20 analysis grid produces native 0.60 slabs, "
          "split at forced synchronization planes without exceeding the tool's maximum, and an "
          "extrusion above that maximum is rejected by validation",
          false },
        { AcceptanceCriterion::FourToolOwnershipAndOverrides,
          CoverageStatus::CoveredByScenario,
          { "[tool-ownership]", "[independent-tool-layers]", "[tool-assignment]" },
          "the full precedence - override, painted, volume, object, default - is run over a "
          "four-tool profile whose per-tool nozzle limits and feature permissions are "
          "preserved, with an override outranking a feature preference and displayed colour "
          "never affecting ownership",
          false },
        { AcceptanceCriterion::IndependentGridsForSeparateObjects,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[dependency-components]", "[independent-tool-layers]" },
          "two objects 80 mm apart decompose into two proven-independent components and each "
          "keeps the grid its own tools imply, driven by its own finer tool and not by the "
          "other object; brought within the margin they become one component and batching is "
          "refused; a declared link keeps them together however far apart they are; an "
          "unexamined geometry leaves its component unproven however far away it looks; and "
          "the decomposition is the same whichever order the objects are listed in",
          false },
        { AcceptanceCriterion::CurvedPaintedBoundaries,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[adaptive-layers]", "[detail-decomposition]", "[seam-optimizer]" },
          "a dome that is near-vertical at the bottom and shallow and tightly curved at the "
          "top is scheduled with a painted colour change partway up at a height the tool does "
          "not prefer: the layer ends exactly on it and says it did so because of the "
          "constraint, the curved band gets finer layers than the wall, an unmeasured slope "
          "claims no surface error while still honouring the boundary, a hidden surface is "
          "not refined for looks, two boundaries a coarse tool cannot separate leave it "
          "naming the one it gave up, and no layer claims to end on a boundary it does not",
          false },
        { AcceptanceCriterion::SolidLayersOverCoarseInfill,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[structural-infill]", "[bridge-planner]" },
          "the first solid layer over the 0.60 tool sparse infill is judged as the span it is: "
          "held at the perimeter and unsupported in the middle it asks for support first, over "
          "solid material it is simply planned on the coarse tool, the visible top surface "
          "above takes the fine tool and pays for the toolchange, a top surface landing on a "
          "refused layer is refused too, and an unmeasured anchor is unproven until somebody "
          "says to trust it",
          false },
        { AcceptanceCriterion::SupportKinds,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[support-interface]", "[mixed-support]" },
          "a mixed-nozzle support puts its bulk on the 0.60 and its contact layer on the 0.20, "
          "each generated with its own tool nozzle and height rather than forced to the "
          "smallest on the machine; the organic style plans by the same tool rules as the "
          "traditional one; a zero contact distance is blocked for a mechanical interface and "
          "allowed for a PVA one; a part is never moved to a tool that cannot print its "
          "height, and a tool that is not on the machine is refused rather than substituted",
          false },
        { AcceptanceCriterion::NeverAboveAToolsMaximumHeight,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[adaptive-layers]", "[gcode-twin]", "[tool-assignment]" },
          "a layer thicker than the machine declares it can print is found in exported g-code "
          "by the twin, and a height outside a tool's range rules that tool out when planning",
          false },
        { AcceptanceCriterion::ActiveToolWidthsAndFlowLimits,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[flow-curves]", "[pressure-planner]" },
          "the four-tool profile is calibrated tool by tool and every extrusion resolved "
          "against the tool actually printing it: each gets its own interpolated limit, the "
          "configured constant stays the ceiling, the uncalibrated 0.60 falls back and says "
          "which tool, an over-limit extrusion is flagged rather than clipped, and no curve is "
          "borrowed across a nozzle, material or width it was not measured over",
          false },
        { AcceptanceCriterion::ReducedChangeCountWhenValid,
          CoverageStatus::CoveredByScenario,
          { "[batch-acceptance]", "[bounded-batching]", "[toolchange-optimizer]" },
          "the two-tower reference case falls from 19 toolchanges to 3 against both baselines, "
          "measured the same way, as case 25F/1",
          false },
        { AcceptanceCriterion::SafeRejectionWhenNotProvable,
          CoverageStatus::CoveredByScenario,
          { "[batch-acceptance]", "[batch-semantics]", "[descent-proof]" },
          "geometry in the corridor, hardware in the corridor, a toolhead described only as far "
          "as its nozzle, and an uncharacterized calibration are each rejected in their own "
          "case in the 25F set",
          false },
        { AcceptanceCriterion::NoUnsafeDownwardMotion,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[descent-proof]", "[swept-collision]", "[gcode-twin]" },
          "every downward move bounded batching produces is turned into g-code and interpreted "
          "by the twin, which reports no unsafe-z finding for the proven descents and does "
          "report one when a descent is moved below the declared safe height",
          false },
        { AcceptanceCriterion::DeterministicDependencyScheduling,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[slice-cache]", "[dependency-components]" },
          "the same graph resolves to the same order and the same keys however its nodes are "
          "presented, and the same schedule comes out of the same components every time",
          false },
        { AcceptanceCriterion::SaveReloadAndOldProjects,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[print-manifest]", "[profile-versioning]" },
          "a manifest round-trips through its own text without changing its digest, and a "
          "project referencing an older exact revision still resolves as usable rather than "
          "being moved to the newest",
          false },
        { AcceptanceCriterion::DisabledFeatureLegacyBehaviour,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[bounded-batching]", "[batch-preview]", "[slice-cache]" },
          "with batching disabled the schedule is the ordinary by-layer one with no downward "
          "moves, and a disabled cache misses every lookup and stores nothing rather than "
          "becoming a smaller cache",
          false },
        { AcceptanceCriterion::TelemetryIdentityAndIncompleteJobs,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[telemetry-import]", "[print-manifest]" },
          "a job is matched to the manifest digest of the print it came from and to no other - "
          "another machine, a manifest one character different, and a log that never said are "
          "three distinct answers - it round-trips through its own document form, an "
          "interrupted job is excluded from the evidence by name while three finished ones "
          "carry a nine-sample proposal, refusing the proposal changes nothing and accepting "
          "it produces a manifest with a different digest",
          false },
        { AcceptanceCriterion::DirectionalMaterialTransitions,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[transition-matrix]" },
          "a four-material sequence across three tools is priced leg by leg: the return trip "
          "costs more than the outbound one and is not inferred from it, removing one "
          "direction blocks export rather than borrowing the other, an incompatible pair names "
          "what is responsible, and one unpriced leg withholds the whole time total while the "
          "measured material totals stand",
          false },
        { AcceptanceCriterion::CalibrationVersioningAndRollback,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[profile-versioning]", "[calibration-map]" },
          "a calibration profile versions like any other, rolling back adds a revision "
          "restoring an earlier one rather than erasing what came between, and the restored "
          "values are unvalidated until checked again",
          false },
        { AcceptanceCriterion::DigitalVerificationOfBadGcode,
          CoverageStatus::CoveredByScenario,
          { "[stage-a]", "[gcode-twin]" },
          "a zero feedrate and extrusion on a tool that was never selected are both found in "
          "otherwise well-formed g-code, and the same file without those faults is acceptable",
          false },
        { AcceptanceCriterion::CachedVersusCleanEquivalence,
          CoverageStatus::CoveredByScenario,
          { "[slice-cache]", "[stage-a]" },
          "an incremental run over a diamond graph reuses exactly the retained stages, "
          "recomputes exactly the invalidated ones, and agrees with a clean run stage for stage",
          false },
    };
}

} // namespace Slic3r
