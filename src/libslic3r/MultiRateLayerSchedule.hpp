///|/ Copyright (c) Prusa Research 2016 - 2025
///|/ Copyright (c) 2026 TigerSlicer contributors
///|/
///|/ PrusaSlicer and TigerSlicer are released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_MultiRateLayerSchedule_hpp_
#define slic3r_MultiRateLayerSchedule_hpp_

#include "libslic3r.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class PrintConfig;

enum class MultiRateFeature : uint8_t {
    Walls = 0,
    SparseInfill,
    SolidInfill,
    TopSurface,
    Support,
    SupportInterface,
    Count
};

const char *multi_rate_feature_name(MultiRateFeature feature);

struct ToolLayerPolicy
{
    unsigned int tool_id { 0 };
    coordf_t nozzle_diameter { 0. };
    coordf_t minimum_height { 0. };
    coordf_t maximum_height { 0. };
    coordf_t preferred_height { 0. };
    int       requested_ratio { 0 };
    std::array<bool, size_t(MultiRateFeature::Count)> permissions {};

    bool allows(MultiRateFeature feature) const { return permissions[size_t(feature)]; }

    // A non-zero ratio is an explicit request relative to the object's analysis
    // grid. Otherwise the resolved preferred height is used.
    coordf_t target_height(coordf_t analysis_height) const;

    // Returns an empty string when this policy is printable on analysis_height.
    std::string validate(coordf_t analysis_height, coordf_t resolution) const;
};

ToolLayerPolicy make_tool_layer_policy(const PrintConfig &config,
                                       unsigned int tool_id,
                                       coordf_t fallback_height);

struct MultiRatePlane
{
    coordf_t bottom_z { 0. };
    coordf_t print_z { 0. };
    bool forced_sync { false };

    coordf_t height() const { return print_z - bottom_z; }
};

// Merge tool/object plane sequences into one strictly increasing sequence.
// Numerically equivalent values are quantized to resolution and deduplicated.
std::vector<coordf_t> merge_multi_rate_planes(const std::vector<std::vector<coordf_t>> &sequences,
                                              coordf_t resolution);

struct MultiRateRun
{
    size_t begin_plane { 0 };
    size_t end_plane { 0 };
    coordf_t bottom_z { 0. };
    coordf_t print_z { 0. };

    coordf_t height() const { return print_z - bottom_z; }
};

// Partition an analysis grid into the longest legal coarse runs. A forced plane
// is always the end of a run. A short remainder is retained only if it is inside
// the tool's configured range; otherwise an error is returned.
std::vector<MultiRateRun> build_multi_rate_runs(const std::vector<MultiRatePlane> &planes,
                                                const ToolLayerPolicy &policy,
                                                coordf_t analysis_height,
                                                std::string *error);

enum class MultiRateDependencyReason : uint8_t {
    MonotonicZ,
    VerticalSupport,
    MaterialInterface,
    Enclosure,
    SupportContact,
    CollisionClearance
};

struct MultiRateOperation
{
    size_t id { 0 };
    size_t object_id { 0 };
    size_t region_id { 0 };
    unsigned int tool_id { 0 };
    MultiRateFeature feature { MultiRateFeature::Walls };
    coordf_t bottom_z { 0. };
    coordf_t print_z { 0. };
    coordf_t extrusion_height { 0. };
    size_t source_begin_plane { 0 };
    size_t source_end_plane { 0 };
};

struct MultiRateDependency
{
    size_t predecessor { 0 };
    size_t successor { 0 };
    MultiRateDependencyReason reason { MultiRateDependencyReason::MonotonicZ };
};

class MultiRateSchedule
{
public:
    void clear() { m_operations.clear(); m_dependencies.clear(); }
    size_t add_operation(MultiRateOperation operation);
    bool add_dependency(size_t predecessor,
                        size_t successor,
                        MultiRateDependencyReason reason,
                        std::string *error = nullptr);

    // Adds conservative increasing-Z edges between adjacent occupied planes.
    void add_monotonic_z_dependencies();

    // Deterministic Kahn ordering. Ready operations at the same Z prefer the
    // current tool, which reduces changes without weakening dependencies.
    std::vector<size_t> ordered_operation_ids(std::string *error = nullptr) const;

    // Validate heights, edge direction, acyclicity and non-decreasing order.
    std::string validate(const std::vector<ToolLayerPolicy> &policies,
                         coordf_t resolution) const;

    const std::vector<MultiRateOperation> &operations() const { return m_operations; }
    const std::vector<MultiRateDependency> &dependencies() const { return m_dependencies; }

private:
    std::vector<MultiRateOperation>  m_operations;
    std::vector<MultiRateDependency> m_dependencies;
};

} // namespace Slic3r

#endif // slic3r_MultiRateLayerSchedule_hpp_
