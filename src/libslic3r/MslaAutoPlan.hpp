///|/ TigerSlicer Phase AUTO3: the MSLA Auto Prepare & Slice planner's mapping
///|/ into the shared optimizer.
///|/
///|/ The MSLA planner is separate from the FDM one and shares no print physics.
///|/ Like AUTO2 it reuses existing engines rather than re-deciding: it consumes
///|/ the resin issue analysis (UV4's ResinAnalysis) and an exposure-preset
///|/ lookup, and maps them into the AUTO1 constraint vocabulary. Its defining
///|/ safety rule: exposure comes only from an audited compatible resin/machine
///|/ preset - never derived from geometry, never substituted from a similar
///|/ resin - and a missing preset stops before slicing rather than guessing.
///|/ Testable libslic3r logic; it slices nothing and links no UVtools code.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_MslaAutoPlan_hpp_
#define slic3r_MslaAutoPlan_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include "AutoPlanOptimizer.hpp"
#include "ResinIssueReport.hpp"

namespace Slic3r {

// Where a candidate's exposure came from. Only an audited preset is safe; the
// others are each a distinct failure the planner must not let through.
enum class MslaExposureSource : uint8_t
{
    // From an audited preset for exactly this printer + resin. The only safe
    // source.
    AuditedPreset,
    // No audited preset exists for this printer + resin. Auto planning must stop
    // and request the preset or a calibration workflow - a requirement gap, not
    // a wrong value.
    None,
    // A preset for a *similar* resin was substituted. Forbidden: exposure must
    // not be borrowed across resins by similarity.
    SubstitutedBySimilarity,
    // Exposure was derived from geometry alone. Forbidden.
    DerivedFromGeometry,
};

const char *msla_exposure_source_name(MslaExposureSource source);

// Whether a geometric validation was actually performed, and its result. Used
// for trapped-resin / drain-hole checks: "not validated" is Unknown, not a
// silent pass.
enum class ValidationState3 : uint8_t
{
    NotValidated,
    Passed,
    Failed,
};

const char *validation_state3_name(ValidationState3 state);

// One MSLA candidate configuration, after the resin analysis and preset lookup
// ran. The objective values come from the estimators; nothing here recomputes
// print physics.
struct MslaCandidatePlan
{
    std::string id;
    std::string summary;

    // The resin issue analysis for this candidate (UV4). Any blocking issue -
    // island, resin trap, suction cup, empty/invalid layer - eliminates the
    // candidate. A non-complete analysis leaves it unproven.
    ResinAnalysis analysis;

    // The exposure source. Only AuditedPreset is safe.
    MslaExposureSource exposure_source { MslaExposureSource::None };
    std::string        resin;
    std::string        printer;

    // Whether the layer height lies inside the printer, resin, profile and
    // geometry intersection. False is a violation.
    bool layer_height_supported { true };

    // Trapped-resin and drain-hole validation for a hollowed part.
    ValidationState3 trapped_resin { ValidationState3::Passed };

    // Objective values from the estimators.
    double predicted_quality { 0. };     // higher better (detail / surface)
    double predicted_support_burden { 0. }; // lower better
    double predicted_time_s { 0. };      // lower better
    double predicted_peel_risk { 0. };   // lower better
};

// Map one MSLA candidate configuration into an AutoPlanCandidate.
//
// exposure_source:
//   AuditedPreset            -> satisfied
//   None                     -> Unknown (stop and request the preset / calibration)
//   SubstitutedBySimilarity  -> Violated (never borrow exposure across resins)
//   DerivedFromGeometry      -> Violated (never derive exposure from geometry)
// Any Blocking resin issue   -> Violated (an unsafe layer stack is not planned)
// A non-complete analysis    -> Unknown (safety not established)
// layer height unsupported   -> Violated
// trapped-resin NotValidated -> Unknown; Failed -> Violated
AutoPlanCandidate map_msla_candidate(const MslaCandidatePlan &plan);

AutoPlanRequest build_msla_request(const std::vector<MslaCandidatePlan> &candidates,
                                   const ObjectiveVector                &objectives,
                                   uint64_t                              seed);

// The MSLA objective vector for a slider position, 0 (fastest) .. 1 (finest).
// Distinct weights from FDM: MSLA trades detail against support burden, peel
// risk and time, not toolchanges. The slider moves only these soft weights.
ObjectiveVector msla_objectives_for_slider(double quality_to_speed);

} // namespace Slic3r

#endif // slic3r_MslaAutoPlan_hpp_
