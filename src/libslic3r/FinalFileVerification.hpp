///|/ TigerSlicer Phase UV8 groundwork: final-file verification and the
///|/ deterministic, atomic export decision.
///|/
///|/ After MSlicer output and any UVtools transformation, the exact final byte
///|/ stream is decoded and checked against the canonical job manifest before it
///|/ is published. This module is the checking and the publish decision, as
///|/ testable libslic3r logic; the decode itself runs behind the UV1 bridge and
///|/ links no UVtools code here.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FinalFileVerification_hpp_
#define slic3r_FinalFileVerification_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ResinJobManifest.hpp"

namespace Slic3r {

// What was decoded back out of the exported file. Every field optional: a
// decoder that could not read a field leaves it absent, which the verifier
// reports as "could not be checked" rather than as a match.
struct FinalFileFacts
{
    std::optional<uint32_t> layer_count;
    // The Z of each layer top, in order, as decoded. Empty when not read.
    std::vector<double>     layer_z_mm;
    std::optional<uint32_t> resolution_x, resolution_y;
    std::optional<PixelOrigin> origin;
    std::optional<bool>     mirror_x, mirror_y;
    std::optional<int>      rotation_deg;

    std::optional<float> normal_exposure_s;
    std::optional<float> bottom_exposure_s;
    std::optional<uint32_t> bottom_layers;

    bool thumbnails_present { false };
    std::optional<std::string> format_revision;
    // A checksum of the decoded raster payload, for the round-trip check.
    std::optional<std::string> raster_checksum;
};

// The result for one checked field.
enum class CheckOutcome : uint8_t
{
    Match,
    // Different, and the difference matters: a hard failure.
    Mismatch,
    // Different, but within what the declared lossy encoding accounts for.
    AcceptedLoss,
    // The decoder did not supply the field, so it could not be checked. Never
    // silently a Match.
    Unchecked,
};

const char *check_outcome_name(CheckOutcome outcome);

struct VerificationField
{
    std::string  name;
    CheckOutcome outcome { CheckOutcome::Unchecked };
    std::string  expected;
    std::string  actual;
    std::string  note;
};

enum class VerificationVerdict : uint8_t
{
    // Every checkable field matched (losses accounted for).
    Verified,
    // At least one field mismatched: the file does not match the manifest.
    Failed,
    // Nothing mismatched, but a print-critical field could not be checked, so
    // the file is not proven to match. Not the same as Verified.
    Unproven,
};

const char *verification_verdict_name(VerificationVerdict verdict);

struct FileVerification
{
    VerificationVerdict verdict { VerificationVerdict::Unproven };
    std::vector<VerificationField> fields;
    // The fields that mismatched and the print-critical ones left unchecked,
    // for a quick summary.
    std::vector<std::string> mismatches;
    std::vector<std::string> unchecked_critical;
    std::string              summary;

    bool verified() const { return verdict == VerificationVerdict::Verified; }
};

// Verify decoded final-file facts against the canonical manifest. The
// conversion-loss report (from Phase UV2) says which differences are expected:
// a raster mismatch is an AcceptedLoss when the report classified the raster
// encoding as a noncritical loss, and a Mismatch otherwise. A print-critical
// field the decoder could not supply leaves the verdict Unproven, never
// Verified.
FileVerification verify_final_file(const ResinJobManifest      &manifest,
                                   const FinalFileFacts        &facts,
                                   const ConversionLossReport  &loss);

// --- atomic publish ---------------------------------------------------------

enum class PublishDecision : uint8_t
{
    // Verification passed: publish the temporary file to the final path.
    Publish,
    // Verification did not pass: keep the last known-good and discard the temp.
    // The last-good file is never overwritten with an unverified result.
    KeepLastGood,
    // There is no last-good and verification did not pass: nothing to publish
    // and nothing to keep. The caller must not present a file.
    Discard,
};

const char *publish_decision_name(PublishDecision decision);

struct PublishState
{
    // A verified file already at the final path, if any.
    bool has_last_good { false };
    // The temp file was written and its bytes decoded for verification.
    bool temp_written { false };
};

struct PublishPlan
{
    PublishDecision decision { PublishDecision::Discard };
    std::string     message;

    bool publishes() const { return decision == PublishDecision::Publish; }
};

// Decide the atomic-publish outcome from a verification and the current state.
// Publish only a Verified file; on anything else keep the last known-good if
// there is one, else discard. This is the one place the "never overwrite a
// good export with a bad one" rule is enforced.
PublishPlan plan_publish(const FileVerification &verification, const PublishState &state);

} // namespace Slic3r

#endif // slic3r_FinalFileVerification_hpp_
