///|/ TigerSlicer Stage A gate: the cross-phase acceptance criteria.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_StageAcceptance_hpp_
#define slic3r_StageAcceptance_hpp_

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

// The twenty-one cross-phase criteria Stage A is accepted against. They are
// listed here rather than only in a document so that "the gate passed" is
// something a program can be made to answer, and so that a criterion cannot
// quietly disappear from the list by being left out of a report.
enum class AcceptanceCriterion : uint8_t
{
    WallsAndInfillAcrossTools,
    AdjacentFineAndCoarseInterfaces,
    CoarseInfillWithRatioSchedule,
    FourToolOwnershipAndOverrides,
    IndependentGridsForSeparateObjects,
    CurvedPaintedBoundaries,
    SolidLayersOverCoarseInfill,
    SupportKinds,
    NeverAboveAToolsMaximumHeight,
    ActiveToolWidthsAndFlowLimits,
    ReducedChangeCountWhenValid,
    SafeRejectionWhenNotProvable,
    NoUnsafeDownwardMotion,
    DeterministicDependencyScheduling,
    SaveReloadAndOldProjects,
    DisabledFeatureLegacyBehaviour,
    TelemetryIdentityAndIncompleteJobs,
    DirectionalMaterialTransitions,
    CalibrationVersioningAndRollback,
    DigitalVerificationOfBadGcode,
    CachedVersusCleanEquivalence,
};

const char *acceptance_criterion_name(AcceptanceCriterion criterion);

// Every criterion, in the specification's order.
const std::vector<AcceptanceCriterion> &all_criteria();

// How far a criterion has actually been taken. The distinctions here are the
// point of the file. A criterion whose parts are each tested in isolation is
// not the same as one whose named scenario has been run end to end, and
// neither is the same as one that has met a printer.
enum class CoverageStatus : uint8_t
{
    // The criterion's own named scenario is run as one, by an automated test,
    // against synthetic inputs.
    CoveredByScenario,
    // Every decision the criterion depends on is tested, but in separate
    // suites; the scenario itself has not been run as one thing. Real
    // coverage, and deliberately not counted as full.
    CoveredByComponentTests,
    // True by construction - the code cannot express the failure - and the
    // construction is named.
    CoveredByConstruction,
    // Cannot be shown in this environment. The reason is recorded and the
    // criterion is not claimed to be met.
    Blocked,
    // Nobody has said. Always a gate failure: an unexamined criterion is not a
    // passing one.
    Unexamined,
};

const char *coverage_status_name(CoverageStatus status);

struct CriterionCoverage
{
    AcceptanceCriterion criterion { AcceptanceCriterion::WallsAndInfillAcrossTools };
    CoverageStatus      status { CoverageStatus::Unexamined };

    // The Catch2 tag or tags the evidence lives under. Empty is a defect for
    // anything claiming to be covered: a claim with no evidence behind it is
    // the thing this file exists to prevent.
    std::vector<std::string> tags;

    // What was actually shown, or why it could not be.
    std::string note;

    // True when the criterion has been exercised against a physical printer.
    // Nothing in this repository sets it: there is no hardware.
    bool hardware_verified { false };
};

enum class GateVerdict : uint8_t
{
    // Every criterion's own scenario has been run, or is met by construction.
    Passed,
    // The same, except that something cannot be shown in this environment.
    PassedWithBlockers,
    // Nothing is unexamined or unevidenced, but some criteria rest only on
    // their parts being tested separately. Not passed; as far as it has got.
    Partial,
    Failed,
};

const char *gate_verdict_name(GateVerdict verdict);

struct GateReport
{
    GateVerdict verdict { GateVerdict::Failed };

    size_t covered_by_scenario { 0 };
    size_t covered_by_component_tests { 0 };
    size_t covered_by_construction { 0 };
    size_t blocked { 0 };
    size_t unexamined { 0 };

    // Criteria that were never reported on at all. Distinct from Unexamined,
    // which is at least an admission.
    std::vector<AcceptanceCriterion> missing;
    // Criteria claiming coverage with no evidence attached.
    std::vector<AcceptanceCriterion> unevidenced;
    // Criteria reported more than once, which makes the report ambiguous
    // rather than thorough.
    std::vector<AcceptanceCriterion> duplicated;

    std::vector<std::string> blockers;

    // Always false here, and stated rather than left to be inferred: no
    // criterion in this repository has been met against a physical printer.
    bool any_hardware_verified { false };

    std::string summary;
};

// Judge a coverage report against the criterion list. Refuses to pass when a
// criterion is missing, duplicated, unexamined, or claims coverage without
// naming where the evidence is, and refuses to report better than Partial
// while any criterion rests only on its parts being tested separately.
GateReport judge_stage_a(const std::vector<CriterionCoverage> &coverage);

// The coverage this repository actually has, as of Stage A's last numbered
// phase. Kept in the library rather than in a test so that it is compiled
// against the criterion list and cannot drift out of sync with it silently.
std::vector<CriterionCoverage> recorded_stage_a_coverage();

} // namespace Slic3r

#endif // slic3r_StageAcceptance_hpp_
