///|/ TigerSlicer Phase UV1 groundwork: the versioned resin-bridge protocol.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/ResinBridgeProtocol.hpp"

using namespace Slic3r;

namespace {

HelperIdentity pinned_helper()
{
    HelperIdentity helper;
    helper.protocol_version = resin_bridge_protocol_version();
    helper.helper_version   = "1.4.0";
    helper.uvtools_version  = "5.0.4";
    helper.source_revision  = "865562dac0";
    helper.capabilities     = { BridgeCapability::ReadCtb,  BridgeCapability::WriteCtb,
                                BridgeCapability::ReadGoo,  BridgeCapability::WriteGoo,
                                BridgeCapability::Thumbnails, BridgeCapability::Progress,
                                BridgeCapability::Cancellation };
    return helper;
}

PinnedExpectation pinned_expectation()
{
    PinnedExpectation expectation;
    expectation.protocol_version         = resin_bridge_protocol_version();
    expectation.source_revision          = "865562dac0";
    expectation.allow_development_override = false;
    return expectation;
}

BridgeRequest convert_request()
{
    BridgeRequest request;
    request.protocol_version     = resin_bridge_protocol_version();
    request.request_id           = "req-1";
    request.operation            = BridgeOperation::Convert;
    request.destination_format   = "goo";
    request.job                  = ResinJobManifest {};
    request.required_capabilities = { BridgeCapability::WriteGoo };
    return request;
}

BridgeResponse good_response(const std::string &id)
{
    BridgeResponse response;
    response.protocol_version = resin_bridge_protocol_version();
    response.request_id       = id;
    response.status           = BridgeStatus::Ok;
    return response;
}

BridgeProgress progress(const std::string &id, double fraction)
{
    BridgeProgress event;
    event.request_id = id;
    event.fraction   = fraction;
    return event;
}

} // namespace

TEST_CASE("UV1: a helper is checked against the pinned protocol and source revision",
          "[resin-bridge]")
{
    SECTION("the pinned helper matches")
    {
        const IdentityCheck check = check_helper_identity(pinned_helper(), pinned_expectation());
        CHECK(check.ok());
        CHECK_FALSE(check.accepted_development_override);
    }

    SECTION("a different protocol version is a fatal mismatch")
    {
        HelperIdentity helper   = pinned_helper();
        helper.protocol_version = resin_bridge_protocol_version() + 1;
        const IdentityCheck check = check_helper_identity(helper, pinned_expectation());
        CHECK(check.status == BridgeStatus::VersionMismatch);
        CHECK_THAT(check.message, Catch::Matchers::ContainsSubstring("protocol version"));
    }

    SECTION("a different source revision is a mismatch unless a dev override is allowed")
    {
        HelperIdentity helper  = pinned_helper();
        helper.source_revision = "deadbeef";
        helper.development_override = true;

        // Not allowed: rejected.
        CHECK(check_helper_identity(helper, pinned_expectation()).status ==
              BridgeStatus::VersionMismatch);

        // Allowed: accepted, but marked as a development override so a dev
        // build is never mistaken for production.
        PinnedExpectation lenient      = pinned_expectation();
        lenient.allow_development_override = true;
        const IdentityCheck check = check_helper_identity(helper, lenient);
        CHECK(check.ok());
        CHECK(check.accepted_development_override);
        CHECK_THAT(check.message, Catch::Matchers::ContainsSubstring("non-production"));
    }

    SECTION("a non-override helper from the wrong revision is rejected even when overrides allowed")
    {
        HelperIdentity helper       = pinned_helper();
        helper.source_revision      = "deadbeef";
        helper.development_override  = false; // claims to be a pinned build, but is not
        PinnedExpectation lenient       = pinned_expectation();
        lenient.allow_development_override = true;
        CHECK(check_helper_identity(helper, lenient).status == BridgeStatus::VersionMismatch);
    }
}

TEST_CASE("UV1: capability negotiation never assumes an absent capability", "[resin-bridge]")
{
    const HelperIdentity helper = pinned_helper();

    SECTION("required capabilities the helper has are agreed")
    {
        const Negotiation negotiation =
            negotiate(helper, { BridgeCapability::WriteGoo, BridgeCapability::ReadCtb }, {});
        CHECK(negotiation.usable());
        CHECK(negotiation.missing_required.empty());
        CHECK(negotiation.agreed.size() == 2);
    }

    SECTION("a required capability the helper lacks makes the exchange unusable, and is named")
    {
        // The helper has no NanoDLP write.
        const Negotiation negotiation =
            negotiate(helper, { BridgeCapability::WriteNanoDlp }, {});
        CHECK_FALSE(negotiation.usable());
        CHECK(negotiation.missing_required ==
              std::vector<BridgeCapability> { BridgeCapability::WriteNanoDlp });
    }

    SECTION("an absent optional capability is noted for explicit degradation, not fatal")
    {
        const Negotiation negotiation =
            negotiate(helper, { BridgeCapability::WriteGoo }, { BridgeCapability::Repair });
        CHECK(negotiation.usable());
        CHECK(negotiation.missing_optional ==
              std::vector<BridgeCapability> { BridgeCapability::Repair });
    }

    SECTION("the same inputs negotiate the same way every time")
    {
        const Negotiation a =
            negotiate(helper, { BridgeCapability::WriteGoo, BridgeCapability::ReadCtb },
                      { BridgeCapability::Repair });
        const Negotiation b =
            negotiate(helper, { BridgeCapability::WriteGoo, BridgeCapability::ReadCtb },
                      { BridgeCapability::Repair });
        CHECK(a.agreed == b.agreed);
        CHECK(a.missing_required == b.missing_required);
        CHECK(a.missing_optional == b.missing_optional);
    }
}

