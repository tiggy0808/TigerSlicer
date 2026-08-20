///|/ TigerSlicer Phase UV1 groundwork: the versioned resin-bridge protocol.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ResinBridgeProtocol.hpp"

#include <algorithm>

namespace Slic3r {

int resin_bridge_protocol_version()
{
    return 1;
}

const char *bridge_operation_name(BridgeOperation operation)
{
    switch (operation) {
    case BridgeOperation::Probe:    return "probe";
    case BridgeOperation::Validate: return "validate";
    case BridgeOperation::Inspect:  return "inspect";
    case BridgeOperation::Repair:   return "repair";
    case BridgeOperation::Convert:  return "convert";
    }
    return "probe";
}

const char *bridge_status_name(BridgeStatus status)
{
    switch (status) {
    case BridgeStatus::Ok:              return "ok";
    case BridgeStatus::InvalidArgument: return "invalid argument";
    case BridgeStatus::InvalidConfig:   return "invalid config";
    case BridgeStatus::Unsupported:     return "unsupported";
    case BridgeStatus::Cancelled:       return "cancelled";
    case BridgeStatus::Timeout:         return "timeout";
    case BridgeStatus::HelperCrash:     return "helper crash";
    case BridgeStatus::VersionMismatch: return "version mismatch";
    case BridgeStatus::ProtocolError:   return "protocol error";
    case BridgeStatus::EngineError:     return "engine error";
    }
    return "protocol error";
}

const char *bridge_capability_name(BridgeCapability capability)
{
    switch (capability) {
    case BridgeCapability::ReadCtb:         return "read ctb";
    case BridgeCapability::WriteCtb:        return "write ctb";
    case BridgeCapability::ReadGoo:         return "read goo";
    case BridgeCapability::WriteGoo:        return "write goo";
    case BridgeCapability::ReadNanoDlp:     return "read nanodlp";
    case BridgeCapability::WriteNanoDlp:    return "write nanodlp";
    case BridgeCapability::Thumbnails:      return "thumbnails";
    case BridgeCapability::IssueDetection:  return "issue detection";
    case BridgeCapability::Repair:          return "repair";
    case BridgeCapability::LosslessConvert: return "lossless convert";
    case BridgeCapability::Progress:        return "progress";
    case BridgeCapability::Cancellation:    return "cancellation";
    }
    return "read ctb";
}

bool HelperIdentity::has(BridgeCapability capability) const
{
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

// --- identity ---------------------------------------------------------------

IdentityCheck check_helper_identity(const HelperIdentity    &helper,
                                    const PinnedExpectation &expectation)
{
    IdentityCheck check;

    if (helper.protocol_version != expectation.protocol_version) {
        check.status  = BridgeStatus::VersionMismatch;
        check.message = "the helper speaks protocol version " +
                        std::to_string(helper.protocol_version) + " but this build pinned " +
                        std::to_string(expectation.protocol_version);
        return check;
    }

    if (helper.source_revision != expectation.source_revision) {
        // A source-revision mismatch is a development override. It is accepted
        // only when the caller allowed it, and even then it is marked so a dev
        // build is never mistaken for the pinned production one.
        if (helper.development_override && expectation.allow_development_override) {
            check.status                        = BridgeStatus::Ok;
            check.accepted_development_override = true;
            check.message = "a development-override helper (" + helper.source_revision +
                            ") was accepted in place of the pinned revision " +
                            expectation.source_revision + "; it is marked as non-production";
            return check;
        }
        check.status  = BridgeStatus::VersionMismatch;
        check.message = "the helper was built from " + helper.source_revision +
                        " but this build pinned " + expectation.source_revision +
                        (helper.development_override
                             ? "; the development override was not permitted here"
                             : "");
        return check;
    }

    check.status  = BridgeStatus::Ok;
    check.message = "the helper matches the pinned protocol version and source revision";
    return check;
}

// --- negotiation ------------------------------------------------------------

Negotiation negotiate(const HelperIdentity                &helper,
                      const std::vector<BridgeCapability> &required,
                      const std::vector<BridgeCapability> &optional)
{
    Negotiation negotiation;

    for (BridgeCapability capability : required) {
        if (helper.has(capability))
            negotiation.agreed.push_back(capability);
        else
            negotiation.missing_required.push_back(capability);
    }
    for (BridgeCapability capability : optional) {
        if (helper.has(capability))
            negotiation.agreed.push_back(capability);
        else
            negotiation.missing_optional.push_back(capability);
    }

    // Stable, de-duplicated agreed set.
    std::sort(negotiation.agreed.begin(), negotiation.agreed.end());
    negotiation.agreed.erase(std::unique(negotiation.agreed.begin(), negotiation.agreed.end()),
                             negotiation.agreed.end());
    std::sort(negotiation.missing_required.begin(), negotiation.missing_required.end());
    std::sort(negotiation.missing_optional.begin(), negotiation.missing_optional.end());
    return negotiation;
}

// --- request / response -----------------------------------------------------

namespace {

bool operation_needs_job(BridgeOperation operation)
{
    return operation != BridgeOperation::Probe;
}

} // namespace

BridgeStatus validate_request(const BridgeRequest &request)
{
    if (request.protocol_version != resin_bridge_protocol_version())
        return BridgeStatus::ProtocolError;
    if (request.request_id.empty())
        return BridgeStatus::InvalidArgument;
    if (operation_needs_job(request.operation) && ! request.job.has_value())
        return BridgeStatus::InvalidArgument;
    if (request.operation == BridgeOperation::Convert && request.destination_format.empty())
        return BridgeStatus::InvalidArgument;
    return BridgeStatus::Ok;
}

BridgeStatus match_response(const BridgeRequest &request, const BridgeResponse &response)
{
    if (response.protocol_version != request.protocol_version)
        return BridgeStatus::ProtocolError;
    if (response.request_id != request.request_id)
        return BridgeStatus::ProtocolError;
    return BridgeStatus::Ok;
}

ProgressCheck check_progress(const std::string                 &request_id,
                             const std::vector<BridgeProgress> &events)
{
    ProgressCheck check;
    double        last = 0.;

    for (const BridgeProgress &event : events) {
        if (event.request_id != request_id) {
            check.status  = BridgeStatus::ProtocolError;
            check.message = "a progress event named request \"" + event.request_id +
                            "\" during exchange \"" + request_id + "\"";
            return check;
        }
        if (event.fraction < 0. || event.fraction > 1.) {
            check.status  = BridgeStatus::ProtocolError;
            check.message = "a progress fraction was outside 0..1";
            return check;
        }
        if (event.fraction + 1e-9 < last) {
            check.status  = BridgeStatus::ProtocolError;
            check.message = "progress went backwards from " + std::to_string(last) + " to " +
                            std::to_string(event.fraction);
            return check;
        }
        last = event.fraction;
        ++check.accepted;
    }

    check.status         = BridgeStatus::Ok;
    check.final_fraction = last;
    return check;
}

BridgeResponse resolve_exchange(const BridgeRequest &request, const ProcessOutcome &outcome)
{
    BridgeResponse response;
    response.protocol_version = request.protocol_version;
    response.request_id       = request.request_id;

    // The deadline is decided first: a helper that produced a result but only
    // after the deadline is still a Timeout, because the host has stopped
    // waiting for it and its output cannot be trusted to be current.
    if (outcome.exceeded_deadline) {
        response.status  = BridgeStatus::Timeout;
        response.message = "the exchange passed its deadline";
        return response;
    }

    if (! outcome.produced_response || ! outcome.response.has_value()) {
        // No structured result at all. Whether the process exited nonzero,
        // vanished, or flagged a response it never populated, from the
        // protocol's view it crashed: it did not answer. Gate on the optional
        // itself, not just the redundant flag, so an inconsistent
        // (flag-set/response-empty) outcome is never dereferenced below.
        response.status  = BridgeStatus::HelperCrash;
        response.message = outcome.process_exited_nonzero
                               ? "the helper exited without a structured response"
                               : "the helper produced no response";
        return response;
    }

    // A response was produced; it still has to answer this request.
    const BridgeResponse &given  = *outcome.response;
    const BridgeStatus    match = match_response(request, given);
    if (match != BridgeStatus::Ok) {
        response.status  = match;
        response.message = "the helper's response did not match this request";
        return response;
    }

    return given;
}

} // namespace Slic3r
