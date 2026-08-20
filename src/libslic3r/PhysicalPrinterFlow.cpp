///|/ TigerSlicer Phase N4/N5/N7: physical-printer connect flow, lifecycle,
///|/ and discovery-tab state.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PhysicalPrinterFlow.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r {

const char *connection_technology_name(ConnectionTechnology technology)
{
    switch (technology) {
    case ConnectionTechnology::FDM:     return "FDM";
    case ConnectionTechnology::Resin:   return "resin";
    case ConnectionTechnology::Unknown: return "unknown";
    }
    return "unknown";
}

const char *credential_kind_name(CredentialKind kind)
{
    switch (kind) {
    case CredentialKind::None:             return "none";
    case CredentialKind::ApiKey:           return "api key";
    case CredentialKind::UsernamePassword: return "username and password";
    case CredentialKind::Certificate:      return "certificate";
    case CredentialKind::PairingAction:    return "pairing action";
    }
    return "none";
}

const char *connect_step_name(ConnectStep step)
{
    switch (step) {
    case ConnectStep::RevalidateReachability:   return "revalidate reachability";
    case ConnectStep::SelectProvider:           return "select provider";
    case ConnectStep::RequestCredentials:       return "request credentials";
    case ConnectStep::TestConnection:           return "test connection";
    case ConnectStep::MatchProfile:             return "match profile";
    case ConnectStep::AssociateSlicingPreset:   return "associate slicing preset";
    case ConnectStep::ReviewCapabilityMismatches: return "review capability mismatches";
    case ConnectStep::ReturnToConnectedTab:     return "return to connected tab";
    }
    return "select provider";
}

const char *address_verdict_name(AddressVerdict verdict)
{
    switch (verdict) {
    case AddressVerdict::Valid:        return "valid";
    case AddressVerdict::Empty:        return "empty";
    case AddressVerdict::Ambiguous:    return "ambiguous";
    case AddressVerdict::Malformed:    return "malformed";
    case AddressVerdict::UnsafeScheme: return "unsafe scheme";
    }
    return "empty";
}

const char *flow_outcome_name(FlowOutcome outcome)
{
    switch (outcome) {
    case FlowOutcome::ReadyToConnect:       return "ready to connect";
    case FlowOutcome::NoProvider:           return "no provider";
    case FlowOutcome::BadAddress:           return "bad address";
    case FlowOutcome::NeedsCredentials:     return "needs credentials";
    case FlowOutcome::NeedsProfileChoice:   return "needs profile choice";
    case FlowOutcome::NeedsMismatchReview:  return "needs mismatch review";
    case FlowOutcome::NoCompatibleProfile:  return "no compatible profile";
    }
    return "no provider";
}

const char *connection_state_name(ConnectionState state)
{
    switch (state) {
    case ConnectionState::Connected:              return "connected";
    case ConnectionState::ConfiguredOffline:      return "configured offline";
    case ConnectionState::AuthenticationRequired: return "authentication required";
    case ConnectionState::Disconnected:           return "disconnected";
    case ConnectionState::NotConfigured:          return "not configured";
    }
    return "not configured";
}

const char *connection_action_name(ConnectionAction action)
{
    switch (action) {
    case ConnectionAction::Connect:         return "connect";
    case ConnectionAction::Disconnect:      return "disconnect";
    case ConnectionAction::Forget:          return "forget";
    case ConnectionAction::EditCredentials: return "edit credentials";
    case ConnectionAction::TestConnection:  return "test connection";
    case ConnectionAction::Rediscover:      return "rediscover";
    }
    return "connect";
}

const char *discovery_tab_state_name(DiscoveryTabState state)
{
    switch (state) {
    case DiscoveryTabState::Discovering:            return "discovering";
    case DiscoveryTabState::NoPrintersFound:        return "no printers found";
    case DiscoveryTabState::Results:                return "results";
    case DiscoveryTabState::PermissionDenied:       return "permission denied";
    case DiscoveryTabState::NetworkOffline:         return "network offline";
    case DiscoveryTabState::AuthenticationRequired: return "authentication required";
    case DiscoveryTabState::Unsupported:            return "unsupported";
    }
    return "unsupported";
}

// --- address validation -----------------------------------------------------

