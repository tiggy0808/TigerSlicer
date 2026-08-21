///|/ TigerSlicer Phase N: asynchronous LAN printer discovery service.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterDiscovery.hpp"

#include <algorithm>
#include <map>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include "Bonjour.hpp"

namespace Slic3r {

namespace {

// Bound every string taken from a LAN advertisement: the payload is untrusted
// input and must not be able to inflate memory or smuggle control characters
// into the UI, logs, or profile names.
std::string sanitize(const std::string &value, size_t max_length = 120)
{
    std::string out;
    out.reserve(std::min(value.size(), max_length));
    for (const char c : value) {
        if (out.size() >= max_length)
            break;
        const unsigned char uc = static_cast<unsigned char>(c);
        // Drop C0/C1 controls; keep ordinary printable and UTF-8 continuation.
        if (uc < 0x20 || uc == 0x7f)
            continue;
        out.push_back(c);
    }
    boost::algorithm::trim(out);
    return out;
}

// mDNS service types TigerSlicer's own connectors can actually talk to.
// Discovery deliberately advertises nothing and scans no ports.
struct ServiceProbe
{
    const char *service;   // Bonjour service name
    const char *provider;  // TigerSlicer print-host provider id
};

const ServiceProbe service_probes[] = {
    { "octoprint", "octoprint" },
};

} // namespace

struct PrinterDiscoveryService::priv
{
    mutable std::mutex     mutex;
    DiscoveredPrinters     devices;
    DiscoveryStatus        status { DiscoveryStatus::Idle };
    UpdateFn               update_fn;
    std::vector<Bonjour::Ptr> lookups;
    // Incremented per scan; a reply carrying a stale generation is ignored so
    // a cancelled scan can never resurrect itself through a late callback.
    std::atomic<unsigned>  generation { 0 };
    std::atomic<int>       pending { 0 };

