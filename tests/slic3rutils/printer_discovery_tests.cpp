// Phase N: LAN printer discovery service - identity, de-duplication, hostile
// advertisement handling, and offline transitions.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "slic3r/Utils/PrinterDiscovery.hpp"

using namespace Slic3r;

namespace {

DiscoveredPrinter make(const std::string &host, const std::string &address,
                       const std::string &provider = "octoprint")
{
    DiscoveredPrinter printer;
    printer.hostname = host;
    printer.address  = address;
    printer.identity = host.empty() ? address : host;
    printer.provider = provider;
    printer.protocols.push_back(provider);
    if (!address.empty())
        printer.addresses.push_back(address);
    printer.last_seen = std::chrono::steady_clock::now();
    return printer;
}

} // namespace

TEST_CASE("Discovery identifies the same device across protocols and addresses", "[discovery]")
{
    DiscoveredPrinter first  = make("printer.local", "192.168.1.20:80", "octoprint");
    DiscoveredPrinter second = make("printer.local", "192.168.1.20:8080", "moonraker");

    REQUIRE(first.same_device(second));

    first.merge(second);
    // Both addresses are preserved rather than one silently replacing the other.
    CHECK(first.addresses.size() == 2);
    CHECK(first.protocols.size() == 2);

    // A device seen only by address still matches when the address repeats.
    DiscoveredPrinter by_address = make("", "192.168.1.20:80");
    CHECK(first.same_device(by_address));

    // A genuinely different device must not collapse into this one.
    DiscoveredPrinter other = make("other.local", "192.168.1.44:80");
    CHECK_FALSE(first.same_device(other));
}

TEST_CASE("Merging never invents or contradicts a technology claim", "[discovery]")
{
    DiscoveredPrinter unknown = make("p.local", "10.0.0.5:80");
    REQUIRE_FALSE(unknown.technology_known);

    DiscoveredPrinter resin = make("p.local", "10.0.0.5:80");
    resin.technology_known = true;
    resin.technology = ptSLA;

    unknown.merge(resin);
    CHECK(unknown.technology_known);
    CHECK(unknown.technology == ptSLA);

    // A later sighting that states nothing must not clear the known value.
    DiscoveredPrinter silent = make("p.local", "10.0.0.5:80");
    unknown.merge(silent);
    CHECK(unknown.technology_known);
    CHECK(unknown.technology == ptSLA);
}

TEST_CASE("Merging preserves the strongest known identity fields", "[discovery]")
{
    DiscoveredPrinter sparse = make("p.local", "10.0.0.5:80");
    DiscoveredPrinter rich   = make("p.local", "10.0.0.5:80");
    rich.model    = "MK4";
    rich.vendor   = "Prusa";
    rich.firmware = "2.7.1";

    sparse.merge(rich);
    CHECK(sparse.model == "MK4");
    CHECK(sparse.vendor == "Prusa");
    CHECK(sparse.firmware == "2.7.1");

    // An empty later value must not erase a known one.
    DiscoveredPrinter blank = make("p.local", "10.0.0.5:80");
    sparse.merge(blank);
    CHECK(sparse.model == "MK4");
}

TEST_CASE("Authentication requirement is sticky once advertised", "[discovery]")
{
    DiscoveredPrinter open = make("p.local", "10.0.0.5:80");
    DiscoveredPrinter guarded = make("p.local", "10.0.0.5:80");
    guarded.requires_authentication = true;

    open.merge(guarded);
    CHECK(open.requires_authentication);

    // Never downgrade a security-relevant claim on a later silent sighting.
    open.merge(make("p.local", "10.0.0.5:80"));
    CHECK(open.requires_authentication);
}

TEST_CASE("Manually added hosts join the device set without contacting them", "[discovery]")
{
    PrinterDiscoveryService service;
    DiscoveredPrinters seen;
    service.set_update_callback([&seen](DiscoveredPrinters devices, DiscoveryStatus) {
        seen = std::move(devices);
    });

    service.add_manual(make("manual.local", "10.1.1.7:80"));
    REQUIRE(seen.size() == 1);
    CHECK(seen.front().hostname == "manual.local");
    CHECK_FALSE(service.is_scanning());
    CHECK(service.status() == DiscoveryStatus::Idle);

    // Re-adding the same host merges instead of duplicating the row.
    service.add_manual(make("manual.local", "10.1.1.7:8080"));
    REQUIRE(seen.size() == 1);
    CHECK(seen.front().addresses.size() == 2);
}

TEST_CASE("Stopping discovery leaves it idle and silent", "[discovery]")
{
    PrinterDiscoveryService service;
    int callbacks = 0;
    service.set_update_callback([&callbacks](DiscoveredPrinters, DiscoveryStatus) { ++callbacks; });

    service.stop();
    CHECK_FALSE(service.is_scanning());
    CHECK(service.status() == DiscoveryStatus::Idle);
    // stop() on an idle service must not fabricate an update.
    CHECK(callbacks == 0);
}
