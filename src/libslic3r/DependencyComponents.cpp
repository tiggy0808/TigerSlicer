///|/ TigerSlicer Phase 25A: dependency-component decomposition for the
///|/ experimental bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "DependencyComponents.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r.h"

namespace Slic3r {

const char *region_kind_name(RegionKind kind)
{
    switch (kind) {
    case RegionKind::Object:           return "object";
    case RegionKind::Part:             return "part";
    case RegionKind::Volume:           return "volume";
    case RegionKind::PaintedRegion:    return "painted region";
    case RegionKind::Support:          return "support";
    case RegionKind::SupportInterface: return "support interface";
    }
    return "object";
}

const char *link_kind_name(LinkKind kind)
{
    switch (kind) {
    case LinkKind::Touching:        return "touching";
    case LinkKind::Overlapping:     return "overlapping";
    case LinkKind::Stacked:         return "stacked";
    case LinkKind::Supports:        return "supports";
    case LinkKind::Encloses:        return "encloses";
    case LinkKind::BridgesTo:       return "bridges to";
    case LinkKind::SharedInterface: return "shares an interface with";
    case LinkKind::Declared:        return "declared dependent on";
    case LinkKind::Unknown:         return "related in an unrecorded way";
    }
    return "declared dependent on";
}

const char *independence_verdict_name(IndependenceVerdict verdict)
{
    switch (verdict) {
    case IndependenceVerdict::Independent: return "independent";
    case IndependenceVerdict::Unproven:    return "unproven";
    }
    return "unproven";
}

bool regions_separated_in_xy(const PrintableRegion &a, const PrintableRegion &b,
                             double margin_mm)
{
    return a.max_x + margin_mm < b.min_x || b.max_x + margin_mm < a.min_x ||
           a.max_y + margin_mm < b.min_y || b.max_y + margin_mm < a.min_y;
}

namespace {

std::string number(double value, int precision)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(precision);
    out << value;
    return out.str();
}

std::string region_name(size_t id) { return "region " + std::to_string(id); }

struct DisjointSet
{
    std::vector<size_t> parent;

    explicit DisjointSet(size_t count) : parent(count)
    {
        for (size_t index = 0; index < count; ++index)
            parent[index] = index;
    }

    size_t find(size_t index)
    {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index         = parent[index];
        }
        return index;
    }

    bool join(size_t left, size_t right)
    {
        const size_t a = find(left);
        const size_t b = find(right);
        if (a == b)
            return false;
        // Always keep the lower root, so the numbering does not depend on the
        // order the merges happened in.
        if (a < b)
            parent[b] = a;
        else
            parent[a] = b;
        return true;
    }
};

bool overlaps_in_z(const PrintableRegion &a, const PrintableRegion &b, double tolerance)
{
    return a.min_z < b.max_z - tolerance && b.min_z < a.max_z - tolerance;
}

} // namespace

