///|/ TigerSlicer Phase N: discovered-printer data model.
///|/
///|/ Kept free of network and UI dependencies so the identity, merge, and
///|/ claim-preservation rules can be compiled and verified in isolation.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrinterDiscovery.hpp"

#include <algorithm>

namespace Slic3r {

bool DiscoveredPrinter::same_device(const DiscoveredPrinter &other) const
{
    if (!identity.empty() && identity == other.identity)
        return true;
    // Fall back to host identity so one device advertising over several
    // protocols or addresses collapses into a single row.
    if (!hostname.empty() && hostname == other.hostname)
        return true;
    for (const std::string &addr : addresses)
        if (std::find(other.addresses.begin(), other.addresses.end(), addr) != other.addresses.end())
            return true;
    return false;
}

void DiscoveredPrinter::merge(const DiscoveredPrinter &other)
{
    auto take_if_empty = [](std::string &target, const std::string &value) {
        if (target.empty() && !value.empty())
            target = value;
    };
    take_if_empty(identity, other.identity);
    take_if_empty(name, other.name);
    take_if_empty(hostname, other.hostname);
    take_if_empty(address, other.address);
    take_if_empty(port, other.port);
    take_if_empty(provider, other.provider);
    take_if_empty(vendor, other.vendor);
    take_if_empty(model, other.model);
    take_if_empty(firmware, other.firmware);
    take_if_empty(matched_profile, other.matched_profile);

    // Only the advertisement that actually stated a technology may set it; a
    // later silent sighting must not clear or contradict it.
    if (!technology_known && other.technology_known) {
        technology_known = true;
        technology = other.technology;
    }
    requires_authentication = requires_authentication || other.requires_authentication;

    for (const std::string &addr : other.addresses)
        if (std::find(addresses.begin(), addresses.end(), addr) == addresses.end())
            addresses.push_back(addr);
    for (const std::string &proto : other.protocols)
        if (std::find(protocols.begin(), protocols.end(), proto) == protocols.end())
            protocols.push_back(proto);

    last_seen = std::max(last_seen, other.last_seen);
    online = online || other.online;
}

} // namespace Slic3r
