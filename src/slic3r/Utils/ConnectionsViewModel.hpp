///|/ TigerSlicer Phase N presentation viewmodel: the pure mappings from
///|/ discovery/profile state into the tested Phase N decision models
///|/ (PhysicalPrinterFlow, PrinterOverview). Kept out of the wx panel so the
///|/ mappings themselves are testable - the N20 acceptance tests exercise
///|/ them here, and the wx panel stays a thin renderer.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ConnectionsViewModel_hpp_
#define slic3r_ConnectionsViewModel_hpp_

#include <cstdint>
#include <optional>
#include <string>

#include "libslic3r/PhysicalPrinterFlow.hpp"
#include "libslic3r/PrinterOverview.hpp"
#include "PrinterDiscovery.hpp"

namespace Slic3r {

// One connections-sidebar row as plain data, independent of wx.
struct ConnectionRow
{
    std::string id;
    std::string title;
    bool saved { false };
    bool online { false };
    bool requires_authentication { false };
    bool technology_known { false };
    bool technology_is_resin { false };
    // A live web-interface session is open for this row right now.
    bool session_open { false };
    bool supports_upload { false };
    bool supports_control { false };
    // Seconds since the device was last seen on the network; absent for a
    // saved profile nothing has advertised.
    std::optional<int64_t> last_seen_age_s;
};

// N7: fold the discovery service's status into the conditions the tab-state
// resolver decides from. The resolver picks exactly one state, most blocking
// first, so "no printers found" can never mask a blocked or offline scan.
DiscoveryConditions discovery_conditions(DiscoveryStatus status, size_t online_results);

// N5: a row's connection status, for view_connection() to derive the offered
// actions. A row without a saved profile is NotConfigured whatever else is
// known about it.
ConnectionStatus connection_status(const ConnectionRow &row);

// N10/N19: a row's report for printer cards and the overview aggregation.
// Discovery advertises identity, not activity, so an online row reports
// Unknown activity - the overview counts it as Offline rather than
// optimistically Ready, which is the N19 rule. `now_s` is supplied by the
// caller; nothing here reads a clock.
PrinterReport printer_report(const ConnectionRow &row, int64_t now_s);

} // namespace Slic3r

#endif // slic3r_ConnectionsViewModel_hpp_
