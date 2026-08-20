///|/ TigerSlicer Phase N4/N5/N7: physical-printer connect flow, lifecycle,
///|/ and discovery-tab state. The decision logic only; the wx presentation
///|/ layer supplies inputs and renders what is returned here.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PhysicalPrinterFlow_hpp_
#define slic3r_PhysicalPrinterFlow_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "PrinterCapabilityMatch.hpp"

namespace Slic3r {

// The printing technology a connection is for. A resin connection never shows
// FDM-only controls and an FDM one never shows resin-only controls, so the
// technology has to travel with the connection rather than being inferred at
// the point of drawing a button.
enum class ConnectionTechnology : uint8_t
{
    FDM,
    Resin,
    // The device did not say. Controls specific to either technology are
    // withheld rather than guessed.
    Unknown,
};

const char *connection_technology_name(ConnectionTechnology technology);

// What a provider needs before a connection can be tested. Requesting only
// what the selected provider needs is an N4 requirement: an octoprint key is
// not asked for when connecting to something that authenticates by pairing.
enum class CredentialKind : uint8_t
{
    None,
    ApiKey,
    UsernamePassword,
    Certificate,
    PairingAction,
};

const char *credential_kind_name(CredentialKind kind);

// The steps of the guided connect flow, in order. Selecting Connect does not
// send commands; it enters this flow at the first step.
enum class ConnectStep : uint8_t
{
    RevalidateReachability,
    SelectProvider,
    RequestCredentials,
    TestConnection,
    MatchProfile,
    AssociateSlicingPreset,
    ReviewCapabilityMismatches,
    ReturnToConnectedTab,
};

const char *connect_step_name(ConnectStep step);

struct ProviderDescriptor
{
    std::string          id;
    std::string          display_name;
    CredentialKind       credentials { CredentialKind::None };
    ConnectionTechnology technology { ConnectionTechnology::Unknown };
    // True when the provider advertises itself on the LAN; false ones can only
    // be reached by manual hostname/IP entry.
    bool self_advertises { true };
};

// A manual address the user typed, or one an advertisement carried. Validated
// before use: an ambiguous or malformed address never reaches a connection
// attempt.
struct ManualAddress
{
    std::string raw;
};

enum class AddressVerdict : uint8_t
{
    Valid,
    Empty,
    // Parses to more than one plausible host, or carries credentials in the
    // authority, or otherwise cannot be pinned to one endpoint.
    Ambiguous,
    Malformed,
    // A scheme that is not a local printer endpoint (file:, javascript:, ...).
    UnsafeScheme,
};

const char *address_verdict_name(AddressVerdict verdict);

struct AddressCheck
{
    AddressVerdict verdict { AddressVerdict::Empty };
    std::string    normalized_host;
    std::optional<int> port;
    std::string    message;

    bool usable() const { return verdict == AddressVerdict::Valid; }
};

// Validate and normalize a manually entered printer address. Rejects anything
// that would parse ambiguously rather than picking one interpretation.
AddressCheck check_manual_address(const ManualAddress &address);

// --- the connect flow -------------------------------------------------------

struct ConnectRequest
{
    // The device as discovered or manually described.
    PrinterDeviceCapabilities device;
    // Empty when the device advertised itself; set for manual setup.
    std::optional<ManualAddress> manual_address;

    // The providers TigerSlicer knows about. The flow selects from these; it
    // does not invent one.
    std::vector<ProviderDescriptor> known_providers;
    // The provider id the caller wants, when the user chose one explicitly.
    std::optional<std::string> chosen_provider;

    // The installed profiles, for the match step.
    std::vector<PrinterProfileSummary> profiles;

    // The slicing preset currently active in the main pane. Connecting must
    // not silently replace it.
    std::string active_slicing_preset;
};

enum class FlowOutcome : uint8_t
{
    // Every step resolved; the flow is ready to save the connection.
    ReadyToConnect,
    // A provider could not be chosen from what was supplied.
    NoProvider,
    // The address the user typed cannot be used.
    BadAddress,
    // Credentials the provider needs were not supplied. (The flow reports
    // which; it never stores or echoes the values.)
    NeedsCredentials,
    // The profile match is ambiguous and needs the user to choose.
    NeedsProfileChoice,
    // A capability mismatch has to be reviewed before saving.
    NeedsMismatchReview,
    // Nothing on the machine can drive this device.
    NoCompatibleProfile,
};

const char *flow_outcome_name(FlowOutcome outcome);

struct ConnectPlan
{
    FlowOutcome outcome { FlowOutcome::NoProvider };

    // The provider the flow selected, when it could.
    std::optional<ProviderDescriptor> provider;

