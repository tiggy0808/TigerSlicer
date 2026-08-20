///|/ TigerSlicer Phase N: guided connection diagnostics.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ConnectionDiagnostics_hpp_
#define slic3r_ConnectionDiagnostics_hpp_

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Ordered stages of the diagnostic flow. Order matters: a later stage cannot
// be judged when an earlier one failed, and reporting it as "passed" or even
// "failed" would be misleading.
enum class DiagnosticStage
{
    NetworkInterface,
    LocalNetworkPermission,
    HostnameResolution,
    RouteSelection,
    ServiceDiscovery,
    PortReachable,
    Authentication,
    CertificateIdentity,
    ApiVersion,
    StoragePermission,
};

enum class DiagnosticResult
{
    Pass,
    Warning,
    Fail,
    // Could not be evaluated because an earlier stage failed. Distinct from a
    // failure so the user is not sent chasing a symptom instead of the cause.
    NotChecked,
};

// Raw observations the caller collected by probing. Every field is optional:
// "not observed" must never be scored as success.
struct DiagnosticObservations
{
    std::optional<bool> has_network_interface;
    std::optional<bool> local_network_permitted;
    std::optional<bool> hostname_resolved;
    std::vector<std::string> resolved_addresses;
    std::optional<bool> route_available;
    std::optional<bool> service_advertised;
    std::optional<bool> port_open;
    std::optional<bool> authenticated;
    std::optional<bool> authentication_required;

    // TLS identity: whether a certificate was seen, and whether it differs
    // from the one previously trusted for this printer.
    std::optional<bool> tls_used;
    std::optional<bool> certificate_changed;

    std::optional<std::string> reported_api_version;
    std::string                minimum_api_version;

    std::optional<bool> storage_writable;
    std::optional<bool> timed_out;
};

struct DiagnosticStep
{
    DiagnosticStage  stage { DiagnosticStage::NetworkInterface };
    DiagnosticResult result { DiagnosticResult::NotChecked };
    std::string      summary;    // plain language
    std::string      detail;     // technical, shown on demand
    std::string      next_action;
};

struct DiagnosticReport
{
    std::vector<DiagnosticStep> steps;
    // First stage that failed, if any - the thing worth fixing first.
    std::optional<DiagnosticStage> first_failure;
    bool healthy() const { return !first_failure.has_value(); }
};

// Evaluate observations into an ordered, explainable report.
//
// This function performs no I/O and changes nothing on the machine or the
// network: diagnostics must never "fix" things by weakening TLS, altering
// firewalls, or restarting printers.
DiagnosticReport diagnose_connection(const DiagnosticObservations &observations);

// Remove anything credential-shaped from text destined for logs or a
// diagnostic bundle. Applied to every string leaving the diagnostics layer.
std::string redact_secrets(const std::string &text);

const char *to_string(DiagnosticStage stage);

} // namespace Slic3r

#endif // slic3r_ConnectionDiagnostics_hpp_
