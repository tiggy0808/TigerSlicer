///|/ TigerSlicer Phase N4/N5/N7: physical-printer connect flow, lifecycle,
///|/ and discovery-tab state.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/PhysicalPrinterFlow.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

ProviderDescriptor provider(const std::string &id, CredentialKind credentials,
                            bool self_advertises = true)
{
    ProviderDescriptor value;
    value.id              = id;
    value.display_name    = id;
    value.credentials     = credentials;
    value.technology      = ConnectionTechnology::FDM;
    value.self_advertises = self_advertises;
    return value;
}

PrinterDeviceCapabilities device(const std::string &vendor, const std::string &model,
                                 int tools)
{
    PrinterDeviceCapabilities value;
    value.vendor           = vendor;
    value.model            = model;
    value.technology_known = true;
    value.technology       = ptFFF;
    value.tool_count       = tools;
    value.bed_width_mm     = 250.;
    value.bed_depth_mm     = 210.;
    value.max_height_mm    = 220.;
    value.output_format    = "gcode";
    return value;
}

PrinterProfileSummary profile(const std::string &name, const std::string &vendor,
                              const std::string &model, int tools)
{
    PrinterProfileSummary value;
    value.name          = name;
    value.vendor        = vendor;
    value.model         = model;
    value.technology    = ptFFF;
    value.tool_count    = tools;
    value.bed_width_mm  = 250.;
    value.bed_depth_mm  = 210.;
    value.max_height_mm = 220.;
    value.output_formats = { "gcode" };
    return value;
}

bool offers(const ConnectionView &view, ConnectionAction action)
{
    return std::find(view.actions.begin(), view.actions.end(), action) != view.actions.end();
}

} // namespace

TEST_CASE("N7: the discovery tab shows exactly one state, most-blocking first",
          "[physical-flow]")
{
    DiscoveryConditions conditions;

    SECTION("actively scanning with nothing yet is discovering")
    {
        conditions.scanning = true;
        conditions.results  = 0;
        CHECK(resolve_discovery_state(conditions) == DiscoveryTabState::Discovering);
    }

    SECTION("results outrank the scanning animation")
    {
        conditions.scanning = true;
        conditions.results  = 3;
        CHECK(resolve_discovery_state(conditions) == DiscoveryTabState::Results);
    }

    SECTION("done scanning with nothing found is no-printers-found, not discovering")
    {
        conditions.scanning = false;
        conditions.results  = 0;
        CHECK(resolve_discovery_state(conditions) == DiscoveryTabState::NoPrintersFound);
    }

    SECTION("a blocked firewall is not a no-printers-found tab")
    {
        conditions.permission_granted = false;
        conditions.results            = 0;
        CHECK(resolve_discovery_state(conditions) == DiscoveryTabState::PermissionDenied);
    }

    SECTION("offline outranks permission and results")
    {
        conditions.network_online     = false;
        conditions.permission_granted = false;
        conditions.results            = 5;
        CHECK(resolve_discovery_state(conditions) == DiscoveryTabState::NetworkOffline);
    }

    SECTION("a build without discovery says unsupported before anything else")
    {
        conditions.discovery_supported = false;
        conditions.network_online      = false;
        CHECK(resolve_discovery_state(conditions) == DiscoveryTabState::Unsupported);
    }

    SECTION("authentication-required sits between permission and a bare scan")
    {
        conditions.authentication_required = true;
        conditions.results                 = 2;
        CHECK(resolve_discovery_state(conditions) ==
              DiscoveryTabState::AuthenticationRequired);
    }
}

TEST_CASE("N4: a manual address is validated before anything is reached", "[physical-flow]")
{
    SECTION("a plain host and port is usable")
    {
        const AddressCheck check = check_manual_address({ "192.168.1.50:80" });
        CHECK(check.usable());
        CHECK(check.normalized_host == "192.168.1.50");
        CHECK(check.port.value() == 80);
    }

    SECTION("an http host normalizes and keeps no path")
    {
        const AddressCheck check = check_manual_address({ "http://printer.local" });
        CHECK(check.usable());
        CHECK(check.normalized_host == "printer.local");
        CHECK_FALSE(check.port.has_value());
    }

    SECTION("a bracketed IPv6 literal with a port is not read as ambiguous")
    {
        const AddressCheck check = check_manual_address({ "[fe80::1]:8080" });
        CHECK(check.usable());
        CHECK(check.normalized_host == "[fe80::1]");
        CHECK(check.port.value() == 8080);
    }

    SECTION("a bare IPv6 address with no brackets is ambiguous, not guessed")
    {
        const AddressCheck check = check_manual_address({ "fe80::1:8080" });
        CHECK(check.verdict == AddressVerdict::Ambiguous);
    }

    SECTION("credentials in the authority are refused, not parsed past")
    {
        const AddressCheck check = check_manual_address({ "http://user:pass@printer.local" });
        CHECK(check.verdict == AddressVerdict::Ambiguous);
        CHECK_THAT(check.message, Catch::Matchers::ContainsSubstring("credentials"));
    }

    SECTION("a path makes the address ambiguous")
    {
        const AddressCheck check = check_manual_address({ "printer.local/print?go=1" });
        CHECK(check.verdict == AddressVerdict::Ambiguous);
    }

    SECTION("a non-printer scheme is refused")
    {
        CHECK(check_manual_address({ "file:///etc/passwd" }).verdict ==
              AddressVerdict::UnsafeScheme);
        CHECK(check_manual_address({ "javascript:alert(1)" }).verdict ==
              AddressVerdict::UnsafeScheme);
    }

    SECTION("an out-of-range or non-numeric port is malformed")
    {
        CHECK(check_manual_address({ "printer.local:70000" }).verdict ==
              AddressVerdict::Malformed);
        CHECK(check_manual_address({ "printer.local:abc" }).verdict ==
              AddressVerdict::Malformed);
    }

    SECTION("an empty address is empty, distinctly from malformed")
    {
        CHECK(check_manual_address({ "   " }).verdict == AddressVerdict::Empty);
    }
}