    void notify_locked(std::unique_lock<std::mutex> &lock)
    {
        UpdateFn fn = update_fn;
        DiscoveredPrinters snapshot = devices;
        DiscoveryStatus st = status;
        lock.unlock();
        if (fn)
            fn(std::move(snapshot), st);
        lock.lock();
    }
};

PrinterDiscoveryService::PrinterDiscoveryService() : p(new priv()) {}

PrinterDiscoveryService::~PrinterDiscoveryService()
{
    stop();
}

void PrinterDiscoveryService::set_update_callback(UpdateFn fn)
{
    std::lock_guard<std::mutex> lock(p->mutex);
    p->update_fn = std::move(fn);
}

DiscoveryStatus PrinterDiscoveryService::status() const
{
    std::lock_guard<std::mutex> lock(p->mutex);
    return p->status;
}

DiscoveredPrinters PrinterDiscoveryService::devices() const
{
    std::lock_guard<std::mutex> lock(p->mutex);
    return p->devices;
}

void PrinterDiscoveryService::add_manual(const DiscoveredPrinter &printer)
{
    std::unique_lock<std::mutex> lock(p->mutex);
    for (DiscoveredPrinter &known : p->devices) {
        if (known.same_device(printer)) {
            known.merge(printer);
            p->notify_locked(lock);
            return;
        }
    }
    p->devices.push_back(printer);
    p->notify_locked(lock);
}

void PrinterDiscoveryService::stop()
{
    // Invalidate in-flight replies before releasing the lookups so a reply
    // already queued on a worker thread becomes a no-op.
    p->generation.fetch_add(1, std::memory_order_acq_rel);
    m_scanning.store(false, std::memory_order_release);

    std::vector<Bonjour::Ptr> lookups;
    {
        std::lock_guard<std::mutex> lock(p->mutex);
        lookups.swap(p->lookups);
        if (p->status == DiscoveryStatus::Scanning)
            p->status = DiscoveryStatus::Idle;
    }
    lookups.clear();
}

void PrinterDiscoveryService::start()
{
    stop();

    const unsigned generation = p->generation.load(std::memory_order_acquire);
    m_scanning.store(true, std::memory_order_release);

    {
        std::unique_lock<std::mutex> lock(p->mutex);
        p->status = DiscoveryStatus::Scanning;
        // Previously seen devices stay listed; this scan refreshes last_seen
        // for the ones that answer again.
        p->notify_locked(lock);
    }

    priv *pp = p.get();
    std::atomic<bool> *scanning = &m_scanning;
    const int probe_count = int(sizeof(service_probes) / sizeof(service_probes[0]));
    p->pending.store(probe_count, std::memory_order_release);

    std::vector<Bonjour::Ptr> lookups;
    for (const ServiceProbe &probe : service_probes) {
        const std::string provider = probe.provider;
        try {
            Bonjour::Ptr lookup = Bonjour(probe.service)
                .set_txt_keys({ "version", "model", "vendor", "type" })
                .set_retries(2)
                .set_timeout(4)
                .on_reply([pp, generation, provider](BonjourReply &&reply) {
                    if (pp->generation.load(std::memory_order_acquire) != generation)
                        return; // scan cancelled or superseded
                    DiscoveredPrinter found;
                    found.name     = sanitize(reply.service_name);
                    found.hostname = sanitize(reply.hostname);
                    found.address  = sanitize(reply.full_address);
                    found.port     = std::to_string(reply.port);
                    found.provider = provider;
                    found.protocols.push_back(provider);
                    if (!found.address.empty())
                        found.addresses.push_back(found.address);
                    found.identity = found.hostname.empty() ? found.address : found.hostname;

                    auto txt = [&reply](const char *key) -> std::string {
                        const auto it = reply.txt_data.find(key);
                        return it == reply.txt_data.end() ? std::string() : sanitize(it->second);
                    };
                    found.model    = txt("model");
                    found.vendor   = txt("vendor");
                    found.firmware = txt("version");

                    // Only claim a technology when the advertisement states a
                    // model we can classify; otherwise leave it unknown.
                    if (found.model == "SL1" || found.model == "SL1S" || found.model == "M1") {
                        found.technology_known = true;
                        found.technology = ptSLA;
                    } else if (!found.model.empty()) {
                        found.technology_known = true;
                        found.technology = ptFFF;
                    }

                    found.last_seen = std::chrono::steady_clock::now();
                    found.online = true;

                    std::unique_lock<std::mutex> lock(pp->mutex);
                    for (DiscoveredPrinter &known : pp->devices) {
                        if (known.same_device(found)) {
                            known.merge(found);
                            known.online = true;
                            pp->notify_locked(lock);
                            return;
                        }
                    }
                    pp->devices.push_back(std::move(found));
                    pp->notify_locked(lock);
                })
                .on_complete([pp, generation, scanning]() {
                    if (pp->generation.load(std::memory_order_acquire) != generation)
                        return;
                    if (pp->pending.fetch_sub(1, std::memory_order_acq_rel) > 1)
                        return; // other probes still running
                    scanning->store(false, std::memory_order_release);

                    const auto now = std::chrono::steady_clock::now();
                    std::unique_lock<std::mutex> lock(pp->mutex);
                    for (DiscoveredPrinter &known : pp->devices) {
                        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                            now - known.last_seen).count();
                        if (age > offline_after_seconds)
                            known.online = false; // understandable offline state, not removal
                    }
                    pp->status = DiscoveryStatus::Finished;
                    pp->notify_locked(lock);
                })
                .lookup();
            lookups.push_back(std::move(lookup));
        } catch (const std::exception &error) {
            BOOST_LOG_TRIVIAL(error) << "Printer discovery probe failed: " << error.what();
            if (p->pending.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
                m_scanning.store(false, std::memory_order_release);
                std::unique_lock<std::mutex> lock(p->mutex);
                // A failed socket is reported as a failure, never as an empty
                // but successful result.
                p->status = DiscoveryStatus::Failed;
                p->notify_locked(lock);
            }
        }
    }

    std::lock_guard<std::mutex> lock(p->mutex);
    p->lookups = std::move(lookups);
}

} // namespace Slic3r
