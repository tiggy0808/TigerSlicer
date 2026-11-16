#include "NonPlanar.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Feature/SmallFeatures/SmallFeatures.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SLA/IndexedMesh.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace Slic3r {

TriangleMesh NonPlanar::object_mesh(const PrintObject &object, double &ground_level)
{
    TriangleMesh             mesh  = object.model_object()->raw_mesh();
    Geometry::Transformation trans = object.model_object()->instances.front()->get_transformation();
    trans.set_offset(Vec3d(-unscale<double>(object.center_offset().x()), -unscale<double>(object.center_offset().y()), 0));
    // A mirrored instance keeps its facets facing out, so that a top facet faces up.
    mesh.transform(trans.get_matrix(), true);
    ground_level = mesh.bounding_box().min.z() - object.model_object()->min_z();
    return mesh;
}

namespace {

// A gently sloped facet of the object's top (mesh coordinates).
struct TopFacet
{
    Vec3d       corners[3];
    Point       middle;
    BoundingBox box;
};

} // namespace

static Point scaled_xy(const Vec3d &p) { return Point(scaled<coord_t>(p.x()), scaled<coord_t>(p.y())); }

// The part of the triangle at or above `z`, seen from above (scaled).
static Polygon clip_above(const Vec3d (&corners)[3], double z)
{
    Polygon out;
    for (int i = 0; i < 3; ++i) {
        const Vec3d &p = corners[i], &q = corners[(i + 1) % 3];
        const bool   p_above = p.z() >= z, q_above = q.z() >= z;
        if (p_above)
            out.points.push_back(scaled_xy(p));
        if (p_above != q_above)
            out.points.push_back(scaled_xy(p + (q - p) * ((z - p.z()) / (q.z() - p.z()))));
    }
    return out;
}

// The height of the object's top at `at` (mesh Z), from a ray cast down from `above`; NaN where it misses.
static double top_at(const sla::IndexedMesh &mesh, const Vec2d &at, double above)
{
    const sla::IndexedMesh::hit_result hit = mesh.query_ray_hit(Vec3d(at.x(), at.y(), above), -Vec3d::UnitZ());
    return hit.is_hit() ? hit.position().z() : std::numeric_limits<double>::quiet_NaN();
}

static bool contains(const ExPolygons &area, const Point &p)
{
    return std::any_of(area.begin(), area.end(), [&p](const ExPolygon &piece) { return piece.contains(p); });
}

// Whether the object is at least `depth` thick under its top everywhere in `footprint`, sampled every millimetre: the
// flat layers under the curved ones need something to stand on.
static bool solid_under(const ExPolygons &footprint, const sla::IndexedMesh &mesh, double above, double depth)
{
    const BoundingBox box  = get_extents(footprint);
    const coord_t     step = scaled<coord_t>(1.);
    for (coord_t y = box.min.y() + step / 2; y < box.max.y(); y += step)
        for (coord_t x = box.min.x() + step / 2; x < box.max.x(); x += step) {
            const Point p(x, y);
            if (!contains(footprint, p))
                continue;
            const Vec2d  at  = unscale(p);
            const double top = top_at(mesh, at, above);
            if (std::isnan(top))
                continue;
            const sla::IndexedMesh::hit_result bottom = mesh.query_ray_hit(Vec3d(at.x(), at.y(), top - 1e-3), -Vec3d::UnitZ());
            if (bottom.is_hit() && top - bottom.position().z() < depth)
                return false;
        }
    return true;
}

