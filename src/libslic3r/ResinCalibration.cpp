///|/ TigerSlicer Phase UV6 groundwork: resin calibration generation and its
///|/ safety gating.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ResinCalibration.hpp"

#include <algorithm>

namespace Slic3r {

const char *resin_calibration_kind_name(ResinCalibrationKind kind)
{
    switch (kind) {
    case ResinCalibrationKind::Exposure:            return "exposure";
    case ResinCalibrationKind::Tolerance:           return "tolerance";
    case ResinCalibrationKind::DimensionalAccuracy: return "dimensional accuracy";
    case ResinCalibrationKind::ElephantFoot:        return "elephant foot";
    case ResinCalibrationKind::GrayscaleBlooming:   return "grayscale / blooming";
    case ResinCalibrationKind::Lift:                return "lift";
    }
    return "exposure";
}

const char *calibration_verdict_name(CalibrationVerdict verdict)
{
    switch (verdict) {
    case CalibrationVerdict::Ready:                 return "ready";
    case CalibrationVerdict::Unsupported:           return "unsupported";
    case CalibrationVerdict::NeedsConfirmedSupport: return "needs confirmed support";
    }
    return "unsupported";
}

const char *project_action_name(ProjectAction action)
{
    switch (action) {
    case ProjectAction::CreateNew: return "create new";
    case ProjectAction::Replace:   return "replace";
    }
    return "create new";
}

const char *save_verdict_name(SaveVerdict verdict)
{
    switch (verdict) {
    case SaveVerdict::Allowed:          return "allowed";
    case SaveVerdict::NeedsConfirmation: return "needs confirmation";
    case SaveVerdict::Refused:          return "refused";
    }
    return "refused";
}

// The disclaimer that must travel with every generated calibration. Spelled out
// here in one place so it can never be quietly dropped or softened.
static const char *const kCalibrationDisclaimer =
    "This calibration is a starting point. It is not guaranteed safe or correct "
    "for every resin, LCD/screen, vat, FEP or firmware; verify results before "
    "relying on them.";

const std::vector<ResinCalibrationTest> &resin_calibration_catalog()
{
    static const std::vector<ResinCalibrationTest> catalog = [] {
        std::vector<ResinCalibrationTest> tests;

        {
            ResinCalibrationTest t;
            t.kind     = ResinCalibrationKind::Exposure;
            t.measures = "the normal-layer exposure that resolves fine features without "
                         "over-curing";
            // An exposure matrix that varies exposure per band needs per-layer
            // exposure, which is an advanced setting.
            t.requirements = { { "per_layer_exposure",
                                 "the exposure sweep sets a different exposure per test band",
                                 true } };
            t.ranges = { { "exposure", 1.5, 4.0, 0.25, "s" } };
            tests.push_back(t);
        }
        {
            ResinCalibrationTest t;
            t.kind         = ResinCalibrationKind::Tolerance;
            t.measures     = "the smallest clearance that still separates after curing";
            t.requirements = {};
            t.ranges       = { { "clearance", 0.05, 0.5, 0.05, "mm" } };
            tests.push_back(t);
        }
        {
            ResinCalibrationTest t;
            t.kind         = ResinCalibrationKind::DimensionalAccuracy;
            t.measures     = "XY and Z scaling error against known nominal dimensions";
            t.requirements = {};
            t.ranges       = { { "nominal", 10., 40., 10., "mm" } };
            tests.push_back(t);
        }
        {
            ResinCalibrationTest t;
            t.kind         = ResinCalibrationKind::ElephantFoot;
            t.measures     = "first-layer compression / elephant-foot compensation";
            t.requirements = {};
            t.ranges       = { { "compensation", 0.0, 0.3, 0.05, "mm" } };
            tests.push_back(t);
        }
        {
            ResinCalibrationTest t;
            t.kind     = ResinCalibrationKind::GrayscaleBlooming;
            t.measures = "anti-aliasing and light-bleed behaviour of the screen";
            // Grayscale needs per-layer light power control, an advanced setting.
            t.requirements = { { "light_pwm",
                                 "the grayscale test varies light power across the pattern",
                                 true } };
            t.ranges = { { "gray_level", 32., 255., 32., "pwm" } };
            tests.push_back(t);
        }
        {
            ResinCalibrationTest t;
            t.kind         = ResinCalibrationKind::Lift;
            t.measures     = "lift distance and speed that clear suction without tearing";
            t.requirements = {};
            t.ranges = { { "lift_distance", 3., 8., 1., "mm" },
                         { "lift_speed", 60., 180., 30., "mm/min" } };
            tests.push_back(t);
        }
        return tests;
    }();
    return catalog;
}

const ResinCalibrationTest *find_calibration_test(ResinCalibrationKind kind)
{
    for (const ResinCalibrationTest &test : resin_calibration_catalog())
        if (test.kind == kind)
            return &test;
    return nullptr;
}

namespace {

CalibrationRange clamp_to_safe(const CalibrationRange &safe, const CalibrationRange &wanted,
                               bool &clamped)
{
    CalibrationRange out = wanted;
    clamped              = false;
    if (out.minimum < safe.minimum) {
        out.minimum = safe.minimum;
        clamped     = true;
    }
    if (out.maximum > safe.maximum) {
        out.maximum = safe.maximum;
        clamped     = true;
    }
    if (out.minimum > out.maximum) {
        out = safe;
        clamped = true;
    }
    return out;
}

// Whether a capability field is confirmed strongly enough for an advanced
// setting: vendor spec or hardware, not merely inferred or inherited.
bool confirmed_enough(const ResolvedField *field)
{
    if (field == nullptr || ! field->value.has_value() || field->conflicted)
        return false;
    return uint8_t(field->evidence) >= uint8_t(EvidenceLevel::ConfirmedByVendorSpec);
}

bool present(const ResolvedField *field)
{
    return field != nullptr && field->value.has_value() && ! field->conflicted;
}

} // namespace

ResinCalibrationPlan plan_resin_calibration(const ResinCalibrationRequest &request)
{
    ResinCalibrationPlan plan;
    plan.kind = request.kind;

    const ResinCalibrationTest *test = find_calibration_test(request.kind);
    if (test == nullptr) {
        plan.verdict = CalibrationVerdict::Unsupported;
        plan.unmet.push_back("no such calibration test");
        return plan;
    }

    // Gate every requirement. A missing capability is Unsupported; an advanced
    // one present only by weak evidence is NeedsConfirmedSupport - a distinct
    // verdict, because gating an advanced per-layer setting on a guess is
    // exactly what UV6 forbids.
    bool unsupported = false;
    bool needs_confirmed = false;
    for (const CalibrationRequirement &requirement : test->requirements) {
        const ResolvedField *field = request.machine.find(requirement.capability_field);
        if (! present(field)) {
            unsupported = true;
            plan.unmet.push_back(requirement.capability_field + ": not supported (" +
                                 requirement.why + ")");
        } else if (requirement.advanced && ! confirmed_enough(field)) {
            needs_confirmed = true;
            plan.unmet.push_back(
                requirement.capability_field +
                ": present but not confirmed by vendor spec or hardware, so this advanced "
                "per-layer setting will not be gated on a guess");
        }
    }

    // Resolve the ranges: descriptor defaults, clamped to the caller's safe
    // overrides.
    for (const CalibrationRange &safe : test->ranges) {
        auto override_it = request.range_overrides.find(safe.parameter);
        if (override_it == request.range_overrides.end()) {
            plan.ranges.push_back(safe);
            continue;
        }
        bool clamped = false;
        plan.ranges.push_back(clamp_to_safe(safe, override_it->second, clamped));
        if (clamped)
            plan.unmet.push_back(safe.parameter +
                                 ": the requested range was clamped to the safe bounds");
    }

    if (unsupported)
        plan.verdict = CalibrationVerdict::Unsupported;
    else if (needs_confirmed)
        plan.verdict = CalibrationVerdict::NeedsConfirmedSupport;
    else
        plan.verdict = CalibrationVerdict::Ready;

    // The provenance and disclaimer are attached whatever the verdict, so even
    // a refused plan carries the record of what was attempted. The disclaimer
    // is only meaningful on a Ready plan but is never omitted.
    plan.disclaimer                = kCalibrationDisclaimer;
    plan.provenance.test           = resin_calibration_kind_name(request.kind);
    plan.provenance.machine_vendor = request.machine_vendor;
    plan.provenance.machine_model  = request.machine_model;
    plan.provenance.machine_firmware    = request.machine_firmware;
    plan.provenance.mslicer_version     = request.mslicer_version;
    plan.provenance.tigerslicer_version = request.tigerslicer_version;
    plan.provenance.uvtools_version     = request.uvtools_version;
    plan.provenance.ranges              = plan.ranges;
    plan.provenance.generated_s         = request.generated_s;
    return plan;
}

CalibrationSaveDecision evaluate_calibration_save(const CalibrationSaveTarget &target,
                                                  ProjectAction                intended)
{
    CalibrationSaveDecision decision;

    // A user model is never a valid calibration target. This is refused
    // outright, not gated on confirmation: a calibration must not be able to
    // destroy a user's work at all.
    if (target.exists && target.is_user_model) {
        decision.verdict = SaveVerdict::Refused;
        decision.message = "the target is an ordinary user model; a calibration project cannot "
                           "overwrite it under any confirmation";
        return decision;
    }

    if (! target.exists || intended == ProjectAction::CreateNew) {
        // Creating a new project, or writing to a path that does not exist, is
        // safe. (If the path exists but the caller asked to create new, the
        // caller is responsible for having chosen a fresh path; a genuinely
        // occupied path with CreateNew is treated as a replacement below.)
        if (! target.exists) {
            decision.verdict = SaveVerdict::Allowed;
            decision.message = "a new calibration project; safe to write";
            return decision;
        }
    }

    // The target exists and is a prior calibration project: replacing it needs
    // confirmation, but is allowed with it.
    decision.verdict = SaveVerdict::NeedsConfirmation;
    decision.message = "a calibration project already exists here; replacing it needs "
                       "confirmation";
    return decision;
}

} // namespace Slic3r
