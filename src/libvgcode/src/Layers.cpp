///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "Layers.hpp"

#include "../include/PathVertex.hpp"
#include "Utils.hpp"

#include <assert.h>
#include <algorithm>

namespace libvgcode {

static bool is_colorprint_option(const PathVertex& v)
{
    return v.type == EMoveType::PausePrint || v.type == EMoveType::CustomGCode;
}

// TigerSlicer: the layer's z is the z of its last extrusion, but inner walls only
// count until something else is extruded. Brick layers print every other inner
// wall half a layer higher, last on the layer, and must not move the layer's z.
static void update_layer_z(float& z, bool& z_from_inner_wall, const PathVertex& vertex)
{
    if (vertex.type != EMoveType::Extrude || vertex.role == EGCodeExtrusionRole::Custom)
        return;
    const bool is_inner_wall = vertex.role == EGCodeExtrusionRole::Perimeter;
    if (is_inner_wall && !z_from_inner_wall)
        return;
    z = vertex.position[2];
    z_from_inner_wall = is_inner_wall;
}

void Layers::update(const PathVertex& vertex, uint32_t vertex_id)
{
    if (m_items.empty() || vertex.layer_id == m_items.size()) {
        // this code assumes that gcode paths are sent sequentially, one layer after the other
        assert(vertex.layer_id == static_cast<uint32_t>(m_items.size()));
        Item& item = m_items.emplace_back(Item());
        update_layer_z(item.z, item.z_from_inner_wall, vertex);
        item.range.set(vertex_id, vertex_id);
        item.times = vertex.times;
        item.contains_colorprint_options |= is_colorprint_option(vertex);
    }
    else {
        Item& item = m_items.back();
        update_layer_z(item.z, item.z_from_inner_wall, vertex);
        item.range.set_max(vertex_id);
        for (size_t i = 0; i < TIME_MODES_COUNT; ++i) {
            item.times[i] += vertex.times[i];
        }
        item.contains_colorprint_options |= is_colorprint_option(vertex);
    }
}

void Layers::reset()
{
    m_items.clear();
    m_view_range.reset();
}

std::vector<float> Layers::get_times(ETimeMode mode) const
{
    std::vector<float> ret;
    if (mode < ETimeMode::COUNT) {
        for (const Item& item : m_items) {
            ret.emplace_back(item.times[static_cast<size_t>(mode)]);
        }
    }
    return ret;
}

std::vector<float> Layers::get_zs() const
{
    std::vector<float> ret;
    ret.reserve(m_items.size());
    for (const Item& item : m_items) {
        ret.emplace_back(item.z);
    }
    return ret;
}

size_t Layers::get_layer_id_at(float z) const
{
    auto iter = std::upper_bound(m_items.begin(), m_items.end(), z, [](float z, const Item& item) { return item.z < z; });
    return std::distance(m_items.begin(), iter);
}

size_t Layers::size_in_bytes_cpu() const
{
    size_t ret = STDVEC_MEMSIZE(m_items, Item);
    return ret;
}

} // namespace libvgcode