void PrintObject::find_non_planar_zones()
{
    m_non_planar_zones.clear();
    std::vector<size_t> regions;
    for (size_t id = 0; id < this->num_printing_regions(); ++id)
        if (this->printing_region(id).config().non_planar_mode.value == npmOn)
            regions.push_back(id);
    if (regions.empty() || m_layers.size() < 2 || m_print->config().spiral_mode.value)
        return;

    double                 ground = 0.;
    const TriangleMesh     mesh   = NonPlanar::object_mesh(*this, ground);
    const sla::IndexedMesh imesh(mesh);
    const double           above        = mesh.bounding_box().max.z() + 1.;
    const double           z_min_print  = m_slicing_params.object_print_z_min;
    const double           first_top    = m_layers.front()->print_z - z_min_print;
    const double           layer_height = m_config.layer_height.value;

    for (const size_t region : regions) {
        const PrintRegionConfig &config    = this->printing_region(region).config();
        // As many curved layers as flat top layers, top_shell_thickness included.
        int shells = config.top_shell_layers.value;
        if (shells > 0 && config.top_shell_thickness.value > 0.)
            shells = std::max(shells, int(std::ceil(config.top_shell_thickness.value / layer_height - EPSILON)));
        const double thickness = shells * layer_height;
        // How tall a curve the nozzle can reach over: its clearance, less the curved layers' own thickness.
        const double reach = config.non_planar_max_height.value - thickness;
        if (shells <= 0 || reach < layer_height)
            continue;
        const double cos_max = std::cos(Geometry::deg2rad(config.non_planar_max_angle.value));

        // The gently sloped facets of the object's top: facing up, nothing above them.
        std::vector<TopFacet> facets;
        Polygons              projected;
        for (const stl_triangle_vertex_indices &f : mesh.its.indices) {
            TopFacet facet;
            for (int i = 0; i < 3; ++i)
                facet.corners[i] = mesh.its.vertices[f(i)].cast<double>();
            const Vec3d normal = (facet.corners[1] - facet.corners[0]).cross(facet.corners[2] - facet.corners[0]);
            if (normal.norm() <= 0. || normal.z() < cos_max * normal.norm())
                continue;
            const Vec3d middle = (facet.corners[0] + facet.corners[1] + facet.corners[2]) / 3.;
            if (imesh.query_ray_hit(middle + Vec3d(0., 0., 1e-3), Vec3d::UnitZ()).is_hit())
                continue;
            projected.push_back(Polygon({scaled_xy(facet.corners[0]), scaled_xy(facet.corners[1]), scaled_xy(facet.corners[2])}));
            facet.middle = scaled_xy(middle);
            facet.box    = get_extents(projected.back());
            facets.push_back(facet);
        }
        if (facets.empty())
            continue;

        // Each island of them is a zone, if it rises far enough to need curved layers.
        for (const ExPolygon &island : union_safety_offset_ex(projected)) {
            const BoundingBox             island_box = get_extents(island);
            std::vector<const TopFacet *> mine;
            double                        mesh_top = -std::numeric_limits<double>::max(), mesh_low = std::numeric_limits<double>::max();
            for (const TopFacet &facet : facets)
                if (island_box.contains(facet.middle) && island.contains(facet.middle)) {
                    mine.push_back(&facet);
                    for (const Vec3d &corner : facet.corners) {
                        mesh_top = std::max(mesh_top, corner.z());
                        mesh_low = std::min(mesh_low, corner.z());
                    }
                }
            if (mine.empty())
                continue;
            const double top = mesh_top - ground;
            // Not lower than the nozzle can reach, nor into the first layer.
            const double bottom = std::max({mesh_low - ground, top - reach, first_top + thickness});
            // Z contouring follows a rise within one layer.
            if (top - bottom < layer_height)
                continue;

            Polygons clipped;
            for (const TopFacet *facet : mine)
                if (Polygon part = clip_above(facet->corners, bottom + ground); part.size() >= 3)
                    clipped.push_back(std::move(part));
            ExPolygons footprint = union_safety_offset_ex(clipped);
            footprint.erase(std::remove_if(footprint.begin(), footprint.end(),
                                           [](const ExPolygon &piece) { return piece.area() < sqr(scaled<double>(1.)); }),
                            footprint.end());
            if (footprint.empty() || !solid_under(footprint, imesh, above, thickness + layer_height))
                continue;

            // The first layer that reaches the zone's top prints the curved layers.
            auto first_reaching = [this, z_min_print](double z) {
                for (size_t i = 0; i < m_layers.size(); ++i)
                    if (m_layers[i]->print_z - z_min_print >= z - EPSILON)
                        return i;
                return m_layers.size() - 1;
            };
            const size_t layer = first_reaching(top);
            // The zone takes the settings of the region it lies in, halfway up.
            const ExPolygon &largest = *std::max_element(footprint.begin(), footprint.end(),
                                                         [](const ExPolygon &a, const ExPolygon &b) { return a.area() < b.area(); });
            const LayerRegionPtrs &halfway = m_layers[first_reaching(0.5 * (bottom + top))]->regions();
            const LayerRegion     *in      = SmallFeatures::region_of(halfway, largest);
            if (in == nullptr || size_t(std::find(halfway.begin(), halfway.end(), in) - halfway.begin()) != region)
                continue;
            // And only where that region is, under the lowest curved layer: not over another region, nor over a hole a
            // negative volume cuts.
            size_t base = 0;
            for (size_t i = 0; i < m_layers.size(); ++i)
                if (m_layers[i]->print_z - z_min_print <= bottom - thickness + EPSILON)
                    base = i;
            footprint = intersection_ex(footprint, to_expolygons(m_layers[base]->get_region(int(region))->slices.surfaces));
            footprint.erase(std::remove_if(footprint.begin(), footprint.end(),
                                           [](const ExPolygon &piece) { return piece.area() < sqr(scaled<double>(1.)); }),
                            footprint.end());
            if (footprint.empty())
                continue;
            m_non_planar_zones.push_back({std::move(footprint), bottom, top, layer_height, shells, region, layer});
        }
    }

    // The flat layers stop a zone's thickness under its surface: from each layer in a zone, cut away the part of the
    // zone where the object's top is less than the thickness above the layer's top.
    for (const NonPlanar::Zone &zone : m_non_planar_zones) {
        std::vector<size_t> trimmed;
        std::vector<float>  zs;
        for (size_t i = 0; i < m_layers.size(); ++i) {
            const double top = m_layers[i]->print_z - z_min_print;
            if (top > zone.bottom - zone.thickness() + EPSILON && top - m_layers[i]->height < zone.top) {
                trimmed.push_back(i);
                zs.push_back(float(top + zone.thickness() + ground - 1e-3));
            }
        }
        if (trimmed.empty())
            continue;
        const std::vector<ExPolygons> sections = slice_mesh_ex(mesh.its, zs, MeshSlicingParamsEx{});
        for (size_t k = 0; k < trimmed.size(); ++k) {
            const ExPolygons cut = diff_ex(zone.footprint, sections[k]);
            if (cut.empty())
                continue;
            Layer *layer = m_layers[trimmed[k]];
            for (LayerRegion *layerm : layer->regions()) {
                Surfaces kept;
                for (const Surface &surface : layerm->slices.surfaces)
                    for (ExPolygon &piece : diff_ex(ExPolygons{surface.expolygon}, cut))
                        kept.emplace_back(surface, std::move(piece));
                layerm->slices.set(std::move(kept));
            }
            layer->make_slices();
        }
    }
}

