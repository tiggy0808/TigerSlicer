///|/ TigerSlicer Phase N: asynchronous LAN printer discovery service.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrinterDiscovery_hpp_
#define slic3r_PrinterDiscovery_hpp_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Only the PrinterTechnology enum is needed here; Config.hpp is far lighter
// than PrintConfig.hpp and keeps the data model independently compilable.
#include "libslic3r/Config.hpp"

namespace Slic3r {

// One device advertised on the local network. Every field is either verified
// from the advertisement or explicitly unknown - discovery never guesses a
// model, technology, or compatibility claim.
struct DiscoveredPrinter
{
    // Stable identity used for de-duplication across protocols and addresses.
    // Derived from the strongest identifier the advertisement provided.
    std::string identity;

    std::string name;          // advertised service/instance name
    std::string hostname;      // advertised hostname, may be empty
    std::string address;       // resolved address (host:port form)
    std::string port;
    std::string provider;      // TigerSlicer print-host provider id, if known
    std::string vendor;        // only when advertised
    std::string model;         // only when advertised
    std::string firmware;      // only when advertised

    // ptFFF/ptSLA only when the advertisement actually identifies it.
    bool                technology_known { false };
    PrinterTechnology   technology { ptFFF };

    bool requires_authentication { false };

    // Every address the device was seen at, preserved rather than collapsed.
    std::vector<std::string> addresses;
    // Protocol ids this device answered on (e.g. "octoprint", "sdcp").
    std::vector<std::string> protocols;

    std::chrono::steady_clock::time_point last_seen;
    bool online { true };

    // A saved physical-printer profile whose host matches this device.
    std::string matched_profile;

    bool same_device(const DiscoveredPrinter &other) const;
    // Fold another sighting of the same device into this record.
    void merge(const DiscoveredPrinter &other);
};

using DiscoveredPrinters = std::vector<DiscoveredPrinter>;

// Reasons discovery can stop producing results, reported verbatim to the UI
// so a permission or interface problem is never shown as "no printers found".
enum class DiscoveryStatus
{
    Idle,
    Scanning,
    Finished,
    NoNetwork,
    PermissionDenied,
    Failed,
};

// Asynchronous, cancellable discovery over the protocols TigerSlicer's print
// host connectors actually support. Results are delivered on a caller-provided
// callback; the service owns no UI and performs no network writes beyond the
// local-link mDNS queries its providers issue.
class PrinterDiscoveryService
{
public:
    // Called from a worker thread whenever the device set or status changes.
    // The callback must marshal to the UI thread itself.
    using UpdateFn = std::function<void(DiscoveredPrinters, DiscoveryStatus)>;

    PrinterDiscoveryService();
    ~PrinterDiscoveryService();

    PrinterDiscoveryService(const PrinterDiscoveryService &) = delete;
    PrinterDiscoveryService &operator=(const PrinterDiscoveryService &) = delete;

    void set_update_callback(UpdateFn fn);

    // Start (or restart) a scan. Safe to call while a scan is running; the
    // previous scan is cancelled first. Devices already known are retained and
    // marked offline if this scan does not see them again.
    void start();
    // Cooperatively cancel. Returns immediately; no callback fires afterwards.
    void stop();

    bool is_scanning() const { return m_scanning.load(std::memory_order_acquire); }
    DiscoveryStatus status() const;
    DiscoveredPrinters devices() const;

    // Fold a manually entered host into the device set so the sidebar can show
    // it beside discovered devices. Never contacts the device.
    void add_manual(const DiscoveredPrinter &printer);

    // Seconds after which an unseen device is reported offline rather than
    // silently dropped.
    static constexpr int offline_after_seconds = 120;

private:
    struct priv;
    std::unique_ptr<priv> p;
    std::atomic<bool> m_scanning { false };
};

} // namespace Slic3r

#endif // slic3r_PrinterDiscovery_hpp_