namespace {

std::string trim(const std::string &text)
{
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

} // namespace

AddressCheck check_manual_address(const ManualAddress &address)
{
    AddressCheck check;
    const std::string raw = trim(address.raw);

    if (raw.empty()) {
        check.verdict = AddressVerdict::Empty;
        check.message = "no address was entered";
        return check;
    }

    // A scheme without an authority - "javascript:...", "file:...", "data:..."
    // - has a colon but no "//". Told apart from a bare "host:port" by what
    // follows the colon: an opaque scheme body is not a plain port number.
    {
        const size_t colon = raw.find(':');
        if (colon != std::string::npos && raw.compare(colon, 3, "://") != 0) {
            std::string maybe_scheme = raw.substr(0, colon);
            const bool  scheme_shaped =
                ! maybe_scheme.empty() &&
                maybe_scheme.find('.') == std::string::npos &&
                std::all_of(maybe_scheme.begin(), maybe_scheme.end(),
                            [](unsigned char c) { return std::isalpha(c); });
            const std::string after = raw.substr(colon + 1);
            const bool looks_like_port =
                ! after.empty() &&
                std::all_of(after.begin(), after.end(),
                            [](unsigned char c) { return std::isdigit(c); });
            if (scheme_shaped && ! looks_like_port) {
                check.verdict = AddressVerdict::UnsafeScheme;
                check.message = "\"" + maybe_scheme +
                                "\" is not a printer endpoint scheme; only http and https are "
                                "used";
                return check;
            }
        }
    }

    std::string rest = raw;
    std::string scheme;
    const size_t scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        scheme = rest.substr(0, scheme_end);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        rest = rest.substr(scheme_end + 3);
        if (scheme != "http" && scheme != "https") {
            check.verdict = AddressVerdict::UnsafeScheme;
            check.message = "\"" + scheme +
                            "\" is not a printer endpoint scheme; only http and https are used";
            return check;
        }
    }

    // Credentials in the authority (user:pass@host) make the host ambiguous
    // and would smuggle a secret into a URL; refuse rather than parse past it.
    if (rest.find('@') != std::string::npos) {
        check.verdict = AddressVerdict::Ambiguous;
        check.message = "an address with credentials in it is not accepted; supply the host "
                        "alone and enter credentials separately";
        return check;
    }

    // A path, query or fragment on what should be a host:port is ambiguous -
    // it is not clear the user meant an endpoint rather than a page.
    const size_t path = rest.find_first_of("/?#");
    std::string authority = path == std::string::npos ? rest : rest.substr(0, path);
    const std::string remainder = path == std::string::npos ? std::string() : rest.substr(path);
    if (! remainder.empty() && remainder != "/") {
        check.verdict = AddressVerdict::Ambiguous;
        check.message = "the address carries a path; enter only the printer's host and optional "
                        "port";
        return check;
    }

    if (authority.empty()) {
        check.verdict = AddressVerdict::Malformed;
        check.message = "no host in the address";
        return check;
    }

    // Split host:port. A bracketed IPv6 literal is handled so its colons are
    // not read as a port separator.
    std::string host;
    std::string port_text;
    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) {
            check.verdict = AddressVerdict::Malformed;
            check.message = "an unclosed IPv6 literal";
            return check;
        }
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                check.verdict = AddressVerdict::Malformed;
                check.message = "unexpected text after the IPv6 literal";
                return check;
            }
            port_text = authority.substr(close + 2);
        }
    } else {
        const size_t colon = authority.find(':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            // A second colon means this is neither a bracketed v6 literal nor a
            // plain host:port, so it cannot be pinned to one endpoint.
            if (authority.find(':', colon + 1) != std::string::npos) {
                check.verdict = AddressVerdict::Ambiguous;
                check.message = "the address has more than one colon and no brackets; wrap an "
                                "IPv6 address in [ ]";
                return check;
            }
            host      = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        }
    }

    if (host.empty()) {
        check.verdict = AddressVerdict::Malformed;
        check.message = "no host before the port";
        return check;
    }

    if (! port_text.empty()) {
        if (! std::all_of(port_text.begin(), port_text.end(),
                          [](unsigned char c) { return std::isdigit(c); })) {
            check.verdict = AddressVerdict::Malformed;
            check.message = "the port is not a number";
            return check;
        }
        // std::stol throws std::out_of_range for all-digit strings that overflow
        // long (on MSVC long is 32-bit, so e.g. "host:9999999999"). That would
        // propagate out and terminate the app, so treat it as a Malformed port
        // rather than letting the exception escape.
        long port = 0;
        try {
            port = std::stol(port_text);
        } catch (const std::exception &) {
            check.verdict = AddressVerdict::Malformed;
            check.message = "the port is out of range";
            return check;
        }
        if (port < 1 || port > 65535) {
            check.verdict = AddressVerdict::Malformed;
            check.message = "the port is out of range";
            return check;
        }
        check.port = int(port);
    }

    check.verdict         = AddressVerdict::Valid;
    check.normalized_host = host;
    check.message         = "usable";
    return check;
}