void PrintObject::cover_non_planar_tops()
{
    // The tops the flat layers leave under the curved ones are covered: solid, but not top surfaces. That includes the
    // layer the lowest curved one lies on, whose top is the zone's thickness under its bottom.
    const double z_min_print = m_slicing_params.object_print_z_min;
    for (const NonPlanar::Zone &zone : m_non_planar_zones)
        for (Layer *layer : m_layers) {
            const double top = layer->print_z - z_min_print;
            if (top < zone.bottom - zone.thickness() - EPSILON || top - layer->height >= zone.top)
                continue;
            for (LayerRegion *layerm : layer->regions())
                for (SurfaceCollection *surfaces : {&layerm->slices, &layerm->fill_surfaces}) {
                    Surfaces out;
                    for (const Surface &surface : surfaces->surfaces) {
                        if (surface.surface_type != stTop) {
                            out.push_back(surface);
                            continue;
                        }
                        for (ExPolygon &piece : intersection_ex(ExPolygons{surface.expolygon}, zone.footprint)) {
                            out.emplace_back(surface, std::move(piece));
                            out.back().surface_type = stInternalSolid;
                        }
                        for (ExPolygon &piece : diff_ex(ExPolygons{surface.expolygon}, zone.footprint))
                            out.emplace_back(surface, std::move(piece));
                    }
                    surfaces->set(std::move(out));
                }
        }
}

