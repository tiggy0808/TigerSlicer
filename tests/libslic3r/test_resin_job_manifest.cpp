///|/ TigerSlicer Phase UV2 groundwork: the canonical resin-job manifest and
///|/ conversion-loss accounting.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/ResinJobManifest.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

MachineIdentity machine(std::vector<ResinCapability> capabilities)
{
    MachineIdentity id;
    id.vendor       = "Elegoo";
    id.model        = "Saturn";
    id.firmware     = "1.0";
    id.capabilities = std::move(capabilities);
    return id;
}

ResinJobManifest simple_job(size_t layer_count)
{
    ResinJobManifest job;
    job.schema_version    = resin_manifest_schema_version();
    job.machine           = machine({ ResinCapability::Thumbnails });
    job.platform_x_mm     = 218.88;
    job.platform_y_mm     = 122.88;
    job.platform_z_mm     = 250.;
    job.resolution_x      = 3840;
    job.resolution_y      = 2160;
    job.pixel_pitch_x_mm  = 0.057;
    job.pixel_pitch_y_mm  = 0.057;
    job.origin            = PixelOrigin::TopLeft;
    job.normal.exposure_s = 2.5f;
    job.bottom.exposure_s = 30.f;
    job.bottom_layers     = 5;
    job.encoding.codec    = "rle";
    job.encoding.lossless = true;

    for (size_t i = 0; i < layer_count; ++i) {
        ResinLayer layer;
        layer.z_mm      = double(i + 1) * 0.05;
        layer.height_mm = 0.05;
        job.layers.push_back(layer);
    }
    return job;
}

const LossRecord *loss_for(const ConversionLossReport &report, const std::string &field)
{
    for (const LossRecord &loss : report.losses)
        if (loss.field == field)
            return &loss;
    return nullptr;
}

} // namespace

TEST_CASE("UV2: a coherent manifest validates, and its defects are named", "[resin-manifest]")
{
    SECTION("a well-formed job is valid")
    {
        const ManifestValidation result = validate_manifest(simple_job(20));
        CHECK(result.ok);
        CHECK(result.errors.empty());
    }

    SECTION("a schema version this build does not read is refused whole")
    {
        ResinJobManifest job = simple_job(20);
        job.schema_version   = 999;
        const ManifestValidation result = validate_manifest(job);
        CHECK_FALSE(result.ok);
        CHECK(result.errors.size() == 1);
        CHECK_THAT(result.errors[0], Catch::Matchers::ContainsSubstring("refused"));
    }

    SECTION("a layer with no Z is not placeable")
    {
        ResinJobManifest job = simple_job(3);
        job.layers[1].z_mm.reset();
        const ManifestValidation result = validate_manifest(job);
        CHECK_FALSE(result.ok);
        CHECK_THAT(result.errors[0], Catch::Matchers::ContainsSubstring("no Z"));
    }

    SECTION("a stack that does not ascend is rejected")
    {
        ResinJobManifest job = simple_job(3);
        job.layers[2].z_mm   = job.layers[1].z_mm; // equal, not above
        const ManifestValidation result = validate_manifest(job);
        CHECK_FALSE(result.ok);
        CHECK_THAT(result.errors[0], Catch::Matchers::ContainsSubstring("ascend"));
    }

    SECTION("more special layers than layers is an error")
    {
        ResinJobManifest job = simple_job(3);
        job.bottom_layers    = 4;
        const ManifestValidation result = validate_manifest(job);
        CHECK_FALSE(result.ok);
        CHECK_THAT(result.errors.back(), Catch::Matchers::ContainsSubstring("bottom+transition"));
    }

    SECTION("a stack taller than the platform is an error")
    {
        ResinJobManifest job = simple_job(20);
        job.platform_z_mm    = 0.5; // shorter than 20 * 0.05 = 1.0 mm
        const ManifestValidation result = validate_manifest(job);
        CHECK_FALSE(result.ok);
        CHECK_THAT(result.errors.back(), Catch::Matchers::ContainsSubstring("taller than"));
    }

    SECTION("an unknown pixel origin warns rather than errors")
    {
        ResinJobManifest job = simple_job(5);
        job.origin           = PixelOrigin::Unknown;
        const ManifestValidation result = validate_manifest(job);
        CHECK(result.ok);
        CHECK_FALSE(result.warnings.empty());
    }

    SECTION("no pixel pitch warns that the mapping cannot be re-derived")
    {
        ResinJobManifest job = simple_job(5);
        job.pixel_pitch_x_mm.reset();
        const ManifestValidation result = validate_manifest(job);
        CHECK(result.ok);
        bool warned = false;
        for (const std::string &w : result.warnings)
            if (w.find("pixel pitch") != std::string::npos)
                warned = true;
        CHECK(warned);
    }
}

TEST_CASE("UV2: provenance travels with a field and defaults to unknown", "[resin-manifest]")
{
    ResinJobManifest job = simple_job(5);
    job.provenance["normal.exposure_s"] = ResinProvenance::Configured;
    job.provenance["platform_x_mm"]     = ResinProvenance::Measured;

    CHECK(job.provenance_of("normal.exposure_s") == ResinProvenance::Configured);
    CHECK(job.provenance_of("platform_x_mm") == ResinProvenance::Measured);
    // A field nobody attributed is Unknown, not silently Configured.
    CHECK(job.provenance_of("pixel_pitch_x_mm") == ResinProvenance::Unknown);
}

