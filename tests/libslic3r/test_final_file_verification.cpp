///|/ TigerSlicer Phase UV8 groundwork: final-file verification and the
///|/ deterministic, atomic export decision.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/FinalFileVerification.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

ResinJobManifest manifest_of(size_t layers)
{
    ResinJobManifest job;
    job.schema_version    = resin_manifest_schema_version();
    job.resolution_x      = 3840;
    job.resolution_y      = 2160;
    job.origin            = PixelOrigin::TopLeft;
    job.normal.exposure_s = 2.5f;
    job.bottom.exposure_s = 30.f;
    job.bottom_layers     = 5;
    job.encoding.codec    = "rle";
    job.encoding.lossless = true;
    job.metadata["raster_checksum"] = "canonical";
    for (size_t i = 0; i < layers; ++i) {
        ResinLayer layer;
        layer.z_mm      = double(i + 1) * 0.05;
        layer.height_mm = 0.05;
        job.layers.push_back(layer);
    }
    return job;
}

// Facts that faithfully decode the manifest above.
FinalFileFacts faithful_facts(const ResinJobManifest &job)
{
    FinalFileFacts facts;
    facts.layer_count       = uint32_t(job.layers.size());
    for (const ResinLayer &layer : job.layers)
        facts.layer_z_mm.push_back(*layer.z_mm);
    facts.resolution_x      = job.resolution_x;
    facts.resolution_y      = job.resolution_y;
    facts.origin            = job.origin;
    facts.mirror_x          = job.mirror_x;
    facts.mirror_y          = job.mirror_y;
    facts.rotation_deg      = job.rotation_deg;
    facts.normal_exposure_s = job.normal.exposure_s;
    facts.bottom_exposure_s = job.bottom.exposure_s;
    facts.bottom_layers     = job.bottom_layers;
    facts.thumbnails_present = true;
    facts.format_revision   = "1";
    facts.raster_checksum    = "canonical";
    return facts;
}

ConversionLossReport no_loss()
{
    ConversionLossReport report;
    report.blocks_export = false;
    return report;
}

const VerificationField *field(const FileVerification &v, const std::string &name)
{
    for (const VerificationField &f : v.fields)
        if (f.name == name)
            return &f;
    return nullptr;
}

} // namespace

TEST_CASE("UV8: a faithful decode of the final file verifies", "[final-verify]")
{
    const ResinJobManifest job   = manifest_of(20);
    const FinalFileFacts   facts = faithful_facts(job);

    const FileVerification result = verify_final_file(job, facts, no_loss());
    CHECK(result.verified());
    CHECK(result.mismatches.empty());
    CHECK(result.unchecked_critical.empty());
}

TEST_CASE("UV8: a field that does not match the manifest fails verification", "[final-verify]")
{
    const ResinJobManifest job = manifest_of(20);

    SECTION("a wrong layer count is a failure")
    {
        FinalFileFacts facts = faithful_facts(job);
        facts.layer_count    = 19;
        const FileVerification result = verify_final_file(job, facts, no_loss());
        CHECK(result.verdict == VerificationVerdict::Failed);
        CHECK(std::find(result.mismatches.begin(), result.mismatches.end(), "layer count") !=
              result.mismatches.end());
    }

    SECTION("a wrong exposure is a failure")
    {
        FinalFileFacts facts   = faithful_facts(job);
        facts.normal_exposure_s = 9.9f;
        CHECK(verify_final_file(job, facts, no_loss()).verdict == VerificationVerdict::Failed);
    }

    SECTION("a flipped orientation is a failure the print would show")
    {
        FinalFileFacts facts = faithful_facts(job);
        facts.mirror_x       = true; // manifest is false
        const FileVerification result = verify_final_file(job, facts, no_loss());
        CHECK(result.verdict == VerificationVerdict::Failed);
        CHECK(field(result, "orientation")->outcome == CheckOutcome::Mismatch);
    }

    SECTION("a Z that drifts out of tolerance is a failure")
    {
        FinalFileFacts facts = faithful_facts(job);
        facts.layer_z_mm[10] += 0.02;
        CHECK(verify_final_file(job, facts, no_loss()).verdict == VerificationVerdict::Failed);
    }
}