TEST_CASE("N4: connecting is a guided flow that sends nothing and replaces no preset",
          "[physical-flow]")
{
    ConnectRequest request;
    request.device                = device("Prusa", "MK4", 1);
    request.known_providers       = { provider("prusalink", CredentialKind::ApiKey) };
    request.chosen_provider       = std::string("prusalink");
    request.profiles              = { profile("Prusa MK4", "Prusa", "MK4", 1) };
    request.active_slicing_preset = "Prusa MK4";

    SECTION("an exact match with a credential still needs the credential first")
    {
        const ConnectPlan plan = plan_connect(request);
        CHECK(plan.outcome == FlowOutcome::NeedsCredentials);
        CHECK(plan.required_credentials == CredentialKind::ApiKey);
        // The flow reports the kind; it never carries the secret.
        CHECK(plan.remaining_steps.front() == ConnectStep::RequestCredentials);
    }

    SECTION("with no credential needed and an exact match, it is ready")
    {
        request.known_providers = { provider("serial", CredentialKind::None) };
        request.chosen_provider = std::string("serial");
        const ConnectPlan plan  = plan_connect(request);
        CHECK(plan.ready());
        CHECK(plan.remaining_steps.back() == ConnectStep::ReturnToConnectedTab);
        CHECK_FALSE(plan.would_change_active_preset);
    }

    SECTION("a device matching a different profile does not silently switch the active preset")
    {
        request.known_providers       = { provider("serial", CredentialKind::None) };
        request.chosen_provider       = std::string("serial");
        request.device                = device("Prusa", "MINI", 1);
        request.profiles              = { profile("Prusa MINI", "Prusa", "MINI", 1) };
        request.active_slicing_preset = "Prusa MK4";
        const ConnectPlan plan        = plan_connect(request);
        CHECK(plan.would_change_active_preset);
        CHECK_THAT(plan.preset_change_note,
                   Catch::Matchers::ContainsSubstring("left unchanged"));
        // It is still ready to connect: the preset choice is surfaced, not
        // forced, and does not block the connection.
        CHECK(plan.ready());
    }

    SECTION("no compatible profile is reported, and the flow does not proceed")
    {
        request.known_providers = { provider("serial", CredentialKind::None) };
        request.chosen_provider = std::string("serial");
        request.profiles        = { profile("Formlabs", "Formlabs", "Form3", 1) };
        request.profiles[0].technology = ptSLA;
        const ConnectPlan plan  = plan_connect(request);
        CHECK(plan.outcome == FlowOutcome::NoCompatibleProfile);
        CHECK(plan.remaining_steps.empty());
    }

    SECTION("an ambiguous match asks the user to choose rather than picking one")
    {
        request.known_providers = { provider("serial", CredentialKind::None) };
        request.chosen_provider = std::string("serial");
        request.profiles        = { profile("MK4 A", "Prusa", "MK4", 1),
                                    profile("MK4 B", "Prusa", "MK4", 1) };
        const ConnectPlan plan  = plan_connect(request);
        CHECK(plan.outcome == FlowOutcome::NeedsProfileChoice);
        CHECK(plan.remaining_steps.front() == ConnectStep::MatchProfile);
    }

    SECTION("a manual setup needs a provider that does not rely on advertisement")
    {
        request.chosen_provider.reset();
        request.manual_address  = ManualAddress { "printer.local:80" };
        request.known_providers = { provider("mdns", CredentialKind::None, true),
                                    provider("manual-http", CredentialKind::None, false) };
        const ConnectPlan plan  = plan_connect(request);
        // The self-advertising provider is not eligible for a typed address;
        // the manual one is selected without the user naming it.
        REQUIRE(plan.provider.has_value());
        CHECK(plan.provider->id == "manual-http");
    }

    SECTION("a bad manual address stops the flow before a provider is chosen")
    {
        request.chosen_provider.reset();
        request.manual_address  = ManualAddress { "http://user:pass@printer.local" };
        request.known_providers = { provider("manual-http", CredentialKind::None, false) };
        const ConnectPlan plan  = plan_connect(request);
        CHECK(plan.outcome == FlowOutcome::BadAddress);
        CHECK_FALSE(plan.provider.has_value());
    }
}