TEST_CASE("UV2: a lossless conversion is reported as lossless", "[resin-manifest]")
{
    ResinJobManifest job = simple_job(20);
    job.thumbnails["preview"] = 1024;

    // The destination has everything the job uses.
    const MachineIdentity destination =
        machine({ ResinCapability::Thumbnails });

    const ConversionLossReport report =
        evaluate_conversion(job, destination, "ctb", "goo", {});
    CHECK(report.losses.empty());
    CHECK_FALSE(report.blocks_export);
    CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("lossless"));
}

TEST_CASE("UV2: a semantic the destination cannot carry is a critical loss that blocks export",
          "[resin-manifest]")
{
    ResinJobManifest job = simple_job(20);
    // Layer 10 wants its own exposure.
    ResinExposure override;
    override.exposure_s        = 3.5f;
    job.layers[10].override_exposure = override;

    // The destination has no per-layer exposure.
    const MachineIdentity destination = machine({ ResinCapability::Thumbnails });

    SECTION("unaccepted, it blocks export and says which field")
    {
        const ConversionLossReport report =
            evaluate_conversion(job, destination, "ctb", "goo", {});
        CHECK(report.blocks_export);
        CHECK(report.critical() == 1);
        const LossRecord *loss = loss_for(report, "per-layer exposure");
        REQUIRE(loss != nullptr);
        CHECK(loss->severity == LossSeverity::Critical);
        CHECK(loss->missing_capability.value() == ResinCapability::PerLayerExposure);
        CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("block export"));
    }

    SECTION("accepted by an expert override, it is recorded and export is allowed")
    {
        const ConversionLossReport report =
            evaluate_conversion(job, destination, "ctb", "goo", { "per-layer exposure" });
        CHECK_FALSE(report.blocks_export);
        CHECK(report.accepted_overrides ==
              std::vector<std::string> { "per-layer exposure" });
        // The loss is still in the report; accepting it does not erase it.
        CHECK(report.critical() == 1);
        CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("accepted by override"));
    }

    SECTION("the same override for a different field does not unblock this one")
    {
        const ConversionLossReport report =
            evaluate_conversion(job, destination, "ctb", "goo", { "tilt" });
        CHECK(report.blocks_export);
    }

    SECTION("a destination that can carry it has no loss at all")
    {
        const MachineIdentity capable =
            machine({ ResinCapability::PerLayerExposure, ResinCapability::Thumbnails });
        const ConversionLossReport report =
            evaluate_conversion(job, capable, "ctb", "goo", {});
        CHECK(report.losses.empty());
        CHECK_FALSE(report.blocks_export);
    }
}

TEST_CASE("UV2: a capability the job does not use is not a loss even if the destination lacks it",
          "[resin-manifest]")
{
    // The job sets no per-layer overrides and no tilt, so a destination without
    // those capabilities loses nothing.
    const ResinJobManifest job = simple_job(20);
    const MachineIdentity   destination = machine({ ResinCapability::Thumbnails });
    const ConversionLossReport report =
        evaluate_conversion(job, destination, "ctb", "goo", {});
    CHECK(report.losses.empty());
}

TEST_CASE("UV2: cosmetic losses are noncritical and do not block export", "[resin-manifest]")
{
    ResinJobManifest job = simple_job(20);
    job.thumbnails["preview"]      = 2048;
    job.encoding.antialias_levels  = 4;

    // The destination carries neither thumbnails nor anti-aliasing.
    const MachineIdentity destination = machine({});

    const ConversionLossReport report =
        evaluate_conversion(job, destination, "ctb", "nanodlp", {});
    CHECK_FALSE(report.blocks_export);
    CHECK(report.critical() == 0);
    CHECK(report.noncritical() == 2);
    CHECK(loss_for(report, "thumbnails") != nullptr);
    CHECK(loss_for(report, "anti-aliasing") != nullptr);
    CHECK_THAT(report.summary, Catch::Matchers::ContainsSubstring("export is allowed"));
}

TEST_CASE("UV2: tilt and light-pwm are critical where the destination lacks them",
          "[resin-manifest]")
{
    ResinJobManifest job = simple_job(20);
    job.normal.tilt_deg  = 2.0f;
    job.normal.light_pwm = 200;

    const MachineIdentity destination = machine({ ResinCapability::Thumbnails });
    const ConversionLossReport report =
        evaluate_conversion(job, destination, "ctb", "goo", {});
    CHECK(report.critical() == 2);
    CHECK(loss_for(report, "tilt") != nullptr);
    CHECK(loss_for(report, "light pwm") != nullptr);
    CHECK(report.blocks_export);
}

TEST_CASE("UV2: the loss report is ordered the same way every time", "[resin-manifest]")
{
    ResinJobManifest job = simple_job(20);
    ResinExposure override;
    override.exposure_s              = 3.f;
    job.layers[5].override_exposure  = override;
    job.normal.tilt_deg              = 1.f;
    job.thumbnails["preview"]        = 512;

    const MachineIdentity destination = machine({});

    const ConversionLossReport a = evaluate_conversion(job, destination, "ctb", "goo", {});
    const ConversionLossReport b = evaluate_conversion(job, destination, "ctb", "goo", {});
    REQUIRE(a.losses.size() == b.losses.size());
    for (size_t i = 0; i < a.losses.size(); ++i)
        CHECK(a.losses[i].field == b.losses[i].field);
    // Critical losses sort before noncritical ones.
    REQUIRE(a.losses.size() >= 2);
    CHECK(a.losses.front().severity == LossSeverity::Critical);
    CHECK(a.losses.back().severity == LossSeverity::Noncritical);
}