// `line` on a curved layer: every 0.2 mm, `below` under the object's top there, as offsets from `print_z`. A point where
// the ray misses the top keeps the last height. With `flat_tops` (the flat layers' tops, object Z, ascending), the path
// lies on the flat layers, and each point's flow fills the step from their top to the curved layer's bottom, `thickness`
// under the object's top.
static ExtrusionPath curved_path(const Polyline &line, ExtrusionRole role, const Flow &flow, const sla::IndexedMesh &mesh, double above,
                                 double below, double print_z, double z_min_print, const std::vector<double> *flat_tops, double thickness)
{
    Points3            points;
    std::vector<float> point_flow;
    double             last_top = std::numeric_limits<double>::quiet_NaN();
    auto               add      = [&](const Vec2d &at) {
        double top = top_at(mesh, at, above);
        if (std::isnan(top))
            top = last_top;
        if (std::isnan(top))
            return;
        last_top = top;
        const double object_top = top - mesh.ground_level();
        const Point3 p(scaled<coord_t>(at.x()), scaled<coord_t>(at.y()), scaled<coord_t>(object_top + z_min_print - below - print_z));
        float        ratio = 1.f;
        if (flat_tops != nullptr) {
            // The highest flat layer under the curved one, and the step from its top up to the curved layer's bottom.
            const double bottom = object_top - thickness;
            auto         it     = std::upper_bound(flat_tops->begin(), flat_tops->end(), bottom + 1e-3);
            if (it != flat_tops->begin())
                ratio = float((flow.height() + std::clamp(bottom - *std::prev(it), 0., double(flow.height()))) / flow.height());
        }
        // A point in line with the last two, at the same flow, replaces the last.
        if (points.size() >= 2 && std::abs(point_flow.back() - ratio) < 0.02f) {
            const Vec3d a = points[points.size() - 2].cast<double>(), b = points.back().cast<double>(), c = p.cast<double>();
            const Vec3d ac = c - a;
            if (ac.norm() > 0. && (b - a).cross(ac).norm() / ac.norm() < scaled<double>(0.001)) {
                points.back()     = p;
                point_flow.back() = ratio;
                return;
            }
        }
        points.push_back(p);
        point_flow.push_back(ratio);
    };
    const double step = scaled<double>(0.2);
    for (size_t i = 0; i < line.points.size(); ++i) {
        const Vec2d b = line.points[i].cast<double>();
        if (i > 0) {
            const Vec2d a     = line.points[i - 1].cast<double>();
            const int   steps = std::max(1, int(std::ceil((b - a).norm() / step)));
            for (int s = 1; s < steps; ++s)
                add((a + (b - a) * (double(s) / steps)) * SCALING_FACTOR);
        }
        add(b * SCALING_FACTOR);
    }
    ExtrusionPath path(role, flow.mm3_per_mm(), flow.width(), flow.height());
    path.polyline    = Polyline3(points);
    path.z_contoured = true;
    path.non_planar  = true;
    if (flat_tops != nullptr)
        path.point_flow = std::move(point_flow);
    return path;
}

void PrintObject::add_non_planar_shells(const sla::IndexedMesh &mesh, double above)
{
    const double        z_min_print = m_slicing_params.object_print_z_min;
    std::vector<double> flat_tops;
    for (const Layer *layer : m_layers)
        flat_tops.push_back(layer->print_z - z_min_print);
    for (const NonPlanar::Zone &zone : m_non_planar_zones) {
        Layer       *layer  = m_layers[zone.layer];
        LayerRegion *layerm = layer->get_region(int(zone.region));
        // Bottom up, each shell a layer height above the last, the top one on the surface. One collection in that order: the
        // G-code writer prints a fill collection's paths, but not collections in it, and must not reorder the shells.
        ExtrusionEntityCollection shells;
        shells.no_sort = true;
        for (int j = 0; j < zone.shells; ++j) {
            const bool   top   = j + 1 == zone.shells;
            const double below = (zone.shells - 1 - j) * zone.layer_height;
            const Flow   wall  = layerm->flow(frExternalPerimeter).with_height(float(zone.layer_height));
            const Flow   fill  = layerm->flow(top ? frTopSolidInfill : frSolidInfill).with_height(float(zone.layer_height));
            // The lowest lies on the flat layers.
            const std::vector<double> *on_flat = j == 0 ? &flat_tops : nullptr;
            auto add = [&](const Polyline &line, const Flow &flow) {
                ExtrusionPath path = curved_path(line, top ? erTopSolidInfill : erSolidInfill, flow, mesh, above, below, layer->print_z,
                                                 z_min_print, on_flat, zone.thickness());
                if (path.polyline.points.size() >= 2)
                    shells.append(std::move(path));
            };

            // A loop around the zone, then lines across it, crossing the last shell's. All one role: the G-code writer
            // picks one extruder for a fill collection.
            for (const ExPolygon &piece : offset_ex(zone.footprint, -0.5f * float(wall.scaled_width())))
                for (const Polygon &loop : to_polygons(piece))
                    add(loop.split_at_first_point(), wall);
            std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
            filler->spacing = fill.spacing();
            filler->angle   = float(M_PI / 4. + (j % 2) * M_PI / 2.);
            FillParams params;
            params.density     = 1.f;
            params.dont_adjust = false;
            for (const ExPolygon &piece : offset_ex(zone.footprint, -float(wall.scaled_width()) + 0.15f * float(fill.scaled_spacing()))) {
                Surface surface(stTop, piece);
                filler->bounding_box = get_extents(piece);
                for (const Polyline &line : filler->fill_surface(&surface, params))
                    add(line, fill);
            }
        }
        if (!shells.empty())
            layerm->fills.append(std::move(shells));
    }
}

} // namespace Slic3r
