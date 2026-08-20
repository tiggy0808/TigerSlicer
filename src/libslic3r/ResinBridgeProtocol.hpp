///|/ TigerSlicer Phase UV1 groundwork: the versioned resin-bridge protocol.
///|/
///|/ The message contract between TigerSlicer and the out-of-process,
///|/ pinned UVtools helper (the integration boundary decided in
///|/ doc/TigerSlicerContinuationRoadmap.md). This is the schema and its rules
///|/ only - it spawns no process and links no UVtools code. It is testable on
///|/ Linux like every other decision layer, and it is what an eventual IPC
///|/ transport serializes; it never parses human-readable console text.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ResinBridgeProtocol_hpp_
#define slic3r_ResinBridgeProtocol_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ResinJobManifest.hpp"

namespace Slic3r {

// The protocol version, separate from the MSlicer ABI version and from the
// manifest schema version: the wire contract can change without either of the
// other two moving. A message from a version this build does not speak is
// refused whole rather than partially read.
int resin_bridge_protocol_version();

// What the host asks the helper to do. Each maps to a UVtools.Core capability;
// an operation the negotiated capability set does not cover is refused before
// anything is sent.
enum class BridgeOperation : uint8_t
{
    Probe,      // report identity and capabilities
    Validate,   // check a job/file without producing output
    Inspect,    // read an existing output file's structure
    Repair,     // apply audited repair operations
    Convert,    // re-encode a job into a destination format
};

const char *bridge_operation_name(BridgeOperation operation);

// Deterministic status of a completed exchange. These mirror the MSlicer ABI
// status vocabulary so a caller does not have to learn two, plus the failures
// that are specific to driving a separate process.
enum class BridgeStatus : uint8_t
{
    Ok,
    InvalidArgument,
    InvalidConfig,
    Unsupported,       // the operation or a required capability is not available
    Cancelled,         // the host cancelled it
    Timeout,           // the deadline passed
    HelperCrash,       // the process died without a structured result
    VersionMismatch,   // the helper is not the pinned/expected build
    ProtocolError,     // a malformed or wrong-version message
    EngineError,       // the helper ran but failed
};

const char *bridge_status_name(BridgeStatus status);

// A capability the two sides negotiate. Named the same way as the MSlicer and
// manifest capability vocabularies so the whole system speaks one language.
enum class BridgeCapability : uint8_t
{
    ReadCtb,
    WriteCtb,
    ReadGoo,
    WriteGoo,
    ReadNanoDlp,
    WriteNanoDlp,
    Thumbnails,
    IssueDetection,
    Repair,
    LosslessConvert,
    Progress,
    Cancellation,
};

const char *bridge_capability_name(BridgeCapability capability);

// Who the helper is. Reported by Probe, and checked against what the host
// pinned. A development override is allowed but must be visibly marked; it
// never silently supersedes the pinned production build.
struct HelperIdentity
{
    std::string protocol_version_text;   // the protocol version the helper speaks
    int         protocol_version { 0 };
    std::string helper_version;          // the helper's own version
    std::string uvtools_version;         // the UVtools revision it wraps
    std::string source_revision;         // the pinned source commit it was built from
    bool        development_override { false };

    std::vector<BridgeCapability> capabilities;

    bool has(BridgeCapability capability) const;
};

// What the host expects, pinned at build time. A helper that does not match is
// a VersionMismatch unless it is an explicitly acknowledged development
// override.
struct PinnedExpectation
{
    int         protocol_version { 0 };
    std::string source_revision;
    // When true, a development-override helper is accepted (and still marked);
    // when false, only the pinned source revision is accepted.
    bool allow_development_override { false };
};

// The result of checking a helper against what was pinned.
struct IdentityCheck
{
    BridgeStatus status { BridgeStatus::VersionMismatch };
    std::string  message;
    // True when a development override was accepted; the caller must surface
    // this so a dev build is never mistaken for the production one.
    bool accepted_development_override { false };