TEST_CASE("UV1: a request is validated before it is sent", "[resin-bridge]")
{
    SECTION("a well-formed convert request is ok")
    {
        CHECK(validate_request(convert_request()) == BridgeStatus::Ok);
    }

    SECTION("a wrong protocol version is a protocol error, not sent")
    {
        BridgeRequest request   = convert_request();
        request.protocol_version = 999;
        CHECK(validate_request(request) == BridgeStatus::ProtocolError);
    }

    SECTION("a request with no id cannot be correlated")
    {
        BridgeRequest request = convert_request();
        request.request_id.clear();
        CHECK(validate_request(request) == BridgeStatus::InvalidArgument);
    }

    SECTION("an operation that needs a job is refused without one")
    {
        BridgeRequest request = convert_request();
        request.job.reset();
        CHECK(validate_request(request) == BridgeStatus::InvalidArgument);
    }

    SECTION("a convert with no destination format is refused")
    {
        BridgeRequest request      = convert_request();
        request.destination_format.clear();
        CHECK(validate_request(request) == BridgeStatus::InvalidArgument);
    }

    SECTION("probe needs no job")
    {
        BridgeRequest probe;
        probe.protocol_version = resin_bridge_protocol_version();
        probe.request_id       = "p1";
        probe.operation        = BridgeOperation::Probe;
        CHECK(validate_request(probe) == BridgeStatus::Ok);
    }
}

TEST_CASE("UV1: a response has to answer the request it claims to", "[resin-bridge]")
{
    const BridgeRequest request = convert_request();

    SECTION("a matching response is accepted")
    {
        CHECK(match_response(request, good_response("req-1")) == BridgeStatus::Ok);
    }

    SECTION("a response for another request is a protocol error")
    {
        CHECK(match_response(request, good_response("req-2")) == BridgeStatus::ProtocolError);
    }

    SECTION("a response from another protocol version is a protocol error")
    {
        BridgeResponse response   = good_response("req-1");
        response.protocol_version = 999;
        CHECK(match_response(request, response) == BridgeStatus::ProtocolError);
    }
}

TEST_CASE("UV1: progress is monotonic and belongs to its exchange", "[resin-bridge]")
{
    SECTION("ascending progress for the right request is accepted")
    {
        const ProgressCheck check = check_progress(
            "req-1", { progress("req-1", 0.), progress("req-1", 0.5), progress("req-1", 1.) });
        CHECK(check.status == BridgeStatus::Ok);
        CHECK(check.final_fraction == 1.);
        CHECK(check.accepted == 3);
    }

    SECTION("progress that goes backwards is a protocol error")
    {
        const ProgressCheck check =
            check_progress("req-1", { progress("req-1", 0.6), progress("req-1", 0.4) });
        CHECK(check.status == BridgeStatus::ProtocolError);
        CHECK_THAT(check.message, Catch::Matchers::ContainsSubstring("backwards"));
    }

    SECTION("progress for another request during this exchange is rejected")
    {
        const ProgressCheck check =
            check_progress("req-1", { progress("req-1", 0.3), progress("req-2", 0.4) });
        CHECK(check.status == BridgeStatus::ProtocolError);
    }

    SECTION("a fraction outside 0..1 is rejected")
    {
        CHECK(check_progress("req-1", { progress("req-1", 1.5) }).status ==
              BridgeStatus::ProtocolError);
    }
}

TEST_CASE("UV1: the terminal status is deterministic even when the helper misbehaves",
          "[resin-bridge]")
{
    const BridgeRequest request = convert_request();

    SECTION("a normal response passes through")
    {
        ProcessOutcome outcome;
        outcome.produced_response = true;
        outcome.response          = good_response("req-1");
        const BridgeResponse resolved = resolve_exchange(request, outcome);
        CHECK(resolved.ok());
    }

    SECTION("no response at all is a helper crash, whatever the exit code")
    {
        ProcessOutcome outcome;
        outcome.produced_response      = false;
        outcome.process_exited_nonzero = true;
        CHECK(resolve_exchange(request, outcome).status == BridgeStatus::HelperCrash);

        outcome.process_exited_nonzero = false;
        CHECK(resolve_exchange(request, outcome).status == BridgeStatus::HelperCrash);
    }

    SECTION("a response produced past the deadline is a timeout, not a late success")
    {
        ProcessOutcome outcome;
        outcome.produced_response = true;
        outcome.exceeded_deadline = true;
        outcome.response          = good_response("req-1"); // it did answer, but too late
        CHECK(resolve_exchange(request, outcome).status == BridgeStatus::Timeout);
    }

    SECTION("a response for the wrong request is a protocol error, not accepted")
    {
        ProcessOutcome outcome;
        outcome.produced_response = true;
        outcome.response          = good_response("req-999");
        CHECK(resolve_exchange(request, outcome).status == BridgeStatus::ProtocolError);
    }

    SECTION("an engine error the helper reported is passed through faithfully")
    {
        BridgeResponse failed = good_response("req-1");
        failed.status         = BridgeStatus::EngineError;
        failed.message        = "the file could not be re-encoded";
        ProcessOutcome outcome;
        outcome.produced_response = true;
        outcome.response          = failed;
        const BridgeResponse resolved = resolve_exchange(request, outcome);
        CHECK(resolved.status == BridgeStatus::EngineError);
        CHECK_THAT(resolved.message, Catch::Matchers::ContainsSubstring("re-encoded"));
    }
}
