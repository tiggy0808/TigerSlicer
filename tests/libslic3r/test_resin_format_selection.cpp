///|/ TigerSlicer Phase UV3 groundwork: output format and revision selection.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/ResinFormatSelection.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

ResinFormatOption option(const std::string &format, const std::string &revision,
                         FormatSupportEvidence evidence,
                         std::vector<ResinCapability> capabilities = {})
{
    ResinFormatOption o;
    o.format       = format;
    o.revision     = revision;
    o.evidence     = evidence;
    o.capabilities = std::move(capabilities);
    return o;
}

ResinJobManifest plain_job()
{
    ResinJobManifest job;
    job.schema_version = resin_manifest_schema_version();
    ResinLayer layer;
    layer.z_mm      = 0.05;
    layer.height_mm = 0.05;
    job.layers.push_back(layer);
    return job;
}

ResinJobManifest job_needing(ResinCapability capability)
{
    ResinJobManifest job = plain_job();
    if (capability == ResinCapability::PerLayerExposure) {
        ResinExposure e;
        e.exposure_s = 3.f;
        job.layers[0].override_exposure = e;
    } else if (capability == ResinCapability::Tilt) {
        job.normal.tilt_deg = 2.f;
    } else if (capability == ResinCapability::Thumbnails) {
        job.thumbnails["preview"] = 128;
    }
    return job;
}

bool contains(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool needs(const std::vector<ResinCapability> &v, ResinCapability c)
{
    return std::find(v.begin(), v.end(), c) != v.end();
}

} // namespace

TEST_CASE("UV3: the job's required capabilities are read off what it actually uses",
          "[format-selection]")
{
    CHECK(job_required_capabilities(plain_job()).empty());
    CHECK(needs(job_required_capabilities(job_needing(ResinCapability::PerLayerExposure)),
                ResinCapability::PerLayerExposure));
    CHECK(needs(job_required_capabilities(job_needing(ResinCapability::Tilt)),
                ResinCapability::Tilt));
    CHECK(needs(job_required_capabilities(job_needing(ResinCapability::Thumbnails)),
                ResinCapability::Thumbnails));
}

TEST_CASE("UV3: the strongest-evidence format that carries the most of the job wins",
          "[format-selection]")
{
    PrinterFormatProfile profile;
    profile.printer = "Saturn";

    SECTION("hardware-verified beats profile-declared")
    {
        profile.options = { option("ctb", "4", FormatSupportEvidence::ProfileDeclared),
                            option("goo", "1", FormatSupportEvidence::VerifiedOnHardware) };
        const FormatSelection selection = select_output_format(profile, plain_job());
        CHECK(selection.ok());
        CHECK(selection.format.value() == "goo");
        CHECK(selection.evidence == FormatSupportEvidence::VerifiedOnHardware);
    }

    SECTION("among equal evidence, the one that carries the job's capability is chosen")
    {
        profile.options = {
            option("ctb", "4", FormatSupportEvidence::ProfileDeclared, {}),
            option("goo", "1", FormatSupportEvidence::ProfileDeclared,
                   { ResinCapability::PerLayerExposure }),
        };
        const FormatSelection selection =
            select_output_format(profile, job_needing(ResinCapability::PerLayerExposure));
        CHECK(selection.format.value() == "goo");
        CHECK(selection.unmet_capabilities.empty());
    }

    SECTION("the chosen format that cannot carry a needed capability reports it, unresolved")
    {
        profile.options = { option("ctb", "4", FormatSupportEvidence::VerifiedOnHardware, {}) };
        const FormatSelection selection =
            select_output_format(profile, job_needing(ResinCapability::PerLayerExposure));
        CHECK(selection.ok()); // it is still the format; the loss gate decides export
        CHECK(needs(selection.unmet_capabilities, ResinCapability::PerLayerExposure));
    }
}

