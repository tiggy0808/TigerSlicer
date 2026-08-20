// Executable verification of the discovered-printer data model.
// Compiles the production PrinterDiscoveryModel.cpp directly; see run.sh for
// why this second execution path exists.
#include <chrono>
#include <cstdio>
#include <string>

#include "slic3r/Utils/PrinterDiscovery.hpp"

using namespace Slic3r;

static int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("   FAILED: %s (line %d)\n", #expr, __LINE__);         \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static DiscoveredPrinter make(const std::string &host, const std::string &address,
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

int main()
{
    // Same device across protocols and addresses.
    {
        DiscoveredPrinter a = make("printer.local", "192.168.1.20:80", "octoprint");
        DiscoveredPrinter b = make("printer.local", "192.168.1.20:8080", "moonraker");
        CHECK(a.same_device(b));
        a.merge(b);
        CHECK(a.addresses.size() == 2);
        CHECK(a.protocols.size() == 2);
        CHECK(a.same_device(make("", "192.168.1.20:80")));
        CHECK(!a.same_device(make("other.local", "192.168.1.44:80")));
    }

    // Technology claims are never invented and never contradicted.
    {
        DiscoveredPrinter unknown = make("p.local", "10.0.0.5:80");
        CHECK(!unknown.technology_known);
        DiscoveredPrinter resin = make("p.local", "10.0.0.5:80");
        resin.technology_known = true;
        resin.technology = ptSLA;
        unknown.merge(resin);
        CHECK(unknown.technology_known);
        CHECK(unknown.technology == ptSLA);
        unknown.merge(make("p.local", "10.0.0.5:80"));
        CHECK(unknown.technology_known);
        CHECK(unknown.technology == ptSLA);
    }

    // Known identity fields survive a later, sparser sighting.
    {
        DiscoveredPrinter sparse = make("p.local", "10.0.0.5:80");
        DiscoveredPrinter rich   = make("p.local", "10.0.0.5:80");
        rich.model = "MK4"; rich.vendor = "Prusa"; rich.firmware = "2.7.1";
        sparse.merge(rich);
        CHECK(sparse.model == "MK4");
        CHECK(sparse.vendor == "Prusa");
        sparse.merge(make("p.local", "10.0.0.5:80"));
        CHECK(sparse.model == "MK4");
    }

    // A security-relevant claim is sticky.
    {
        DiscoveredPrinter open = make("p.local", "10.0.0.5:80");
        DiscoveredPrinter guarded = make("p.local", "10.0.0.5:80");
        guarded.requires_authentication = true;
        open.merge(guarded);
        CHECK(open.requires_authentication);
        open.merge(make("p.local", "10.0.0.5:80"));
        CHECK(open.requires_authentication);
    }

    if (failures == 0)
        std::printf("   discovery model: all assertions passed\n");
    return failures == 0 ? 0 : 1;
}
