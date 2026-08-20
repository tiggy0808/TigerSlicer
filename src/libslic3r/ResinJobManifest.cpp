///|/ TigerSlicer Phase UV2 groundwork: the canonical resin-job manifest and
///|/ conversion-loss accounting.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ResinJobManifest.hpp"

#include <algorithm>

namespace Slic3r {

const char *resin_provenance_name(ResinProvenance provenance)
{
    switch (provenance) {
    case ResinProvenance::Measured:   return "measured";
    case ResinProvenance::Configured: return "configured";
    case ResinProvenance::Assumed:    return "assumed";
    case ResinProvenance::Unknown:    return "unknown";
    }
    return "unknown";
}

const char *pixel_origin_name(PixelOrigin origin)
{
    switch (origin) {
    case PixelOrigin::TopLeft:     return "top-left";
    case PixelOrigin::TopRight:    return "top-right";
    case PixelOrigin::BottomLeft:  return "bottom-left";
    case PixelOrigin::BottomRight: return "bottom-right";
    case PixelOrigin::Unknown:     return "unknown";
    }
    return "unknown";
}

const char *resin_capability_name(ResinCapability capability)
{
    switch (capability) {
    case ResinCapability::PerLayerExposure: return "per-layer exposure";
    case ResinCapability::PerLayerLift:     return "per-layer lift";
    case ResinCapability::PerLayerLightPwm: return "per-layer light pwm";
    case ResinCapability::Tilt:             return "tilt";
    case ResinCapability::MultiStageMotion: return "multi-stage motion";
    case ResinCapability::Thumbnails:       return "thumbnails";
    case ResinCapability::AntiAliasing:     return "anti-aliasing";
    case ResinCapability::LightPwm:         return "light pwm";
    }
    return "per-layer exposure";
}

const char *loss_severity_name(LossSeverity severity)
{
    switch (severity) {
    case LossSeverity::Lossless:    return "lossless";
    case LossSeverity::Noncritical: return "noncritical";
    case LossSeverity::Critical:    return "critical";
    }
    return "lossless";
}

bool MachineIdentity::has(ResinCapability capability) const
{
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

ResinProvenance ResinJobManifest::provenance_of(const std::string &field) const
{
    auto it = provenance.find(field);
    return it == provenance.end() ? ResinProvenance::Unknown : it->second;
}

size_t ConversionLossReport::noncritical() const
{
    size_t count = 0;
    for (const LossRecord &loss : losses)
        if (loss.severity == LossSeverity::Noncritical)
            ++count;
    return count;
}

size_t ConversionLossReport::critical() const
{
    size_t count = 0;
    for (const LossRecord &loss : losses)
        if (loss.severity == LossSeverity::Critical)
            ++count;
    return count;
}

int resin_manifest_schema_version()
{
    return 1;
}

// --- conversion --------------------------------------------------------------

namespace {

bool layer_overrides_exposure(const ResinExposure &e)
{
    return e.exposure_s.has_value() || e.light_off_delay_s.has_value();
}

bool layer_overrides_lift(const ResinExposure &e)
{
    return e.lift_distance_mm.has_value() || e.lift_speed_mm_min.has_value() ||
           e.retract_distance_mm.has_value() || e.retract_speed_mm_min.has_value();
}

} // namespace

ConversionLossReport evaluate_conversion(const ResinJobManifest         &job,
                                         const MachineIdentity          &destination,
                                         const std::string              &source_format,
                                         const std::string              &destination_format,
                                         const std::vector<std::string> &accepted_critical)
{
    ConversionLossReport report;
    report.source_format      = source_format;
    report.destination_format = destination_format;

    auto add = [&report](const std::string &field, LossSeverity severity,
                         const std::string &detail,
                         std::optional<ResinCapability> capability) {
        LossRecord record;
        record.field              = field;
        record.severity           = severity;
        record.detail             = detail;
        record.missing_capability = capability;
        report.losses.push_back(record);
    };

    // Whether any layer carries a per-field override, so a capability the
    // destination lacks only matters when the job actually uses it.
    bool uses_layer_exposure = false;
    bool uses_layer_lift     = false;
    bool uses_layer_pwm      = false;
    bool uses_layer_tilt     = false;
    for (const ResinLayer &layer : job.layers) {
        if (! layer.override_exposure.has_value())
            continue;
        const ResinExposure &e = *layer.override_exposure;
        uses_layer_exposure |= layer_overrides_exposure(e);
        uses_layer_lift |= layer_overrides_lift(e);
        uses_layer_pwm |= e.light_pwm.has_value();
        uses_layer_tilt |= e.tilt_deg.has_value();
    }

    if (uses_layer_exposure && ! destination.has(ResinCapability::PerLayerExposure))
        add("per-layer exposure", LossSeverity::Critical,
            "the job sets exposure on individual layers, which " + destination_format +
                " cannot carry; those layers would print at the class default",
            ResinCapability::PerLayerExposure);
    if (uses_layer_lift && ! destination.has(ResinCapability::PerLayerLift))
        add("per-layer lift", LossSeverity::Critical,
            "the job sets lift or retract on individual layers, which " + destination_format +
                " cannot carry",
            ResinCapability::PerLayerLift);
    if (uses_layer_pwm && ! destination.has(ResinCapability::PerLayerLightPwm))
        add("per-layer light pwm", LossSeverity::Critical,
            "the job sets light power on individual layers, which " + destination_format +
                " cannot carry",
            ResinCapability::PerLayerLightPwm);

    // Whole-job semantics. Tilt and light-pwm can appear on the normal or bottom
    // class, on the transition class, or (for tilt) on any individual layer;
    // every source must be checked or a job would silently flatten on export.
    const bool uses_tilt = job.normal.tilt_deg.has_value() || job.bottom.tilt_deg.has_value() ||
                           uses_layer_tilt ||
                           (job.transition.has_value() && job.transition->tilt_deg.has_value());
    if (uses_tilt && ! destination.has(ResinCapability::Tilt))
        add("tilt", LossSeverity::Critical,
            destination_format + " has no tilt motion; a tilting job cannot be represented",
            ResinCapability::Tilt);

    const bool uses_pwm = job.normal.light_pwm.has_value() || job.bottom.light_pwm.has_value() ||
                          (job.transition.has_value() && job.transition->light_pwm.has_value());
    if (uses_pwm && ! destination.has(ResinCapability::LightPwm))
        add("light pwm", LossSeverity::Critical,
            "the job controls light power, which " + destination_format + " cannot carry",
            ResinCapability::LightPwm);

    // Anti-aliasing is a quality reduction, not a semantic break: noncritical.
    if (job.encoding.antialias_levels.has_value() && *job.encoding.antialias_levels > 1 &&
        ! destination.has(ResinCapability::AntiAliasing))
        add("anti-aliasing", LossSeverity::Noncritical,
            destination_format + " does not carry anti-aliasing; edges would be aliased",
            ResinCapability::AntiAliasing);

    // Thumbnails present but unrepresentable: noncritical, they are cosmetic.
    if (! job.thumbnails.empty() && ! destination.has(ResinCapability::Thumbnails))
        add("thumbnails", LossSeverity::Noncritical,
            destination_format + " carries no thumbnails; previews would be dropped",
            ResinCapability::Thumbnails);

    // A lossy raster codec into a job that was lossless is a noncritical loss
    // the operator should see.
    if (! job.encoding.lossless)
        add("raster encoding", LossSeverity::Noncritical,
            "the source raster is stored with a lossy codec (" + job.encoding.codec +
                "); pixel values may not round-trip exactly",
            std::nullopt);

    // Stable order: critical first, then by field name, so two reports of the
    // same conversion read alike.
    std::sort(report.losses.begin(), report.losses.end(),
              [](const LossRecord &a, const LossRecord &b) {
                  if (a.severity != b.severity)
                      return uint8_t(a.severity) > uint8_t(b.severity); // Critical first
                  return a.field < b.field;
              });

    // Export is blocked by any critical loss that was not explicitly accepted.
    size_t unaccepted_critical = 0;
    for (const LossRecord &loss : report.losses) {
        if (loss.severity != LossSeverity::Critical)
            continue;
        const bool accepted = std::find(accepted_critical.begin(), accepted_critical.end(),
                                        loss.field) != accepted_critical.end();
        if (accepted)
            report.accepted_overrides.push_back(loss.field);
        else
            ++unaccepted_critical;
    }

    report.blocks_export = unaccepted_critical > 0;

    if (report.losses.empty()) {
        report.summary = "the conversion to " + destination_format + " is lossless";
    } else if (report.blocks_export) {
        report.summary = std::to_string(unaccepted_critical) +
                         " critical loss(es) block export to " + destination_format +
                         " until each is explicitly accepted";
    } else if (! report.accepted_overrides.empty()) {
        report.summary = std::to_string(report.accepted_overrides.size()) +
                         " critical loss(es) were accepted by override and recorded; " +
                         std::to_string(report.noncritical()) + " noncritical loss(es) remain";
    } else {
        report.summary = std::to_string(report.noncritical()) +
                         " noncritical loss(es) to " + destination_format +
                         "; export is allowed";
    }

    return report;
}

// --- validation --------------------------------------------------------------

ManifestValidation validate_manifest(const ResinJobManifest &job)
{
    ManifestValidation result;

    if (job.schema_version != resin_manifest_schema_version()) {
        result.errors.push_back(
            "the manifest is schema version " + std::to_string(job.schema_version) +
            " but this build reads version " + std::to_string(resin_manifest_schema_version()) +
            "; it is refused rather than partially read");
        // A version mismatch is fatal on its own; nothing else can be trusted.
        result.ok = false;
        return result;
    }

    if (job.layers.empty())
        result.errors.push_back("the job has no layers");

    // Every layer has to be placeable, and the stack has to ascend.
    std::optional<double> previous_z;
    size_t                index = 0;
    for (const ResinLayer &layer : job.layers) {
        if (! layer.z_mm.has_value())
            result.errors.push_back("layer " + std::to_string(index) + " has no Z");
        if (! layer.height_mm.has_value())
            result.errors.push_back("layer " + std::to_string(index) + " has no height");
        if (layer.z_mm.has_value() && previous_z.has_value() && *layer.z_mm <= *previous_z)
            result.errors.push_back("layer " + std::to_string(index) +
                                    " does not ascend above the layer below it");
        if (layer.z_mm.has_value())
            previous_z = layer.z_mm;
        ++index;
    }

    // The pixel grid has to map to millimetres for geometry to be placed.
    if (! job.resolution_x.has_value() || ! job.resolution_y.has_value())
        result.errors.push_back("the pixel resolution is not set");
    if (! job.pixel_pitch_x_mm.has_value() || ! job.pixel_pitch_y_mm.has_value())
        result.warnings.push_back(
            "the pixel pitch is not set, so the mapping from pixels to millimetres cannot be "
            "re-derived and must be taken from the machine profile");
    if (job.origin == PixelOrigin::Unknown)
        result.warnings.push_back(
            "the pixel origin is unknown; orientation cannot be confirmed and a mirrored print "
            "would not be detected here");

    // Bottom and transition layers cannot exceed the stack.
    const uint64_t special = uint64_t(job.bottom_layers) + uint64_t(job.transition_layers);
    if (special > job.layers.size())
        result.errors.push_back("more bottom+transition layers were declared (" +
                                std::to_string(special) + ") than the job has layers (" +
                                std::to_string(job.layers.size()) + ")");
    if (job.transition_layers > 0 && ! job.transition.has_value())
        result.warnings.push_back(
            "transition layers are declared but no transition exposure is set; they would use "
            "the normal exposure");

    if (job.platform_z_mm.has_value() && previous_z.has_value() &&
        *previous_z > *job.platform_z_mm + 1e-6)
        result.errors.push_back("the layer stack is taller than the platform Z");

    result.ok = result.errors.empty();
    return result;
}

} // namespace Slic3r