// --- connect flow -----------------------------------------------------------

namespace {

// Pick the provider: an explicit choice wins; otherwise the unique one whose
// technology and advertisement match the device. More than one candidate is
// not resolved here - it is left for the user.
std::optional<ProviderDescriptor> select_provider(const ConnectRequest &request,
                                                  bool                 &ambiguous)
{
    ambiguous = false;
    if (request.chosen_provider.has_value()) {
        for (const ProviderDescriptor &provider : request.known_providers)
            if (provider.id == *request.chosen_provider)
                return provider;
        return std::nullopt;
    }

    const bool manual = request.manual_address.has_value();
    std::vector<ProviderDescriptor> candidates;
    for (const ProviderDescriptor &provider : request.known_providers) {
        // A manual setup needs a provider that can be reached without an
        // advertisement.
        if (manual && provider.self_advertises)
            continue;
        candidates.push_back(provider);
    }
    if (candidates.size() == 1)
        return candidates.front();
    if (candidates.size() > 1)
        ambiguous = true;
    return std::nullopt;
}

} // namespace

ConnectPlan plan_connect(const ConnectRequest &request)
{
    ConnectPlan plan;

    // Manual address, when present, is validated before anything is selected:
    // there is no point choosing a provider for an address that cannot be
    // reached.
    if (request.manual_address.has_value()) {
        const AddressCheck check = check_manual_address(*request.manual_address);
        if (! check.usable()) {
            plan.outcome = FlowOutcome::BadAddress;
            plan.message = check.message;
            return plan;
        }
    }

    bool ambiguous_provider = false;
    const std::optional<ProviderDescriptor> provider =
        select_provider(request, ambiguous_provider);
    if (! provider.has_value()) {
        plan.outcome = FlowOutcome::NoProvider;
        plan.message = ambiguous_provider
                           ? "more than one provider could drive this device; the user has to "
                             "choose one"
                           : "no known provider matches this device";
        return plan;
    }
    plan.provider              = provider;
    plan.required_credentials  = provider->credentials;

    // The profile match is reported, never applied.
    plan.match = match_printer_profile(request.device, request.profiles);

    // Connecting must not silently replace the active slicing preset. Whether
    // saving would change it is surfaced, not acted on.
    if (plan.match.level != ProfileMatchLevel::NoCompatibleProfile &&
        ! plan.match.best_profile.empty() &&
        plan.match.best_profile != request.active_slicing_preset &&
        ! request.active_slicing_preset.empty()) {
        plan.would_change_active_preset = true;
        plan.preset_change_note =
            "this device best matches \"" + plan.match.best_profile +
            "\"; the active slicing preset \"" + request.active_slicing_preset +
            "\" is left unchanged unless the user asks to switch";
    }

    for (const MatchEvidence &evidence : plan.match.evidence)
        if (! evidence.agrees)
            plan.mismatch_warnings.push_back(
                evidence.field + ": device \"" + evidence.device_value + "\" vs profile \"" +
                evidence.profile_value + "\"" + (evidence.blocking ? " (blocking)" : ""));

    // Decide where the flow currently stands, and what still has to happen.
    auto remaining_from = [&plan](ConnectStep first) {
        static const ConnectStep order[] = {
            ConnectStep::RevalidateReachability, ConnectStep::SelectProvider,
            ConnectStep::RequestCredentials,     ConnectStep::TestConnection,
            ConnectStep::MatchProfile,           ConnectStep::AssociateSlicingPreset,
            ConnectStep::ReviewCapabilityMismatches, ConnectStep::ReturnToConnectedTab,
        };
        bool reached = false;
        for (ConnectStep step : order) {
            if (step == first)
                reached = true;
            if (reached)
                plan.remaining_steps.push_back(step);
        }
    };

    if (plan.match.level == ProfileMatchLevel::NoCompatibleProfile) {
        plan.outcome = FlowOutcome::NoCompatibleProfile;
        plan.message = "no installed profile can drive this device";
        return plan;
    }
    if (plan.match.level == ProfileMatchLevel::Ambiguous) {
        plan.outcome = FlowOutcome::NeedsProfileChoice;
        plan.message = "several profiles fit equally well; the user has to choose";
        remaining_from(ConnectStep::MatchProfile);
        return plan;
    }
    if (provider->credentials != CredentialKind::None) {
        plan.outcome = FlowOutcome::NeedsCredentials;
        plan.message = std::string("the ") + provider->display_name + " provider needs " +
                       credential_kind_name(provider->credentials) +
                       " before the connection can be tested";
        remaining_from(ConnectStep::RequestCredentials);
        return plan;
    }
    if (plan.match.level == ProfileMatchLevel::CompatibleWithWarnings) {
        plan.outcome = FlowOutcome::NeedsMismatchReview;
        plan.message = "the profile is usable but has capability mismatches to review before "
                       "saving";
        remaining_from(ConnectStep::ReviewCapabilityMismatches);
        return plan;
    }

    plan.outcome = FlowOutcome::ReadyToConnect;
    plan.message = "the device revalidated, a provider was selected, and an exact profile match "
                   "was found";
    remaining_from(ConnectStep::ReturnToConnectedTab);
    return plan;
}