    bool ok() const { return status == BridgeStatus::Ok; }
};

// Check a probed helper against the pinned expectation. A protocol-version
// mismatch is fatal; a source-revision mismatch is fatal unless it is an
// acknowledged development override.
IdentityCheck check_helper_identity(const HelperIdentity    &helper,
                                    const PinnedExpectation &expectation);

// --- capability negotiation -------------------------------------------------

struct Negotiation
{
    // The intersection of what the host wants and what the helper offers.
    std::vector<BridgeCapability> agreed;
    // Capabilities the host required that the helper does not offer. Non-empty
    // means the exchange cannot proceed.
    std::vector<BridgeCapability> missing_required;
    // Capabilities the host would like but can do without, that are absent.
    std::vector<BridgeCapability> missing_optional;

    bool usable() const { return missing_required.empty(); }
};

// Negotiate the capability set for an exchange. Required capabilities that the
// helper lacks are reported, never assumed present; optional ones that are
// absent are noted so the caller can degrade explicitly.
Negotiation negotiate(const HelperIdentity                &helper,
                      const std::vector<BridgeCapability> &required,
                      const std::vector<BridgeCapability> &optional);

// --- request / response envelopes ------------------------------------------

struct BridgeRequest
{
    int         protocol_version { 0 };
    std::string request_id;      // echoed in the response; non-empty
    BridgeOperation operation { BridgeOperation::Probe };

    // The capabilities this request requires and would like. An operation is
    // refused if a required one is not negotiated.
    std::vector<BridgeCapability> required_capabilities;
    std::vector<BridgeCapability> optional_capabilities;

    // The job the operation acts on (empty for Probe).
    std::optional<ResinJobManifest> job;
    // For Convert: where the job is going.
    std::string destination_format;

    // A monotonic deadline in the caller's clock, and how long the caller will
    // wait. Zero means no deadline. Nothing here reads a clock; the transport
    // supplies now to check it.
    int64_t deadline_s { 0 };
};

// One progress event. Fraction is monotonic non-decreasing within an exchange;
// an out-of-order event is a ProtocolError, not silently reordered.
struct BridgeProgress
{
    std::string request_id;
    double      fraction { 0. };   // 0..1
    std::string stage;
};

struct BridgeResponse
{
    int          protocol_version { 0 };
    std::string  request_id;       // must match the request
    BridgeStatus status { BridgeStatus::ProtocolError };
    std::string  message;

    // The result payload, opaque here (a serialized manifest, an inspection
    // report, a repaired file reference). The protocol carries it; the format
    // layer interprets it.
    std::string payload;

    // Structured diagnostics, never free console text.
    std::vector<std::string> diagnostics;

    // For Convert: what the conversion lost, so the host applies its own
    // export policy rather than trusting the helper's.
    std::optional<ConversionLossReport> loss;

    bool ok() const { return status == BridgeStatus::Ok; }
};

// Validate a request against the protocol before it is sent: the right
// protocol version, a request id, and - for operations that need one - a job.
// Returns Ok, or the status the transport should fail with without sending.
BridgeStatus validate_request(const BridgeRequest &request);

// Check that a response answers the request it claims to: matching id and
// protocol version. A response for another request, or from another protocol
// version, is a ProtocolError rather than being accepted.
BridgeStatus match_response(const BridgeRequest &request, const BridgeResponse &response);

// Fold a sequence of progress events for one exchange, rejecting a
// regression. Returns Ok and the final fraction, or ProtocolError at the first
// event that goes backwards or names the wrong request.
struct ProgressCheck
{
    BridgeStatus status { BridgeStatus::Ok };
    double       final_fraction { 0. };
    size_t       accepted { 0 };
    std::string  message;
};

ProgressCheck check_progress(const std::string                 &request_id,
                             const std::vector<BridgeProgress> &events);

// Decide the terminal status of an exchange from the raw process outcome. A
// helper that produced no structured response is a HelperCrash; one that ran
// past the deadline is a Timeout; one that returned a response has whatever
// status the response carried (after the id/version match). This is the one
// place the "no result" and "late" cases are turned into deterministic codes.
struct ProcessOutcome
{
    bool                          produced_response { false };
    bool                          exceeded_deadline { false };
    bool                          process_exited_nonzero { false };
    std::optional<BridgeResponse> response;
};

BridgeResponse resolve_exchange(const BridgeRequest &request, const ProcessOutcome &outcome);

} // namespace Slic3r

#endif // slic3r_ResinBridgeProtocol_hpp_
