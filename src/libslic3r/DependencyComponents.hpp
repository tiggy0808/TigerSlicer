///|/ TigerSlicer Phase 25A: dependency-component decomposition for the
///|/ experimental bounded-Z batching scheduler.
///|/
///|/ TigerSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_DependencyComponents_hpp_
#define slic3r_DependencyComponents_hpp_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

enum class RegionKind : uint8_t
{
    Object,
    Part,
    Volume,
    PaintedRegion,
    Support,
    SupportInterface,
};

const char *region_kind_name(RegionKind kind);

// Why two regions are not independent of each other.
enum class LinkKind : uint8_t
{
    Touching,
    Overlapping,
    Stacked,
    Supports,
    Encloses,
    BridgesTo,
    SharedInterface,
    // Declared by the caller without a reason of its own.
    Declared,
    // The caller knows these two are related but not how. This is a real
    // answer and it keeps the component unproven.
    Unknown,
};

const char *link_kind_name(LinkKind kind);

enum class IndependenceVerdict : uint8_t
{
    // Shown to be independent of every other component.
    Independent,
    // Something needed to decide was never examined. Batching must not treat
    // this as independent.
    Unproven,
};

const char *independence_verdict_name(IndependenceVerdict verdict);

struct PrintableRegion
{
    size_t     id { 0 };
    RegionKind kind { RegionKind::Object };
    // Which top-level object this came out of.
    size_t parent_object { 0 };

    int         tool { 0 };
    std::string color;

    double min_x { 0. }, max_x { 0. };
    double min_y { 0. }, max_y { 0. };
    double min_z { 0. }, max_z { 0. };

    // Whether the actual geometry was examined, as opposed to only the
    // bounding box or the object list. False keeps the component unproven:
    // visual separation alone is never proof of independence.
    bool geometry_analyzed { true };
};

struct RegionLink
{
    size_t      a { 0 }, b { 0 };
    LinkKind    kind { LinkKind::Declared };
    std::string description;
};

struct ComponentOptions
{
    // Two regions closer than this in xy are not treated as separate.
    double separation_margin_mm { 0.5 };

    // Whether disconnected regions inside one object may become separate
    // components. Off by default, because one object's regions are usually
    // related in ways the bounding boxes do not show.
    bool allow_intra_object_split { false };

    // Whether a region whose geometry was never analyzed blocks the whole
    // decomposition from being used for batching.
    bool require_geometry_analysis { true };

    size_t max_regions { 2000 };

    double tolerance_mm { 1e-6 };
};

struct DependencyComponent
{
    // The lowest region id in the component, so the numbering is stable.
    size_t              id { 0 };
    std::vector<size_t> regions;
    std::vector<int>    tools;

    IndependenceVerdict verdict { IndependenceVerdict::Unproven };

    double min_z { 0. }, max_z { 0. };

    std::vector<std::string> reasons;
};

struct ComponentDecomposition
{
    std::vector<DependencyComponent> components;

    // True only when there is more than one component and every one of them
    // was shown to be independent. The batching scheduler is not allowed to
    // run otherwise.
    bool batching_eligible { false };

    std::vector<std::string> notes;
    std::string              error;
};

// Whether two regions are far enough apart in xy to be considered separate.
bool regions_separated_in_xy(const PrintableRegion &a, const PrintableRegion &b,
                             double margin_mm);

// Break the print into components that do not depend on each other.
//
// Regions that touch, overlap, sit above one another, support, enclose or
// bridge to each other stay in one component. Separation is decided from
// geometry and declared dependencies, never from object identity or from the
// fact that two things merely look separate: a region whose geometry was never
// analyzed leaves its component unproven, and an unproven component blocks
// batching rather than being assumed safe.
ComponentDecomposition decompose_components(const std::vector<PrintableRegion> &regions,
                                            const std::vector<RegionLink>      &links,
                                            const ComponentOptions             &options);

} // namespace Slic3r

#endif // slic3r_DependencyComponents_hpp_