TEST_CASE("UV8: a print-critical field that could not be decoded is Unproven, not Verified",
          "[final-verify]")
{
    const ResinJobManifest job = manifest_of(20);

    SECTION("no decoded resolution leaves the file unproven")
    {
        FinalFileFacts facts = faithful_facts(job);
        facts.resolution_x.reset();
        facts.resolution_y.reset();
        const FileVerification result = verify_final_file(job, facts, no_loss());
        CHECK(result.verdict == VerificationVerdict::Unproven);
        CHECK(std::find(result.unchecked_critical.begin(), result.unchecked_critical.end(),
                        "resolution") != result.unchecked_critical.end());
    }

    SECTION("an undecoded exposure is likewise unproven, never a silent pass")
    {
        FinalFileFacts facts = faithful_facts(job);
        facts.normal_exposure_s.reset();
        CHECK(verify_final_file(job, facts, no_loss()).verdict == VerificationVerdict::Unproven);
    }

    SECTION("a mismatch outweighs an unchecked field: it is Failed, not Unproven")
    {
        FinalFileFacts facts = faithful_facts(job);
        facts.format_revision.reset();  // unchecked
        facts.layer_count = 5;          // mismatch
        CHECK(verify_final_file(job, facts, no_loss()).verdict == VerificationVerdict::Failed);
    }
}

TEST_CASE("UV8: an expected lossy raster difference is accepted, an unexpected one is not",
          "[final-verify]")
{
    ResinJobManifest job = manifest_of(20);
    job.metadata["raster_checksum"] = "original";
    FinalFileFacts facts = faithful_facts(job);
    facts.raster_checksum = "different-after-reencode";

    SECTION("with no declared loss and a lossless codec, a raster difference fails")
    {
        const FileVerification result = verify_final_file(job, facts, no_loss());
        CHECK(field(result, "raster")->outcome == CheckOutcome::Mismatch);
        CHECK(result.verdict == VerificationVerdict::Failed);
    }

    SECTION("when the UV2 loss report declared a raster loss, the difference is accepted")
    {
        ConversionLossReport loss;
        LossRecord           record;
        record.field    = "raster encoding";
        record.severity = LossSeverity::Noncritical;
        loss.losses.push_back(record);

        const FileVerification result = verify_final_file(job, facts, loss);
        CHECK(field(result, "raster")->outcome == CheckOutcome::AcceptedLoss);
        CHECK(result.verified());
    }

    SECTION("a lossy codec alone accounts for the difference")
    {
        job.encoding.lossless = false;
        const FileVerification result = verify_final_file(job, facts, no_loss());
        CHECK(field(result, "raster")->outcome == CheckOutcome::AcceptedLoss);
    }
}

TEST_CASE("UV8: publish is atomic and never overwrites a good export with a bad one",
          "[final-verify]")
{
    const ResinJobManifest job = manifest_of(20);

    FileVerification good;
    good.verdict = VerificationVerdict::Verified;
    FileVerification failed;
    failed.verdict = VerificationVerdict::Failed;
    FileVerification unproven;
    unproven.verdict = VerificationVerdict::Unproven;

    SECTION("a verified file publishes")
    {
        PublishState state;
        state.temp_written = true;
        CHECK(plan_publish(good, state).decision == PublishDecision::Publish);
    }

    SECTION("a failed verification keeps the last known-good")
    {
        PublishState state;
        state.temp_written  = true;
        state.has_last_good = true;
        const PublishPlan plan = plan_publish(failed, state);
        CHECK(plan.decision == PublishDecision::KeepLastGood);
        CHECK_THAT(plan.message, Catch::Matchers::ContainsSubstring("last known-good"));
    }

    SECTION("an unproven verification also keeps the last known-good")
    {
        PublishState state;
        state.has_last_good = true;
        CHECK(plan_publish(unproven, state).decision == PublishDecision::KeepLastGood);
    }

    SECTION("a failure with no last-good discards; nothing is presented")
    {
        PublishState state;
        state.temp_written = true;
        CHECK(plan_publish(failed, state).decision == PublishDecision::Discard);
    }
}