    // What the selected provider needs. The flow reports the kind; the caller
    // collects the secret through OS secure storage and never through here.
    CredentialKind required_credentials { CredentialKind::None };

    // The profile match, carried through from Phase N8. Never applied here.
    ProfileMatch match;

    // True when saving this connection would change the active slicing preset.
    // The flow never does this itself; it surfaces the choice.
    bool would_change_active_preset { false };
    std::string preset_change_note;

    // The steps that still have to happen, in order, so the UI can drive the
    // rest of the flow without re-deriving it.
    std::vector<ConnectStep> remaining_steps;

    std::vector<std::string> mismatch_warnings;
    std::string              message;

    bool ready() const { return outcome == FlowOutcome::ReadyToConnect; }
};

// Advance the guided connect flow as far as the supplied inputs allow, and
// report what is still needed. Sends nothing, stores nothing, selects no
// slicing preset.
ConnectPlan plan_connect(const ConnectRequest &request);

// --- connection lifecycle (N5) ---------------------------------------------

enum class ConnectionState : uint8_t
{
    // Configured and reachable.
    Connected,
    // Configured but not reachable right now. Still listed, still has
    // diagnostic actions; not the same as forgotten.
    ConfiguredOffline,
    // Reachable but its credentials are no longer accepted.
    AuthenticationRequired,
    // Configured, and the user has disconnected the live session but kept the
    // profile.
    Disconnected,
    // Not configured at all.
    NotConfigured,
};

const char *connection_state_name(ConnectionState state);

// The actions a connection can offer. Disconnect and Forget are deliberately
// distinct: one ends the session, the other deletes the saved profile.
enum class ConnectionAction : uint8_t
{
    Connect,
    Disconnect,
    Forget,
    EditCredentials,
    TestConnection,
    Rediscover,
};

const char *connection_action_name(ConnectionAction action);

struct ConnectionView
{
    ConnectionState      state { ConnectionState::NotConfigured };
    ConnectionTechnology technology { ConnectionTechnology::Unknown };

    std::string physical_profile;   // the saved connection profile
    std::string slicing_preset;     // the preset associated with it, if any

    // Whether the live features are usable right now.
    bool upload_available { false };
    bool control_available { false };

    // The actions offered, in a stable order.
    std::vector<ConnectionAction> actions;

    // True when a control specific to the other technology was asked for and
    // withheld; the note says which.
    std::vector<std::string> withheld_controls;

    std::string message;
};

struct ConnectionStatus
{
    ConnectionState      state { ConnectionState::NotConfigured };
    ConnectionTechnology technology { ConnectionTechnology::Unknown };
    std::string          physical_profile;
    std::string          slicing_preset;

    // What the transport reports right now.
    bool reachable { false };
    bool credentials_accepted { true };
    bool session_open { false };

    // Whether the provider supports these at all, before asking whether they
    // are usable now.
    bool supports_upload { false };
    bool supports_control { false };
};

// Resolve which actions and features a configured printer offers in its
// current state. Never returns an action that would control a printer the user
// has not asked to control; Connect is the only thing offered on a printer
// whose session is closed.
ConnectionView view_connection(const ConnectionStatus &status);

// Whether a control identified as FDM- or resin-specific may be shown on this
// connection. Unknown technology withholds both.
bool control_allowed(ConnectionTechnology connection, ConnectionTechnology control_technology,
                     std::string *reason);

// --- discovery-tab state (N7) ----------------------------------------------

// The single state the discovery tab is in. Exactly one, chosen from what the
// environment reports, so the tab never shows two contradictory things.
enum class DiscoveryTabState : uint8_t
{
    Discovering,
    NoPrintersFound,
    Results,
    PermissionDenied,   // the OS firewall blocked discovery
    NetworkOffline,
    AuthenticationRequired,
    Unsupported,        // discovery is not available on this build/platform
};

const char *discovery_tab_state_name(DiscoveryTabState state);

struct DiscoveryConditions
{
    bool discovery_supported { true };
    bool network_online { true };
    bool permission_granted { true };
    bool authentication_required { false };

    // Still actively scanning.
    bool scanning { false };
    // How many devices are currently listed.
    size_t results { 0 };
};

// Choose the one state the tab should render. The order of checks is the order
// of severity: a blocked-by-firewall tab is not a no-printers-found tab, and
// saying "none found" when discovery never ran is the failure this resolves.
DiscoveryTabState resolve_discovery_state(const DiscoveryConditions &conditions);

} // namespace Slic3r

#endif // slic3r_PhysicalPrinterFlow_hpp_