// --- lifecycle --------------------------------------------------------------

bool control_allowed(ConnectionTechnology connection, ConnectionTechnology control_technology,
                     std::string *reason)
{
    if (control_technology == ConnectionTechnology::Unknown)
        return true; // a control not specific to either technology is always fine
    if (connection == ConnectionTechnology::Unknown) {
        if (reason)
            *reason = "the connection's technology is unknown, so a " +
                      std::string(connection_technology_name(control_technology)) +
                      "-specific control is withheld rather than guessed";
        return false;
    }
    if (connection != control_technology) {
        if (reason)
            *reason = std::string(connection_technology_name(control_technology)) +
                      "-only controls are not shown on a " +
                      connection_technology_name(connection) + " connection";
        return false;
    }
    return true;
}

ConnectionView view_connection(const ConnectionStatus &status)
{
    ConnectionView view;
    view.state            = status.state;
    view.technology       = status.technology;
    view.physical_profile = status.physical_profile;
    view.slicing_preset   = status.slicing_preset;

    switch (status.state) {
    case ConnectionState::NotConfigured:
        view.actions = { ConnectionAction::Connect };
        view.message = "no connection is configured";
        return view;

    case ConnectionState::Disconnected:
        // The profile is kept; the live session is closed. Connect reopens it,
        // Forget deletes it. Nothing controls the printer.
        view.actions = { ConnectionAction::Connect, ConnectionAction::EditCredentials,
                         ConnectionAction::Forget };
        view.message = "configured but not connected; the saved profile is kept until forgotten";
        return view;

    case ConnectionState::ConfiguredOffline:
        // Still visible, still has diagnostics; never dropped just because it
        // is unreachable this moment.
        view.actions = { ConnectionAction::TestConnection, ConnectionAction::Rediscover,
                         ConnectionAction::EditCredentials, ConnectionAction::Forget };
        view.message = "configured but offline; diagnostic actions remain available";
        return view;

    case ConnectionState::AuthenticationRequired:
        view.actions = { ConnectionAction::EditCredentials, ConnectionAction::TestConnection,
                         ConnectionAction::Forget };
        view.message = "reachable, but the stored credentials were rejected";
        return view;

    case ConnectionState::Connected:
        view.upload_available  = status.supports_upload && status.session_open;
        view.control_available = status.supports_control && status.session_open;
        view.actions           = { ConnectionAction::Disconnect, ConnectionAction::TestConnection,
                         ConnectionAction::EditCredentials, ConnectionAction::Forget };
        view.message = "connected";
        return view;
    }
    return view;
}

// --- discovery-tab state ----------------------------------------------------

DiscoveryTabState resolve_discovery_state(const DiscoveryConditions &conditions)
{
    // Most-blocking first, so a tab that could not scan never reads as "none
    // found". Each condition is a distinct reason the user needs to act on
    // differently.
    if (! conditions.discovery_supported)
        return DiscoveryTabState::Unsupported;
    if (! conditions.network_online)
        return DiscoveryTabState::NetworkOffline;
    if (! conditions.permission_granted)
        return DiscoveryTabState::PermissionDenied;
    if (conditions.authentication_required)
        return DiscoveryTabState::AuthenticationRequired;
    if (conditions.results > 0)
        return DiscoveryTabState::Results;
    if (conditions.scanning)
        return DiscoveryTabState::Discovering;
    return DiscoveryTabState::NoPrintersFound;
}

} // namespace Slic3r
