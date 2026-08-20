///|/ Copyright (c) Prusa Research 2016 - 2025
///|/ Copyright (c) 2026 TigerSlicer contributors
///|/
///|/ PrusaSlicer and TigerSlicer are released under the terms of the AGPLv3 or higher
///|/

#include "MultiRateLayerSchedule.hpp"

#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>

namespace Slic3r {

namespace {

coordf_t quantize_z(coordf_t value, coordf_t resolution)
{
    return resolution > 0. ? std::round(value / resolution) * resolution : value;
}

std::string height_error(unsigned int tool_id, const std::string &detail)
{
    std::ostringstream out;
    out << "Tool T" << tool_id << ' ' << detail;
    return out.str();
}

} // namespace

const char *multi_rate_feature_name(MultiRateFeature feature)
{
    switch (feature) {
    case MultiRateFeature::Walls:            return "walls";
    case MultiRateFeature::SparseInfill:     return "sparse infill";
    case MultiRateFeature::SolidInfill:      return "solid infill";
    case MultiRateFeature::TopSurface:       return "top surfaces";
    case MultiRateFeature::Support:          return "supports";
    case MultiRateFeature::SupportInterface: return "support interfaces";
    case MultiRateFeature::Count:            break;
    }
    return "unknown feature";
}

coordf_t ToolLayerPolicy::target_height(coordf_t analysis_height) const
{
    // A profile-wide ratio describes this tool relative to a finer grid in an
    // interacting object. An isolated object is already sliced directly at the
    // tool's preferred height, so applying the ratio again would multiply the
    // native height (for example 0.40 mm into 1.60 mm for a 4:1 T3 profile).
    return requested_ratio > 0 && preferred_height > analysis_height + EPSILON ?
        coordf_t(requested_ratio) * analysis_height : preferred_height;
}

std::string ToolLayerPolicy::validate(coordf_t analysis_height, coordf_t resolution) const
{
    if (nozzle_diameter <= 0.)
        return height_error(tool_id, "has an invalid nozzle diameter.");
    if (minimum_height <= 0. || maximum_height + EPSILON < minimum_height)
        return height_error(tool_id, "has invalid minimum/maximum layer-height limits.");
    if (analysis_height <= 0.)
        return height_error(tool_id, "has an invalid analysis layer height.");
    if (requested_ratio < 0)
        return height_error(tool_id, "has a negative layer-height ratio.");

    const coordf_t target = target_height(analysis_height);
    if (target < minimum_height - EPSILON || target > maximum_height + EPSILON) {
        std::ostringstream detail;
        detail << "requests " << target << " mm layers outside its "
               << minimum_height << " to " << maximum_height << " mm range.";
        return height_error(tool_id, detail.str());
    }
    if (resolution > 0. && std::abs(target - quantize_z(target, resolution)) > EPSILON)
        return height_error(tool_id, "requests a layer height that is not a multiple of print resolution.");
    if (requested_ratio > 0 && preferred_height > 0. && std::abs(preferred_height - target) > EPSILON) {
        std::ostringstream detail;
        detail << "has a preferred height of " << preferred_height
               << " mm that disagrees with ratio " << requested_ratio
               << " on the " << analysis_height << " mm synchronization grid.";
        return height_error(tool_id, detail.str());
    }
    return {};
}

ToolLayerPolicy make_tool_layer_policy(const PrintConfig &config,
                                       unsigned int tool_id,
                                       coordf_t fallback_height)
{
    ToolLayerPolicy policy;
    policy.tool_id = tool_id;
    if (tool_id >= config.nozzle_diameter.size())
        return policy;

    policy.nozzle_diameter = config.nozzle_diameter.get_at(tool_id);
    const coordf_t configured_minimum = config.min_layer_height.get_at(tool_id);
    const coordf_t configured_maximum = config.max_layer_height.get_at(tool_id);
    policy.minimum_height = configured_minimum > 0. ? std::max(coordf_t(0.01), configured_minimum) : coordf_t(0.07);
    policy.maximum_height = configured_maximum > 0. ? std::max(policy.minimum_height, configured_maximum)
                                                     : std::max(policy.minimum_height, coordf_t(0.75) * policy.nozzle_diameter);

    const coordf_t configured_preferred = config.extruder_preferred_layer_height.get_at(tool_id);
    policy.preferred_height = configured_preferred > 0. ? configured_preferred : fallback_height;
    policy.requested_ratio = config.extruder_layer_height_ratio.get_at(tool_id);
    policy.permissions[size_t(MultiRateFeature::Walls)] = config.extruder_independent_walls.get_at(tool_id);
    policy.permissions[size_t(MultiRateFeature::SparseInfill)] = config.extruder_independent_infill.get_at(tool_id);
    policy.permissions[size_t(MultiRateFeature::SolidInfill)] = config.extruder_independent_solid_infill.get_at(tool_id);
    policy.permissions[size_t(MultiRateFeature::TopSurface)] = config.extruder_independent_top_surfaces.get_at(tool_id);
    policy.permissions[size_t(MultiRateFeature::Support)] = config.extruder_independent_support.get_at(tool_id);
    policy.permissions[size_t(MultiRateFeature::SupportInterface)] = config.extruder_independent_support_interface.get_at(tool_id);
    return policy;
}

std::vector<coordf_t> merge_multi_rate_planes(const std::vector<std::vector<coordf_t>> &sequences,
                                              coordf_t resolution)
{
    std::vector<coordf_t> merged;
    size_t count = 0;
    for (const std::vector<coordf_t> &sequence : sequences)
        count += sequence.size();
    merged.reserve(count);
    for (const std::vector<coordf_t> &sequence : sequences)
        for (coordf_t z : sequence)
            if (z >= 0.)
                merged.emplace_back(quantize_z(z, resolution));

    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end(), [](coordf_t lhs, coordf_t rhs) {
        return std::abs(lhs - rhs) <= EPSILON;
    }), merged.end());
    return merged;
}