ComponentDecomposition decompose_components(const std::vector<PrintableRegion> &regions,
                                            const std::vector<RegionLink>      &links,
                                            const ComponentOptions             &options)
{
    ComponentDecomposition result;

    if (regions.empty()) {
        result.error = "no printable regions were described, so there is nothing to "
                       "decompose";
        return result;
    }

    size_t considered = regions.size();
    if (considered > options.max_regions) {
        considered = options.max_regions;
        result.notes.push_back(
            "this print has " + std::to_string(regions.size()) + " regions and only the "
            "first " + std::to_string(options.max_regions) +
            " were examined; the rest were not decomposed at all, so batching is not "
            "offered");
    }

    // Region id to index, so links can name regions rather than positions.
    auto index_of = [&](size_t id) -> std::optional<size_t> {
        for (size_t index = 0; index < considered; ++index)
            if (regions[index].id == id)
                return index;
        return std::nullopt;
    };

    DisjointSet                          sets(considered);
    std::vector<std::vector<std::string>> why(considered);
    std::vector<bool>                     unproven(considered, false);

    auto merge = [&](size_t left, size_t right, const std::string &reason) {
        if (sets.join(left, right)) {
            why[left].push_back(reason);
            why[right].push_back(reason);
        }
    };

    // Declared dependencies first: the caller knows things the boxes do not.
    for (const RegionLink &link : links) {
        const std::optional<size_t> left  = index_of(link.a);
        const std::optional<size_t> right = index_of(link.b);
        if (!left.has_value() || !right.has_value()) {
            result.notes.push_back("a declared link names a region that is not in this "
                                   "print, so it could not be applied");
            continue;
        }
        const std::string reason =
            region_name(link.a) + " " + link_kind_name(link.kind) + " " +
            region_name(link.b) +
            (link.description.empty() ? std::string() : ": " + link.description);
        merge(*left, *right, reason);
        if (link.kind == LinkKind::Unknown) {
            unproven[*left]  = true;
            unproven[*right] = true;
            why[*left].push_back("the way these two are related was never recorded, so this "
                                 "component cannot be called independent");
        }
    }

    // Geometry. Regions that are not clearly apart in xy stay together,
    // whether they touch side by side or one sits above the other.
    for (size_t left = 0; left < considered; ++left) {
        for (size_t right = left + 1; right < considered; ++right) {
            const PrintableRegion &a = regions[left];
            const PrintableRegion &b = regions[right];

            if (!regions_separated_in_xy(a, b, options.separation_margin_mm)) {
                const bool stacked = !overlaps_in_z(a, b, options.tolerance_mm);
                merge(left, right,
                      region_name(a.id) + " and " + region_name(b.id) +
                          (stacked ? " share ground in xy with one above the other, so the "
                                     "upper one is printed onto the lower"
                                   : " come within " +
                                         number(options.separation_margin_mm, 2) +
                                         " mm of each other in xy, so they are not "
                                         "independent"));
                continue;
            }

            // Same object, and splitting inside an object was not asked for.
            if (a.parent_object == b.parent_object && !options.allow_intra_object_split)
                merge(left, right,
                      region_name(a.id) + " and " + region_name(b.id) +
                          " are parts of the same object; splitting a component out of one "
                          "object was not enabled, so they are kept together");
        }
    }

    // A region nobody actually looked at cannot make its component independent.
    for (size_t index = 0; index < considered; ++index) {
        if (regions[index].geometry_analyzed)
            continue;
        if (options.require_geometry_analysis) {
            unproven[index] = true;
            why[index].push_back(
                region_name(regions[index].id) +
                " was never analyzed beyond its bounding box; looking separate is not proof "
                "of being separate, so this component is unproven");
        } else {
            result.notes.push_back(region_name(regions[index].id) +
                                   " was never analyzed, and geometry analysis was not "
                                   "required; its independence rests on its bounding box "
                                   "alone");
        }
    }

    // Gather. Roots are the lowest index in each set, so the output order and
    // the component ids are the same for the same input every time.
    std::vector<size_t> roots;
    for (size_t index = 0; index < considered; ++index) {
        const size_t root = sets.find(index);
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    }
    std::sort(roots.begin(), roots.end());

    for (size_t root : roots) {
        DependencyComponent component;
        component.id      = regions[root].id;
        component.verdict = IndependenceVerdict::Independent;
        component.min_z   = regions[root].min_z;
        component.max_z   = regions[root].max_z;

        for (size_t index = 0; index < considered; ++index) {
            if (sets.find(index) != root)
                continue;
            component.regions.push_back(regions[index].id);
            component.min_z = std::min(component.min_z, regions[index].min_z);
            component.max_z = std::max(component.max_z, regions[index].max_z);
            if (std::find(component.tools.begin(), component.tools.end(),
                          regions[index].tool) == component.tools.end())
                component.tools.push_back(regions[index].tool);
            if (unproven[index])
                component.verdict = IndependenceVerdict::Unproven;
            for (const std::string &reason : why[index])
                if (std::find(component.reasons.begin(), component.reasons.end(), reason) ==
                    component.reasons.end())
                    component.reasons.push_back(reason);
        }

        std::sort(component.regions.begin(), component.regions.end());
        std::sort(component.tools.begin(), component.tools.end());
        component.id = component.regions.front();
        result.components.push_back(component);
    }

    // By stable id, not by the order the regions happened to be presented in.
    // The same print described with its objects listed in a different order is
    // the same print, and anything downstream that pairs components with
    // operations by position would otherwise get a different answer for it.
    std::sort(result.components.begin(), result.components.end(),
              [](const DependencyComponent &a, const DependencyComponent &b) {
                  return a.id < b.id;
              });

    const bool all_independent =
        std::all_of(result.components.begin(), result.components.end(),
                    [](const DependencyComponent &component) {
                        return component.verdict == IndependenceVerdict::Independent;
                    });

    result.batching_eligible =
        result.components.size() > 1 && all_independent && considered == regions.size();

    if (result.components.size() < 2)
        result.notes.push_back("everything in this print depends on everything else, so "
                               "there is only one component and there is nothing to batch "
                               "between");
    else if (!all_independent)
        result.notes.push_back("at least one component could not be shown to be independent, "
                               "so bounded-Z batching is not offered for this print");

    return result;
}

} // namespace Slic3r
