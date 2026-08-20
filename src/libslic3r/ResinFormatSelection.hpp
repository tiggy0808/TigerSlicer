///|/ TigerSlicer Phase UV3 groundwork: output format and revision selection.
///|/
///|/ The decision of *which* resin format and revision to encode to, tied to a
///|/ printer profile's verified capabilities - the routing that happens before
///|/ any bytes are written. The encode itself (UV3, blocked on the UVtools
///|/ source and real fixtures) runs behind the UV1 bridge; this is the choice
///|/ that precedes it, as testable libslic3r logic linking no UVtools code.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ResinFormatSelection_hpp_
#define slic3r_ResinFormatSelection_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ResinJobManifest.hpp"

namespace Slic3r {

// A format+revision a printer profile declares it can consume, with how well
// that support is evidenced. A format the printer merely might decode is not
// the same as one it is verified to print.
enum class FormatSupportEvidence : uint8_t
{
    // Ordered weakest-first so a numeric compare ranks them.
    Unknown,
    StructurallySupported,   // the encoder can write it, but no printer confirmed it
    ProfileDeclared,         // the printer profile lists it
    VerifiedOnHardware,      // a printer actually printed it
};

const char *format_support_evidence_name(FormatSupportEvidence evidence);

struct ResinFormatOption
{
    std::string           format;        // "ctb", "goo", "nanodlp"
    std::string           revision;      // format revision, when it matters
    FormatSupportEvidence evidence { FormatSupportEvidence::Unknown };

    // The capabilities this format+revision can carry, so a job needing a
    // semantic the format lacks is not routed to it (that is UV2's loss
    // report; here it only affects which option is preferred).
    std::vector<ResinCapability> capabilities;

    bool has(ResinCapability capability) const;
};

// What the printer profile offers, plus which format the user pinned (if any).
struct PrinterFormatProfile
{
    std::string                    printer;
    std::vector<ResinFormatOption> options;
    // A format the user chose explicitly. It is honoured if it is offered at
    // all; the selection never silently switches away from it.
    std::optional<std::string> pinned_format;
    // When true, only VerifiedOnHardware options may be selected - the strict
    // setting for a machine the user does not want to risk on structural-only
    // support.
    bool require_verified { false };
};

enum class FormatSelectionVerdict : uint8_t
{
    // A single best format was chosen.
    Selected,
    // The user's pinned format was honoured.
    Pinned,
    // Several options are equally good and the user must choose.
    Ambiguous,
    // Nothing the printer offers can carry what the job needs.
    NoCompatibleFormat,
    // The printer profile offers no formats at all.
    NoFormatsDeclared,
    // The pinned format is not offered by the printer.
    PinnedUnavailable,
};

const char *format_selection_verdict_name(FormatSelectionVerdict verdict);

struct FormatSelection
{
    FormatSelectionVerdict verdict { FormatSelectionVerdict::NoFormatsDeclared };

    std::optional<std::string> format;
    std::optional<std::string> revision;
    FormatSupportEvidence      evidence { FormatSupportEvidence::Unknown };

    // The capabilities the job needs that the chosen format cannot carry, if
    // any. Empty for a clean selection; non-empty means the caller must run the
    // UV2 loss gate before committing.
    std::vector<ResinCapability> unmet_capabilities;

    // Equally-good options, when the verdict is Ambiguous.
    std::vector<std::string> tied;

    std::string message;

    bool ok() const
    {
        return verdict == FormatSelectionVerdict::Selected ||
               verdict == FormatSelectionVerdict::Pinned;
    }
};

// The capabilities a job actually exercises, so selection prefers a format that
// can carry them and reports the ones no offered format can.
std::vector<ResinCapability> job_required_capabilities(const ResinJobManifest &job);

// Choose the output format and revision for a job on a printer.
//
// A pinned format is honoured when offered (and reported PinnedUnavailable when
// not); it is never silently swapped for another. Otherwise the option with the
// strongest support evidence that carries the most of what the job needs wins;
// a tie between equally-good options is reported for the user to break, never
// resolved by list order. When require_verified is set, only hardware-verified
// options are eligible. A format is only "compatible" here in the sense of
// being offered and carrying the job's semantics as far as it can; the UV2 loss
// report remains the authority on whether an unmet capability blocks export.
FormatSelection select_output_format(const PrinterFormatProfile &profile,
                                     const ResinJobManifest     &job);

// --- format-revision conflict (UV7.6) --------------------------------------

// Whether a job already tied to a format+revision still agrees with the
// printer's currently offered support. A project sliced for one revision that
// the printer no longer offers must not export silently to another.
enum class RevisionAgreement : uint8_t
{
    Agrees,
    // The printer offers the format but a different revision.
    RevisionChanged,
    // The printer no longer offers the format at all.
    FormatDropped,
};

const char *revision_agreement_name(RevisionAgreement agreement);

struct RevisionCheck
{
    RevisionAgreement agreement { RevisionAgreement::FormatDropped };
    std::string       message;
    // True when export must be blocked until the mismatch is resolved.
    bool blocks_export { true };
};

// Check a job's committed format+revision against the printer profile. A
// changed revision or dropped format blocks export rather than re-encoding to
// whatever the printer now offers.
RevisionCheck check_committed_revision(const PrinterFormatProfile &profile,
                                       const std::string          &committed_format,
                                       const std::string          &committed_revision);

} // namespace Slic3r

#endif // slic3r_ResinFormatSelection_hpp_
