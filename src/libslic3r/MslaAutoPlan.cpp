///|/ TigerSlicer Phase AUTO3: the MSLA Auto Prepare & Slice planner's mapping
///|/ into the shared optimizer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "MslaAutoPlan.hpp"

namespace Slic3r {

const char *msla_exposure_source_name(MslaExposureSource source)
{
    switch (source) {
    case MslaExposureSource::AuditedPreset:           return "audited preset";
    case MslaExposureSource::None:                    return "none";
    case MslaExposureSource::SubstitutedBySimilarity: return "substituted by similarity";
    case MslaExposureSource::DerivedFromGeometry:     return "derived from geometry";
    }
    return "none";
}

const char *validation_state3_name(ValidationState3 state)
{
    switch (state) {
    case ValidationState3::NotValidated: return "not validated";
    case ValidationState3::Passed:       return "passed";
    case ValidationState3::Failed:       return "failed";
    }
    return "not validated";
}

namespace {

ConstraintResult satisfied(const std::string &name, const std::string &detail = "")
{
    return { name, ConstraintOutcome::Satisfied, detail, "" };
}

ConstraintResult violated(const std::string &name, const std::string &detail)
{
    return { name, ConstraintOutcome::Violated, detail, "" };
}

ConstraintResult unknown(const std::string &name, const std::string &detail,
                         const std::string &datum)
{
    return { name, ConstraintOutcome::Unknown, detail, datum };
}

} // namespace

AutoPlanCandidate map_msla_candidate(const MslaCandidatePlan &plan)
{
    AutoPlanCandidate candidate;
    candidate.id      = plan.id;
    candidate.summary = plan.summary;

    // Exposure source is the defining MSLA safety gate.
    switch (plan.exposure_source) {
    case MslaExposureSource::AuditedPreset:
        candidate.constraints.push_back(satisfied("exposure preset"));
        break;
    case MslaExposureSource::None:
        candidate.constraints.push_back(unknown(
            "exposure preset",
            "no audited exposure preset for " + plan.printer + " + " + plan.resin +
                "; auto planning stops and requests the preset or a calibration workflow",
            "audited exposure preset for " + plan.printer + " + " + plan.resin));
        break;
    case MslaExposureSource::SubstitutedBySimilarity:
        candidate.constraints.push_back(violated(
            "exposure preset", "a preset for a similar resin was substituted; exposure is "
                               "never borrowed across resins by similarity"));
        break;
    case MslaExposureSource::DerivedFromGeometry:
        candidate.constraints.push_back(violated(
            "exposure preset",
            "exposure was derived from geometry; it must come from an audited preset"));
        break;
    }

    // Resin issue analysis (UV4). A non-complete analysis has not established
    // safety; any blocking issue is a violation.
    if (plan.analysis.state != AnalysisState::Complete)
        candidate.constraints.push_back(unknown(
            "resin analysis",
            "the resin analysis is " + std::string(analysis_state_name(plan.analysis.state)) +
                ", so the absence of a blocking issue is not established",
            "a complete resin analysis"));
    else if (plan.analysis.has_blocking())
        candidate.constraints.push_back(violated(
            "resin analysis",
            std::to_string(plan.analysis.count(ResinIssueSeverity::Blocking)) +
                " blocking resin issue(s) (islands, resin traps, suction cups, empty/invalid "
                "layers)"));
    else
        candidate.constraints.push_back(satisfied("resin analysis"));

    // Layer height inside the printer/resin/profile/geometry intersection.
    if (! plan.layer_height_supported)
        candidate.constraints.push_back(violated(
            "layer height", "outside what the printer, resin and profile jointly allow"));
    else
        candidate.constraints.push_back(satisfied("layer height"));

    // Trapped-resin / drain validation for a hollow part.
    switch (plan.trapped_resin) {
    case ValidationState3::Passed:
        candidate.constraints.push_back(satisfied("trapped resin"));
        break;
    case ValidationState3::NotValidated:
        candidate.constraints.push_back(unknown(
            "trapped resin", "the hollowed part's drainage was not validated",
            "trapped-resin validation"));
        break;
    case ValidationState3::Failed:
        candidate.constraints.push_back(violated(
            "trapped resin", "the hollowed part would trap resin without a drain path"));
        break;
    }

    candidate.objectives = {
        { "quality", plan.predicted_quality },
        { "support_burden", plan.predicted_support_burden },
        { "time_s", plan.predicted_time_s },
        { "peel_risk", plan.predicted_peel_risk },
    };
    return candidate;
}

AutoPlanRequest build_msla_request(const std::vector<MslaCandidatePlan> &candidates,
                                   const ObjectiveVector                &objectives,
                                   uint64_t                              seed)
{
    AutoPlanRequest request;
    request.objectives = objectives;
    request.seed       = seed;
    for (const MslaCandidatePlan &plan : candidates)
        request.candidates.push_back(map_msla_candidate(plan));
    return request;
}

ObjectiveVector msla_objectives_for_slider(double quality_to_speed)
{
    const double q = quality_to_speed < 0. ? 0. : quality_to_speed > 1. ? 1. : quality_to_speed;
    const double quality_weight = q;
    const double speed_weight    = 1. - q;

    ObjectiveVector vector;
    vector.objectives = {
        { "quality", ObjectiveDirection::HigherIsBetter, quality_weight },
        { "time_s", ObjectiveDirection::LowerIsBetter, speed_weight },
        // Support burden and peel risk always matter: a fragile fast plan does
        // not win purely on speed.
        { "support_burden", ObjectiveDirection::LowerIsBetter, 0.15 },
        { "peel_risk", ObjectiveDirection::LowerIsBetter, 0.2 },
    };
    return vector;
}

} // namespace Slic3r
