///|/ TigerSlicer Phase N presentation viewmodel.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ConnectionsViewModel.hpp"

namespace Slic3r {

DiscoveryConditions discovery_conditions(DiscoveryStatus status, size_t online_results)
{
    DiscoveryConditions conditions;
    // Failed means the scanner could not be started on this system at all -
    // the honest tab state for that is Unsupported, never NoPrintersFound.
    conditions.discovery_supported = status != DiscoveryStatus::Failed;
    conditions.network_online      = status != DiscoveryStatus::NoNetwork;
    conditions.permission_granted  = status != DiscoveryStatus::PermissionDenied;
    conditions.scanning            = status == DiscoveryStatus::Scanning;
    conditions.results             = online_results;
    return conditions;
}

ConnectionStatus connection_status(const ConnectionRow &row)
{
    ConnectionStatus status;
    status.physical_profile = row.title;
    status.technology = row.technology_known
        ? (row.technology_is_resin ? ConnectionTechnology::Resin : ConnectionTechnology::FDM)
        : ConnectionTechnology::Unknown;
    status.reachable            = row.online;
    status.credentials_accepted = !row.requires_authentication;
    status.session_open         = row.session_open;
    status.supports_upload      = row.supports_upload;
    status.supports_control     = row.supports_control;

    if (!row.saved)
        status.state = ConnectionState::NotConfigured;
    else if (row.session_open)
        status.state = ConnectionState::Connected;
    else if (row.online && row.requires_authentication)
        status.state = ConnectionState::AuthenticationRequired;
    else if (row.online)
        status.state = ConnectionState::Disconnected;
    else
        status.state = ConnectionState::ConfiguredOffline;
    return status;
}

PrinterReport printer_report(const ConnectionRow &row, int64_t now_s)
{
    PrinterReport report;
    report.id           = row.id;
    report.display_name = row.title;
    // Discovery advertises identity, never activity. An online row therefore
    // reports Unknown - the overview treats reachable-but-silent as Offline
    // rather than optimistically Ready - and an offline row reports Offline.
    report.activity = row.online ? PrinterActivity::Unknown : PrinterActivity::Offline;
    if (row.last_seen_age_s.has_value())
        report.activity_reported_s = now_s - *row.last_seen_age_s;
    // No capability is claimed: the discovery advertisement carries none of
    // job, temperatures, materials, camera, or queue.
    return report;
}

} // namespace Slic3r