TEST_CASE("UV3: a genuine tie between different formats is reported, not broken by order",
          "[format-selection]")
{
    PrinterFormatProfile profile;
    profile.options = { option("ctb", "4", FormatSupportEvidence::VerifiedOnHardware),
                        option("goo", "1", FormatSupportEvidence::VerifiedOnHardware) };
    const FormatSelection selection = select_output_format(profile, plain_job());
    CHECK(selection.verdict == FormatSelectionVerdict::Ambiguous);
    CHECK(contains(selection.tied, "ctb"));
    CHECK(contains(selection.tied, "goo"));

    // The same format at two revisions is not an ambiguity.
    PrinterFormatProfile revisions;
    revisions.options = { option("ctb", "3", FormatSupportEvidence::VerifiedOnHardware),
                          option("ctb", "4", FormatSupportEvidence::VerifiedOnHardware) };
    CHECK(select_output_format(revisions, plain_job()).ok());
}

TEST_CASE("UV3: a pinned format is honoured when offered and never silently swapped",
          "[format-selection]")
{
    PrinterFormatProfile profile;
    profile.options = { option("ctb", "4", FormatSupportEvidence::VerifiedOnHardware),
                        option("goo", "1", FormatSupportEvidence::ProfileDeclared) };

    SECTION("the pinned format wins even when a stronger option exists")
    {
        profile.pinned_format = std::string("goo");
        const FormatSelection selection = select_output_format(profile, plain_job());
        CHECK(selection.verdict == FormatSelectionVerdict::Pinned);
        CHECK(selection.format.value() == "goo");
    }

    SECTION("a pinned format the printer does not offer is reported, not replaced")
    {
        profile.pinned_format = std::string("cbddlp");
        const FormatSelection selection = select_output_format(profile, plain_job());
        CHECK(selection.verdict == FormatSelectionVerdict::PinnedUnavailable);
        CHECK_FALSE(selection.format.has_value());
        CHECK_THAT(selection.message, Catch::Matchers::ContainsSubstring("not offered"));
    }
}

TEST_CASE("UV3: require-verified excludes structural-only options", "[format-selection]")
{
    PrinterFormatProfile profile;
    profile.require_verified = true;
    profile.options = { option("ctb", "4", FormatSupportEvidence::StructurallySupported),
                        option("goo", "1", FormatSupportEvidence::VerifiedOnHardware) };

    const FormatSelection selection = select_output_format(profile, plain_job());
    CHECK(selection.format.value() == "goo");

    SECTION("a pinned but unverified format under require-verified is reported distinctly")
    {
        profile.pinned_format = std::string("ctb");
        const FormatSelection pinned = select_output_format(profile, plain_job());
        CHECK(pinned.verdict == FormatSelectionVerdict::PinnedUnavailable);
        CHECK_THAT(pinned.message, Catch::Matchers::ContainsSubstring("not hardware-verified"));
    }

    SECTION("nothing verified at all is no-compatible-format")
    {
        PrinterFormatProfile strict;
        strict.require_verified = true;
        strict.options = { option("ctb", "4", FormatSupportEvidence::StructurallySupported) };
        CHECK(select_output_format(strict, plain_job()).verdict ==
              FormatSelectionVerdict::NoCompatibleFormat);
    }
}

TEST_CASE("UV3: a printer with no declared formats selects nothing", "[format-selection]")
{
    PrinterFormatProfile profile;
    CHECK(select_output_format(profile, plain_job()).verdict ==
          FormatSelectionVerdict::NoFormatsDeclared);
}

TEST_CASE("UV7.6: a committed revision the printer no longer offers blocks export",
          "[format-selection]")
{
    PrinterFormatProfile profile;
    profile.options = { option("ctb", "4", FormatSupportEvidence::VerifiedOnHardware) };

    SECTION("an exact match agrees and does not block")
    {
        const RevisionCheck check = check_committed_revision(profile, "ctb", "4");
        CHECK(check.agreement == RevisionAgreement::Agrees);
        CHECK_FALSE(check.blocks_export);
    }

    SECTION("a changed revision blocks rather than re-encoding silently")
    {
        const RevisionCheck check = check_committed_revision(profile, "ctb", "3");
        CHECK(check.agreement == RevisionAgreement::RevisionChanged);
        CHECK(check.blocks_export);
        CHECK_THAT(check.message, Catch::Matchers::ContainsSubstring("blocked"));
    }

    SECTION("a dropped format blocks rather than routing to another")
    {
        const RevisionCheck check = check_committed_revision(profile, "goo", "1");
        CHECK(check.agreement == RevisionAgreement::FormatDropped);
        CHECK(check.blocks_export);
    }
}
