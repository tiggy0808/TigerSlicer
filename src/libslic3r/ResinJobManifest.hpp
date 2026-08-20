///|/ TigerSlicer Phase UV2 groundwork: the canonical resin-job manifest and
///|/ conversion-loss accounting.
///|/
///|/ This is the versioned intermediate representation the Stage B UVtools
///|/ layer serializes across. It is decision logic in libslic3r, testable on
///|/ Linux, and it integrates no UVtools code: it extends the existing
///|/ MSlicerResin job model (MSlicer stays the only resin producer) with the
///|/ provenance and loss accounting a format-conversion layer needs. The
///|/ UVtools source audit, capability matrix, licence review and integration
///|/ boundary remain open (see doc/TigerSlicerContinuationRoadmap.md).
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ResinJobManifest_hpp_
#define slic3r_ResinJobManifest_hpp_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Where a semantic in the job came from and how far it can be trusted. A
// value nobody characterized is Unknown, never a silent default.
enum class ResinProvenance : uint8_t
{
    Measured,     // read from a printer-produced artifact
    Configured,   // set in a profile
    Assumed,      // a default stood in
    Unknown,
};

const char *resin_provenance_name(ResinProvenance provenance);

// The origin corner of the pixel grid. Which corner a machine treats as (0,0)
// changes where geometry lands; getting it wrong mirrors a print.
enum class PixelOrigin : uint8_t
{
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Unknown,
};

const char *pixel_origin_name(PixelOrigin origin);

// One capability the destination format must have for a field to survive a
// conversion. A per-layer override that the format cannot express is a loss,
// not a silent drop.
enum class ResinCapability : uint8_t
{
    PerLayerExposure,
    PerLayerLift,
    PerLayerLightPwm,
    Tilt,
    MultiStageMotion,
    Thumbnails,
    AntiAliasing,
    LightPwm,
};

const char *resin_capability_name(ResinCapability capability);

struct MachineIdentity
{
    std::string vendor;
    std::string model;
    std::string firmware;
    // Which capabilities the destination machine/format actually has. A field
    // gated on a capability not in this set cannot be represented.
    std::vector<ResinCapability> capabilities;

    bool has(ResinCapability capability) const;
};

// The exposure and motion of one layer class (normal, bottom, transition), or
// one layer's override of it.
struct ResinExposure
{
    std::optional<float> exposure_s;
    std::optional<float> light_off_delay_s;
    std::optional<int>   light_pwm;       // 0..255 where the machine supports it
    std::optional<float> lift_distance_mm;
    std::optional<float> lift_speed_mm_min;
    std::optional<float> retract_distance_mm;
    std::optional<float> retract_speed_mm_min;
    std::optional<float> tilt_deg;        // gated on ResinCapability::Tilt
};

struct ResinLayer
{
    // Absolute Z of the layer top, and its own height, both required: a layer
    // with no Z is not placeable.
    std::optional<double> z_mm;
    std::optional<double> height_mm;

    // Present only when this layer departs from its class defaults. Each field
    // is capability-gated at conversion time.
    std::optional<ResinExposure> override_exposure;
};

// How a raster layer image is stored, so a round trip can be checked for
// bit-exactness where the format is lossless and flagged where it is not.
struct RasterEncoding
{
    std::string codec;          // e.g. "rle", "png", "raw"
    bool        lossless { true };
    std::optional<int> bit_depth;
    std::optional<int> antialias_levels;
};

struct ResinJobManifest
{
    // Bumped when the schema changes. A reader refuses a version it does not
    // understand rather than partially reading it.
    int schema_version { 1 };

    MachineIdentity machine;

    // Physical build volume and the pixel grid that maps onto it.
    std::optional<double>   platform_x_mm, platform_y_mm, platform_z_mm;
    std::optional<uint32_t> resolution_x, resolution_y;
    // Millimetres per pixel. Absent means the mapping from pixels to
    // millimetres is unknown and cannot be re-derived safely.
    std::optional<double> pixel_pitch_x_mm, pixel_pitch_y_mm;

    PixelOrigin origin { PixelOrigin::Unknown };
    bool        mirror_x { false };
    bool        mirror_y { false };
    int         rotation_deg { 0 };   // 0/90/180/270
    bool        portrait { false };

    ResinExposure normal;
    ResinExposure bottom;
    std::optional<ResinExposure> transition;
    uint32_t bottom_layers { 0 };
    uint32_t transition_layers { 0 };

    std::vector<ResinLayer> layers;

    RasterEncoding encoding;

    // Thumbnails as (name -> byte size), so their presence survives even when
    // the pixels are not carried in the manifest itself.
    std::map<std::string, size_t> thumbnails;

    // Free-form metadata a format may carry.
    std::map<std::string, std::string> metadata;

    // Provenance: who produced this job and at what version, so a converted
    // file can be traced back.
    std::string mslicer_version;
    std::string tigerslicer_version;
    std::string uvtools_version;   // empty until the UVtools layer touches it

    // Per-field trust. A field with no entry is treated as Unknown.
    std::map<std::string, ResinProvenance> provenance;

    // Findings recorded during slicing/validation, carried into the file.
    std::vector<std::string> warnings;
    std::vector<std::string> validation_findings;
    std::vector<std::string> repairs;

    ResinProvenance provenance_of(const std::string &field) const;
};

// --- conversion loss accounting --------------------------------------------

// How serious a loss is. A critical loss blocks export by default; a
// noncritical one may be overridden by an expert, with the override recorded.
enum class LossSeverity : uint8_t
{
    // The destination represents the field exactly.
    Lossless,
    // Representable, but not exactly (e.g. anti-aliasing levels reduced).
    Noncritical,
    // A semantic the destination cannot carry safely (e.g. per-layer exposure
    // dropped on a format with none). Blocks export unless overridden.
    Critical,
};

const char *loss_severity_name(LossSeverity severity);

struct LossRecord
{
    std::string  field;
    LossSeverity severity { LossSeverity::Lossless };
    std::string  detail;
    // The capability the destination is missing, when that is the cause.
    std::optional<ResinCapability> missing_capability;
};

struct ConversionLossReport
{
    std::string source_format;
    std::string destination_format;

    // Only the records that are not Lossless, in a stable order.
    std::vector<LossRecord> losses;

    size_t noncritical() const;
    size_t critical() const;

    // Export is permitted when there are no critical losses, or every critical
    // loss has been explicitly accepted. Computed by evaluate_export.
    bool blocks_export { true };
    // Critical losses that were accepted by an expert override.
    std::vector<std::string> accepted_overrides;
    std::string summary;
};

// Compare a job against a destination machine/format capability set and report
// what a conversion would lose. Nothing is dropped silently: a field the
// destination cannot represent becomes a LossRecord, and a critical one blocks
// export until named in `accepted_critical`.
ConversionLossReport evaluate_conversion(const ResinJobManifest      &job,
                                         const MachineIdentity       &destination,
                                         const std::string           &source_format,
                                         const std::string           &destination_format,
                                         const std::vector<std::string> &accepted_critical);

// --- validation -------------------------------------------------------------

struct ManifestValidation
{
    bool                     ok { false };
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Check a manifest is internally coherent before it is trusted as an IR: a
// schema version this build understands, a placeable layer stack, a pixel
// grid that maps to millimetres, and bottom/transition counts that fit the
// layers present. Reports what is wrong; never repairs silently.
ManifestValidation validate_manifest(const ResinJobManifest &job);

// The schema version this build reads and writes.
int resin_manifest_schema_version();

} // namespace Slic3r

#endif // slic3r_ResinJobManifest_hpp_