std::vector<MultiRateRun> build_multi_rate_runs(const std::vector<MultiRatePlane> &planes,
                                                const ToolLayerPolicy &policy,
                                                coordf_t analysis_height,
                                                std::string *error)
{
    if (error)
        error->clear();
    std::vector<MultiRateRun> runs;
    if (planes.empty())
        return runs;

    const std::string policy_error = policy.validate(analysis_height, SCALING_FACTOR);
    if (! policy_error.empty()) {
        if (error)
            *error = policy_error;
        return {};
    }

    const coordf_t target = policy.target_height(analysis_height);
    size_t begin = 0;
    while (begin < planes.size()) {
        size_t end = begin;
        const coordf_t bottom = planes[begin].bottom_z;
        for (; end + 1 < planes.size(); ++ end) {
            const coordf_t next_height = planes[end + 1].print_z - bottom;
            if (planes[end].forced_sync || next_height > target + EPSILON)
                break;
        }

        const coordf_t run_height = planes[end].print_z - bottom;
        if (run_height < policy.minimum_height - EPSILON || run_height > policy.maximum_height + EPSILON) {
            if (error) {
                std::ostringstream detail;
                detail << "would create a " << run_height << " mm remainder layer outside its "
                       << policy.minimum_height << " to " << policy.maximum_height << " mm range.";
                *error = height_error(policy.tool_id, detail.str());
            }
            return {};
        }
        runs.push_back({ begin, end, bottom, planes[end].print_z });
        begin = end + 1;
    }
    return runs;
}

size_t MultiRateSchedule::add_operation(MultiRateOperation operation)
{
    operation.id = m_operations.size();
    m_operations.emplace_back(std::move(operation));
    return m_operations.back().id;
}

bool MultiRateSchedule::add_dependency(size_t predecessor,
                                       size_t successor,
                                       MultiRateDependencyReason reason,
                                       std::string *error)
{
    if (predecessor >= m_operations.size() || successor >= m_operations.size() || predecessor == successor) {
        if (error)
            *error = "Invalid multi-rate dependency endpoint.";
        return false;
    }
    if (m_operations[predecessor].print_z > m_operations[successor].print_z + EPSILON) {
        if (error)
            *error = "A multi-rate dependency would require printing at a lower Z after a higher operation.";
        return false;
    }
    const auto duplicate = std::find_if(m_dependencies.begin(), m_dependencies.end(), [predecessor, successor](const MultiRateDependency &edge) {
        return edge.predecessor == predecessor && edge.successor == successor;
    });
    if (duplicate == m_dependencies.end())
        m_dependencies.push_back({ predecessor, successor, reason });
    return true;
}

void MultiRateSchedule::add_monotonic_z_dependencies()
{
    std::vector<size_t> ids(m_operations.size());
    std::iota(ids.begin(), ids.end(), size_t(0));
    std::stable_sort(ids.begin(), ids.end(), [this](size_t lhs, size_t rhs) {
        const MultiRateOperation &a = m_operations[lhs];
        const MultiRateOperation &b = m_operations[rhs];
        return a.print_z < b.print_z - EPSILON || (std::abs(a.print_z - b.print_z) <= EPSILON && a.id < b.id);
    });

    size_t plane_begin = 0;
    while (plane_begin < ids.size()) {
        size_t plane_end = plane_begin + 1;
        while (plane_end < ids.size() && std::abs(m_operations[ids[plane_end]].print_z - m_operations[ids[plane_begin]].print_z) <= EPSILON)
            ++ plane_end;
        size_t next_begin = plane_end;
        size_t next_end = next_begin;
        while (next_end < ids.size() && std::abs(m_operations[ids[next_end]].print_z - m_operations[ids[next_begin]].print_z) <= EPSILON)
            ++ next_end;
        if (next_begin < ids.size())
            for (size_t i = plane_begin; i < plane_end; ++ i)
                for (size_t j = next_begin; j < next_end; ++ j)
                    add_dependency(ids[i], ids[j], MultiRateDependencyReason::MonotonicZ);
        plane_begin = plane_end;
    }
}