TEST_CASE("N5: disconnect and forget are different, and nothing controls a closed session",
          "[physical-flow]")
{
    ConnectionStatus status;
    status.technology       = ConnectionTechnology::FDM;
    status.physical_profile = "MK4 on the bench";
    status.slicing_preset   = "Prusa MK4";
    status.supports_upload  = true;
    status.supports_control = true;

    SECTION("a connected printer can be disconnected but not connected again")
    {
        status.state        = ConnectionState::Connected;
        status.reachable    = true;
        status.session_open = true;
        const ConnectionView view = view_connection(status);
        CHECK(offers(view, ConnectionAction::Disconnect));
        CHECK_FALSE(offers(view, ConnectionAction::Connect));
        CHECK(offers(view, ConnectionAction::Forget));
        CHECK(view.upload_available);
        CHECK(view.control_available);
    }

    SECTION("a disconnected printer keeps its profile and offers connect, not disconnect")
    {
        status.state        = ConnectionState::Disconnected;
        status.session_open = false;
        const ConnectionView view = view_connection(status);
        CHECK(offers(view, ConnectionAction::Connect));
        CHECK_FALSE(offers(view, ConnectionAction::Disconnect));
        // Forget is a separate, explicit action - the profile is not deleted
        // by disconnecting.
        CHECK(offers(view, ConnectionAction::Forget));
        CHECK_FALSE(view.upload_available);
        CHECK_FALSE(view.control_available);
    }

    SECTION("a configured-but-offline printer stays visible with diagnostic actions")
    {
        status.state        = ConnectionState::ConfiguredOffline;
        status.reachable    = false;
        status.session_open = false;
        const ConnectionView view = view_connection(status);
        CHECK(offers(view, ConnectionAction::TestConnection));
        CHECK(offers(view, ConnectionAction::Rediscover));
        CHECK(offers(view, ConnectionAction::Forget));
        CHECK_FALSE(view.control_available);
    }

    SECTION("rejected credentials lead to editing them, not to control")
    {
        status.state = ConnectionState::AuthenticationRequired;
        const ConnectionView view = view_connection(status);
        CHECK(offers(view, ConnectionAction::EditCredentials));
        CHECK_FALSE(offers(view, ConnectionAction::Disconnect));
        CHECK_FALSE(view.control_available);
    }

    SECTION("an unconfigured slot offers only connect")
    {
        status.state = ConnectionState::NotConfigured;
        const ConnectionView view = view_connection(status);
        CHECK(view.actions == std::vector<ConnectionAction> { ConnectionAction::Connect });
    }

    SECTION("a connected printer whose provider cannot upload does not offer upload")
    {
        status.state           = ConnectionState::Connected;
        status.session_open    = true;
        status.supports_upload = false;
        const ConnectionView view = view_connection(status);
        CHECK_FALSE(view.upload_available);
        CHECK(view.control_available);
    }
}

TEST_CASE("N5: a control is only shown when its technology matches the connection",
          "[physical-flow]")
{
    std::string reason;

    SECTION("an FDM control on an FDM connection is allowed")
    {
        CHECK(control_allowed(ConnectionTechnology::FDM, ConnectionTechnology::FDM, &reason));
    }

    SECTION("a resin control on an FDM connection is withheld, and says so")
    {
        CHECK_FALSE(
            control_allowed(ConnectionTechnology::FDM, ConnectionTechnology::Resin, &reason));
        CHECK_THAT(reason, Catch::Matchers::ContainsSubstring("resin-only"));
    }

    SECTION("an FDM control on a resin connection is withheld too")
    {
        CHECK_FALSE(
            control_allowed(ConnectionTechnology::Resin, ConnectionTechnology::FDM, &reason));
    }

    SECTION("a technology-neutral control is always allowed")
    {
        CHECK(control_allowed(ConnectionTechnology::FDM, ConnectionTechnology::Unknown, &reason));
        CHECK(
            control_allowed(ConnectionTechnology::Resin, ConnectionTechnology::Unknown, &reason));
    }

    SECTION("an unknown connection withholds technology-specific controls rather than guessing")
    {
        CHECK_FALSE(
            control_allowed(ConnectionTechnology::Unknown, ConnectionTechnology::FDM, &reason));
        CHECK_THAT(reason, Catch::Matchers::ContainsSubstring("unknown"));
    }
}
