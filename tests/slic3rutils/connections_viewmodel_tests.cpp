///|/ TigerSlicer Phase N20: acceptance tests for the Physical Printer
///|/ workspace at the decision layer. The wx panel is a thin renderer over
///|/ these mappings and the tested Phase N models; what is asserted here is
///|/ what the panel shows. SAC blocks running GUI executables on the build
///|/ machine, so the GUI itself stays compile-verified and these tests carry
///|/ the behavioral guarantees.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "slic3r/Utils/ConnectionsViewModel.hpp"

using namespace Slic3r;

static ConnectionRow saved_row()
{
    ConnectionRow row;
    row.id    = "saved:Example";
    row.title = "Example";
    row.saved = true;
    row.technology_known = true;
    return row;
}

TEST_CASE("N7: a blocked or offline scan is never reported as no printers found",
          "[connections-viewmodel]")
{
    // Permission denied with zero results must resolve to the permission
    // state, not to "none found".
    CHECK(resolve_discovery_state(discovery_conditions(DiscoveryStatus::PermissionDenied, 0)) ==
          DiscoveryTabState::PermissionDenied);
    CHECK(resolve_discovery_state(discovery_conditions(DiscoveryStatus::NoNetwork, 0)) ==
          DiscoveryTabState::NetworkOffline);
    CHECK(resolve_discovery_state(discovery_conditions(DiscoveryStatus::Failed, 0)) ==
          DiscoveryTabState::Unsupported);
    // A live scan is Discovering even with nothing listed yet.
    CHECK(resolve_discovery_state(discovery_conditions(DiscoveryStatus::Scanning, 0)) ==
          DiscoveryTabState::Discovering);
    // Only a finished, unblocked, empty scan may say none were found.
    CHECK(resolve_discovery_state(discovery_conditions(DiscoveryStatus::Finished, 0)) ==
          DiscoveryTabState::NoPrintersFound);
    CHECK(resolve_discovery_state(discovery_conditions(DiscoveryStatus::Finished, 2)) ==
          DiscoveryTabState::Results);
}

TEST_CASE("N5: a closed session offers Connect and never a control",
          "[connections-viewmodel]")
{
    ConnectionRow row = saved_row();
    row.online = true;
    row.supports_upload = row.supports_control = true;

    const ConnectionView closed = view_connection(connection_status(row));
    CHECK(closed.state == ConnectionState::Disconnected);
    CHECK_FALSE(closed.control_available);
    CHECK_FALSE(closed.upload_available);
    REQUIRE_FALSE(closed.actions.empty());
    CHECK(std::find(closed.actions.begin(), closed.actions.end(),
                    ConnectionAction::Connect) != closed.actions.end());
    CHECK(std::find(closed.actions.begin(), closed.actions.end(),
                    ConnectionAction::Disconnect) == closed.actions.end());

    // Open session: Disconnect appears, Connect does not.
    row.session_open = true;
    const ConnectionView open = view_connection(connection_status(row));
    CHECK(open.state == ConnectionState::Connected);
    CHECK(std::find(open.actions.begin(), open.actions.end(),
                    ConnectionAction::Disconnect) != open.actions.end());
    CHECK(std::find(open.actions.begin(), open.actions.end(),
                    ConnectionAction::Connect) == open.actions.end());
}

TEST_CASE("N5: rejected credentials lead to editing them, not to control",
          "[connections-viewmodel]")
{
    ConnectionRow row = saved_row();
    row.online = true;
    row.requires_authentication = true;
    row.supports_control = row.supports_upload = true;

    const ConnectionView view = view_connection(connection_status(row));
    CHECK(view.state == ConnectionState::AuthenticationRequired);
    CHECK_FALSE(view.control_available);
    CHECK(std::find(view.actions.begin(), view.actions.end(),
                    ConnectionAction::EditCredentials) != view.actions.end());
}

TEST_CASE("N5: a configured but offline printer stays listed with diagnostics",
          "[connections-viewmodel]")
{
    ConnectionRow row = saved_row();
    row.online = false;

    const ConnectionView view = view_connection(connection_status(row));
    CHECK(view.state == ConnectionState::ConfiguredOffline);
    // Offline is not forgotten: it still offers something to do.
    CHECK_FALSE(view.actions.empty());
    CHECK(std::find(view.actions.begin(), view.actions.end(),
                    ConnectionAction::Forget) != view.actions.end());
}

TEST_CASE("N19: reachable-but-silent counts Offline, never optimistically Ready",
          "[connections-viewmodel]")
{
    const int64_t now_s = 1'000'000;

    ConnectionRow silent = saved_row();
    silent.online = true;
    silent.last_seen_age_s = 5;

    ConnectionRow offline = saved_row();
    offline.id = "saved:Other";
    offline.title = "Other";
    offline.online = false;

    const std::vector<PrinterReport> reports = {
        printer_report(silent, now_s), printer_report(offline, now_s)
    };
    const PrinterOverview overview = build_overview(reports, {}, OverviewPolicy{}, now_s);
    CHECK(overview.counts.total == 2);
    CHECK(overview.counts.ready == 0);
    CHECK(overview.counts.printing == 0);
    // Both partition into Offline: nothing reported activity.
    CHECK(overview.counts.offline == 2);
}

TEST_CASE("N10: a stale sighting is never drawn as current", "[connections-viewmodel]")
{
    const int64_t now_s = 1'000'000;
    ConnectionRow row = saved_row();
    row.online = true;

    row.last_seen_age_s = 5;
    const PrinterCard live = build_printer_card(printer_report(row, now_s),
                                                CardDensity::Compact, OverviewPolicy{}, now_s);
    CHECK(live.activity_freshness == Freshness::Live);

    row.last_seen_age_s = 300; // past the 30 s staleness window
    const PrinterCard stale = build_printer_card(printer_report(row, now_s),
                                                 CardDensity::Compact, OverviewPolicy{}, now_s);
    CHECK(stale.activity_freshness == Freshness::Stale);
    CHECK(stale.has_stale_data);

    // Discovery advertises no capabilities, so no field is invented: the
    // card carries no temperature, job, or queue fields.
    CHECK(stale.compact_fields.empty());
}
