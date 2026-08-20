///|/ TigerSlicer Phase N: guided connection diagnostics.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ConnectionDiagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <stdexcept>

#include <boost/algorithm/string.hpp>

namespace Slic3r {

const char *to_string(DiagnosticStage stage)
{
    switch (stage) {
    case DiagnosticStage::NetworkInterface:       return "Network interface";
    case DiagnosticStage::LocalNetworkPermission: return "Local network permission";
    case DiagnosticStage::HostnameResolution:     return "Hostname resolution";
    case DiagnosticStage::RouteSelection:         return "Route selection";
    case DiagnosticStage::ServiceDiscovery:       return "Service discovery";
    case DiagnosticStage::PortReachable:          return "Port reachability";
    case DiagnosticStage::Authentication:         return "Authentication";
    case DiagnosticStage::CertificateIdentity:    return "Certificate identity";
    case DiagnosticStage::ApiVersion:             return "API version";
    case DiagnosticStage::StoragePermission:      return "Storage permission";
    }
    return "Unknown";
}

std::string redact_secrets(const std::string &text)
{
    static const std::regex patterns[] = {
        // key=value / key: value forms for credential-ish names
        std::regex(R"((?:api[_-]?key|apikey|token|password|passwd|secret|authorization|bearer)\s*[:=]\s*\"?[^\s\"&;]+)",
                   std::regex::icase),
        // credentials embedded in a URL
        std::regex(R"(([a-z][a-z0-9+.-]*://)[^/\s:@]+:[^/\s@]+@)", std::regex::icase),
        // X-Api-Key style headers
        std::regex(R"(X-(?:Api-Key|Auth-Token)\s*:\s*\S+)", std::regex::icase),
    };

    std::string out = text;
    out = std::regex_replace(out, patterns[0], "[redacted]");
    out = std::regex_replace(out, patterns[1], "$1[redacted]@");
    out = std::regex_replace(out, patterns[2], "[redacted]");
    return out;
}

namespace {

DiagnosticStep step(DiagnosticStage stage, DiagnosticResult result,
                    std::string summary, std::string detail = {},
                    std::string next_action = {})
{
    DiagnosticStep item;
    item.stage       = stage;
    item.result      = result;
    item.summary     = redact_secrets(summary);
    item.detail      = redact_secrets(detail);
    item.next_action = redact_secrets(next_action);
    return item;
}

} // namespace

DiagnosticReport diagnose_connection(const DiagnosticObservations &obs)
{
    DiagnosticReport report;
    bool blocked = false;   // an earlier stage failed; later ones are unknown

    auto add = [&report, &blocked](DiagnosticStage stage, DiagnosticResult result,
                                   std::string summary, std::string detail = {},
                                   std::string next_action = {}) {
        if (blocked) {
            report.steps.push_back(step(stage, DiagnosticResult::NotChecked,
                "Not checked because an earlier step failed."));
            return;
        }
        report.steps.push_back(step(stage, result, std::move(summary), std::move(detail),
                                    std::move(next_action)));
        if (result == DiagnosticResult::Fail) {
            if (!report.first_failure.has_value())
                report.first_failure = stage;
            blocked = true;
        }
    };

    // An observation the caller never made cannot be a pass.
    auto tri = [](const std::optional<bool> &value) {
        return !value.has_value() ? DiagnosticResult::Warning
                                  : (*value ? DiagnosticResult::Pass : DiagnosticResult::Fail);
    };

    add(DiagnosticStage::NetworkInterface, tri(obs.has_network_interface),
        obs.has_network_interface.value_or(true)
            ? "A usable network interface is available."
            : "No usable network interface was found.",
        {},
        obs.has_network_interface.value_or(true)
            ? "" : "Check that Wi-Fi or Ethernet is connected.");

    add(DiagnosticStage::LocalNetworkPermission, tri(obs.local_network_permitted),
        obs.local_network_permitted.value_or(true)
            ? "TigerSlicer may communicate on the local network."
            : "The operating system is blocking local network access.",
        {},
        obs.local_network_permitted.value_or(true)
            ? "" : "Allow TigerSlicer to access devices on your local network in system settings.");

    add(DiagnosticStage::HostnameResolution, tri(obs.hostname_resolved),
        obs.hostname_resolved.value_or(true)
            ? "The printer's hostname resolved."
            : "The printer's hostname could not be resolved.",
        obs.resolved_addresses.empty()
            ? std::string()
            : "Resolved to: " + boost::algorithm::join(obs.resolved_addresses, ", "),
        obs.hostname_resolved.value_or(true)
            ? "" : "Try the printer's IP address instead of its hostname.");

    add(DiagnosticStage::RouteSelection, tri(obs.route_available),
        obs.route_available.value_or(true)
            ? "A route to the printer is available."
            : "No route to the printer's address.",
        {},
        obs.route_available.value_or(true)
            ? "" : "Confirm the printer is on the same network segment.");

    // Discovery is informational: a printer that does not advertise itself can
    // still be reached by address, so a miss here must not fail the flow.
    add(DiagnosticStage::ServiceDiscovery,
        !obs.service_advertised.has_value() ? DiagnosticResult::Warning
            : (*obs.service_advertised ? DiagnosticResult::Pass : DiagnosticResult::Warning),
        obs.service_advertised.value_or(false)
            ? "The printer advertises its service on the network."
            : "The printer does not advertise itself; this is normal for some models.",
        {},
        obs.service_advertised.value_or(false) ? "" : "You can still add it manually by address.");

    const bool timed_out = obs.timed_out.value_or(false);
    add(DiagnosticStage::PortReachable,
        timed_out ? DiagnosticResult::Fail : tri(obs.port_open),
        timed_out ? "The connection attempt timed out."
                  : (obs.port_open.value_or(true) ? "The printer's service port accepted a connection."
                                                  : "The printer's service port refused the connection."),
        {},
        (timed_out || !obs.port_open.value_or(true))
            ? "A firewall may be blocking the connection, or the service may not be running."
            : "");

    // Authentication only matters when the device says it needs it.
    const bool auth_needed = obs.authentication_required.value_or(false);
    add(DiagnosticStage::Authentication,
        !auth_needed ? DiagnosticResult::Pass : tri(obs.authenticated),
        !auth_needed ? "This printer does not require authentication."
                     : (obs.authenticated.value_or(false)
                            ? "Authentication succeeded."
                            : "Authentication failed or no credentials are stored."),
        {},
        (auth_needed && !obs.authenticated.value_or(false))
            ? "Enter or update the printer's API key or password." : "");

    // A changed certificate is never auto-trusted; it is surfaced as a
    // decision for the user.
    const bool tls = obs.tls_used.value_or(false);
    add(DiagnosticStage::CertificateIdentity,
        !tls ? DiagnosticResult::Pass
             : (obs.certificate_changed.value_or(false) ? DiagnosticResult::Warning
                                                        : DiagnosticResult::Pass),
        !tls ? "The connection does not use TLS."
             : (obs.certificate_changed.value_or(false)
                    ? "The printer's certificate has changed since it was last trusted."
                    : "The printer's certificate matches the one previously trusted."),
        {},
        (tls && obs.certificate_changed.value_or(false))
            ? "Confirm this is the same printer before continuing; do not accept a changed "
              "certificate you cannot account for."
            : "");

    DiagnosticResult api_result = DiagnosticResult::Warning;
    std::string api_summary = "The printer did not report an API version.";
    if (obs.reported_api_version.has_value()) {
        if (obs.minimum_api_version.empty()) {
            api_result  = DiagnosticResult::Pass;
            api_summary = "The printer reports API version " + *obs.reported_api_version + ".";
        } else {
            // Lexicographic comparison is not adequate for versions; compare
            // dotted numeric components.
            auto parts = [](const std::string &value) {
                std::vector<int> out;
                std::string current;
                for (const char c : value + ".") {
                    if (std::isdigit(static_cast<unsigned char>(c))) {
                        // Firmware and discovery responses are untrusted input.
                        // Keep the comparison total for arbitrarily long numeric
                        // components instead of letting std::stoi throw.
                        if (current.size() < 10)
                            current.push_back(c);
                    } else if (c == '.') {
                        int component = 0;
                        if (! current.empty()) {
                            try {
                                component = std::stoi(current);
                            } catch (const std::out_of_range &) {
                                component = std::numeric_limits<int>::max();
                            }
                        }
                        out.push_back(component);
                        current.clear();
                    }
                }
                return out;
            };
            const std::vector<int> have = parts(*obs.reported_api_version);
            const std::vector<int> need = parts(obs.minimum_api_version);
            bool sufficient = true;
            for (size_t i = 0; i < need.size(); ++i) {
                const int h = i < have.size() ? have[i] : 0;
                if (h > need[i]) break;
                if (h < need[i]) { sufficient = false; break; }
            }
            api_result  = sufficient ? DiagnosticResult::Pass : DiagnosticResult::Fail;
            api_summary = sufficient
                ? "The printer's API version " + *obs.reported_api_version + " is supported."
                : "The printer reports API version " + *obs.reported_api_version +
                      ", but " + obs.minimum_api_version + " or newer is required.";
        }
    }
    add(DiagnosticStage::ApiVersion, api_result, api_summary, {},
        api_result == DiagnosticResult::Fail ? "Update the printer's firmware." : "");

    add(DiagnosticStage::StoragePermission, tri(obs.storage_writable),
        obs.storage_writable.value_or(true)
            ? "The printer accepts uploaded files."
            : "The printer rejected a test of its upload storage.",
        {},
        obs.storage_writable.value_or(true)
            ? "" : "Check available space and upload permissions on the printer.");

    return report;
}

} // namespace Slic3r