std::vector<size_t> MultiRateSchedule::ordered_operation_ids(std::string *error) const
{
    if (error)
        error->clear();
    const size_t count = m_operations.size();
    std::vector<size_t> indegree(count, 0);
    std::vector<std::vector<size_t>> outgoing(count);
    for (const MultiRateDependency &edge : m_dependencies) {
        if (edge.predecessor >= count || edge.successor >= count) {
            if (error)
                *error = "The multi-rate dependency graph contains an invalid endpoint.";
            return {};
        }
        ++ indegree[edge.successor];
        outgoing[edge.predecessor].push_back(edge.successor);
    }

    std::vector<size_t> ready;
    for (size_t id = 0; id < count; ++ id)
        if (indegree[id] == 0)
            ready.push_back(id);

    std::vector<size_t> ordered;
    ordered.reserve(count);
    unsigned int current_tool = std::numeric_limits<unsigned int>::max();
    while (! ready.empty()) {
        const auto minimum_z_it = std::min_element(ready.begin(), ready.end(), [this](size_t lhs, size_t rhs) {
            return m_operations[lhs].print_z < m_operations[rhs].print_z;
        });
        const coordf_t minimum_z = m_operations[*minimum_z_it].print_z;

        auto better = [this, current_tool, minimum_z](size_t lhs, size_t rhs) {
            const MultiRateOperation &a = m_operations[lhs];
            const MultiRateOperation &b = m_operations[rhs];
            const bool a_minimum_z = std::abs(a.print_z - minimum_z) <= EPSILON;
            const bool b_minimum_z = std::abs(b.print_z - minimum_z) <= EPSILON;
            if (a_minimum_z != b_minimum_z)
                return a_minimum_z;
            const bool a_same_tool = a.tool_id == current_tool;
            const bool b_same_tool = b.tool_id == current_tool;
            if (a_same_tool != b_same_tool)
                return a_same_tool;
            return a.id < b.id;
        };
        const auto chosen_it = std::min_element(ready.begin(), ready.end(), [&better](size_t lhs, size_t rhs) {
            return better(lhs, rhs);
        });
        const size_t chosen = *chosen_it;
        ready.erase(chosen_it);
        ordered.push_back(chosen);
        current_tool = m_operations[chosen].tool_id;
        for (size_t successor : outgoing[chosen])
            if (-- indegree[successor] == 0)
                ready.push_back(successor);
    }

    if (ordered.size() != count) {
        if (error)
            *error = "The multi-rate dependency graph contains a cycle.";
        return {};
    }
    return ordered;
}

std::string MultiRateSchedule::validate(const std::vector<ToolLayerPolicy> &policies,
                                        coordf_t resolution) const
{
    for (const MultiRateOperation &operation : m_operations) {
        if (operation.tool_id >= policies.size())
            return "A multi-rate operation refers to an unavailable physical tool.";
        if (operation.bottom_z < -EPSILON || operation.print_z <= operation.bottom_z + EPSILON)
            return "A multi-rate operation has an invalid occupied Z slab.";
        if (std::abs(operation.extrusion_height - (operation.print_z - operation.bottom_z)) > EPSILON)
            return "A multi-rate operation's extrusion height does not match its occupied Z slab.";
        const ToolLayerPolicy &policy = policies[operation.tool_id];
        if (operation.extrusion_height < policy.minimum_height - EPSILON ||
            operation.extrusion_height > policy.maximum_height + EPSILON)
            return height_error(operation.tool_id, "has an operation outside its layer-height limits.");
        if (resolution > 0. && std::abs(operation.print_z - quantize_z(operation.print_z, resolution)) > EPSILON)
            return "A multi-rate operation plane is not aligned to print resolution.";
    }

    for (const MultiRateDependency &edge : m_dependencies) {
        if (edge.predecessor >= m_operations.size() || edge.successor >= m_operations.size())
            return "The multi-rate dependency graph contains an invalid endpoint.";
        if (m_operations[edge.predecessor].print_z > m_operations[edge.successor].print_z + EPSILON)
            return "The multi-rate dependency graph contains an unsafe downward-Z edge.";
    }

    std::string order_error;
    const std::vector<size_t> order = ordered_operation_ids(&order_error);
    if (! order_error.empty())
        return order_error;
    coordf_t previous_z = -std::numeric_limits<coordf_t>::max();
    for (size_t id : order) {
        if (m_operations[id].print_z + EPSILON < previous_z)
            return "The multi-rate operation order would move downward in Z.";
        previous_z = m_operations[id].print_z;
    }
    return {};
}

} // namespace Slic3r
