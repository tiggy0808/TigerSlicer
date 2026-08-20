// Phase N: connection diagnostics. The value of this flow is that it points at
// the real cause and never leaks credentials, so the tests focus on ordering
// (later stages are "not checked" once something fails), on unobserved data
// not counting as success, and on redaction.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "libslic3r/ConnectionDiagnostics.hpp"

using namespace Slic3r;

namespace {

DiagnosticObservations healthy()
{
    DiagnosticObservations obs;
    obs.has_network_interface    = true;
    obs.local_network_permitted  = true;
    obs.hostname_resolved        = true;
    obs.resolved_addresses       = { "192.168.1.20" };
    obs.route_available          = true;
    obs.service_advertised       = true;
    obs.port_open                = true;
    obs.authentication_required  = false;
    obs.tls_used                 = false;
    obs.reported_api_version     = "1.4.0";
    obs.minimum_api_version      = "1.2.0";
    obs.storage_writable         = true;
    obs.timed_out                = false;
    return obs;
}

const DiagnosticStep *find(const DiagnosticReport &report, DiagnosticStage stage)
{
    const auto it = std::find_if(report.steps.begin(), report.steps.end(),
        [stage](const DiagnosticStep &step) { return step.stage == stage; });
    return it == report.steps.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("A healthy connection reports no failure", "[diagnostics]")
{
    const DiagnosticReport report = diagnose_connection(healthy());
    CHECK(report.healthy());
    CHECK_FALSE(report.first_failure.has_value());
}

TEST_CASE("Stages after a failure are not checked rather than failed", "[diagnostics]")
{
    DiagnosticObservations obs = healthy();
    obs.hostname_resolved = false;

    const DiagnosticReport report = diagnose_connection(obs);
    REQUIRE(report.first_failure.has_value());
    CHECK(*report.first_failure == DiagnosticStage::HostnameResolution);

    // Downstream stages must not be presented as problems of their own.
    const DiagnosticStep *port = find(report, DiagnosticStage::PortReachable);
    REQUIRE(port != nullptr);
    CHECK(port->result == DiagnosticResult::NotChecked);

    // ...and an earlier passing stage is still reported as passing.
    const DiagnosticStep *iface = find(report, DiagnosticStage::NetworkInterface);
    REQUIRE(iface != nullptr);
    CHECK(iface->result == DiagnosticResult::Pass);
}

TEST_CASE("The first failure is what gets reported", "[diagnostics]")
{
    DiagnosticObservations obs = healthy();
    obs.local_network_permitted = false;
    obs.port_open               = false;   // a later problem too

    const DiagnosticReport report = diagnose_connection(obs);
    REQUIRE(report.first_failure.has_value());
    // Sending the user after the port when permission is the cause would waste
    // their time.
    CHECK(*report.first_failure == DiagnosticStage::LocalNetworkPermission);
}

TEST_CASE("Unobserved data warns instead of passing", "[diagnostics]")
{
    DiagnosticObservations obs;   // nothing observed at all
    const DiagnosticReport report = diagnose_connection(obs);

    const DiagnosticStep *iface = find(report, DiagnosticStage::NetworkInterface);
    REQUIRE(iface != nullptr);
    CHECK(iface->result == DiagnosticResult::Warning);
    CHECK(report.healthy());   // warnings are not failures
}

TEST_CASE("A printer that does not advertise itself is not a failure", "[diagnostics]")
{
    DiagnosticObservations obs = healthy();
    obs.service_advertised = false;

    const DiagnosticReport report = diagnose_connection(obs);
    const DiagnosticStep *discovery = find(report, DiagnosticStage::ServiceDiscovery);
    REQUIRE(discovery != nullptr);
    // Manual-by-address printers are ordinary, so this may only warn.
    CHECK(discovery->result == DiagnosticResult::Warning);
    CHECK(report.healthy());
}

TEST_CASE("A timeout is reported as a reachability failure", "[diagnostics]")
{
    DiagnosticObservations obs = healthy();
    obs.timed_out = true;

    const DiagnosticReport report = diagnose_connection(obs);
    REQUIRE(report.first_failure.has_value());
    CHECK(*report.first_failure == DiagnosticStage::PortReachable);
    CHECK_FALSE(find(report, DiagnosticStage::PortReachable)->next_action.empty());
}

TEST_CASE("Missing credentials only matter when authentication is required", "[diagnostics]")
{
    DiagnosticObservations open = healthy();
    open.authentication_required = false;
    open.authenticated.reset();
    CHECK(find(diagnose_connection(open), DiagnosticStage::Authentication)->result ==
          DiagnosticResult::Pass);

    DiagnosticObservations guarded = healthy();
    guarded.authentication_required = true;
    guarded.authenticated           = false;
    const DiagnosticReport report = diagnose_connection(guarded);
    REQUIRE(report.first_failure.has_value());
    CHECK(*report.first_failure == DiagnosticStage::Authentication);
}

TEST_CASE("A changed certificate warns and is never auto-trusted", "[diagnostics]")
{
    DiagnosticObservations obs = healthy();
    obs.tls_used            = true;
    obs.certificate_changed = true;

    const DiagnosticReport report = diagnose_connection(obs);
    const DiagnosticStep *tls = find(report, DiagnosticStage::CertificateIdentity);
    REQUIRE(tls != nullptr);
    CHECK(tls->result == DiagnosticResult::Warning);
    CHECK_FALSE(tls->next_action.empty());
}

TEST_CASE("API versions are compared numerically, not lexically", "[diagnostics]")
{
    DiagnosticObservations newer = healthy();
    newer.reported_api_version = "1.10.0";
    newer.minimum_api_version  = "1.9.0";
    // "1.10.0" < "1.9.0" as strings, so a lexical compare would wrongly fail.
    CHECK(find(diagnose_connection(newer), DiagnosticStage::ApiVersion)->result ==
          DiagnosticResult::Pass);

    DiagnosticObservations older = healthy();
    older.reported_api_version = "1.1.0";
    older.minimum_api_version  = "1.9.0";
    CHECK(find(diagnose_connection(older), DiagnosticStage::ApiVersion)->result ==
          DiagnosticResult::Fail);

    DiagnosticObservations malformed = healthy();
    malformed.reported_api_version =
        "999999999999999999999999999999999999999999999999.0";
    malformed.minimum_api_version = "2.0";
    // A malformed or hostile discovery response must not terminate the app.
    CHECK_NOTHROW(diagnose_connection(malformed));
    CHECK(find(diagnose_connection(malformed), DiagnosticStage::ApiVersion)->result ==
          DiagnosticResult::Pass);
}

TEST_CASE("Credentials are redacted from diagnostic text", "[diagnostics]")
{
    CHECK(redact_secrets("api_key=SUPERSECRET123").find("SUPERSECRET123") == std::string::npos);
    CHECK(redact_secrets("X-Api-Key: abc123def").find("abc123def") == std::string::npos);
    CHECK(redact_secrets("password: hunter2").find("hunter2") == std::string::npos);
    CHECK(redact_secrets("http://user:pa55w0rd@printer.local/api").find("pa55w0rd") ==
          std::string::npos);

    // Ordinary text must survive intact so reports stay useful.
    const std::string plain = "Resolved to: 192.168.1.20";
    CHECK(redact_secrets(plain) == plain);
}
