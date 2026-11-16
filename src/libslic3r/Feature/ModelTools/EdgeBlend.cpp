#include "EdgeBlend.hpp"

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace Slic3r::ModelTools {

namespace {

// How far the blend solid reaches past the faces (into air for a cut, into material for a fill), so
// that it does not share a face with the model.
constexpr double kReach = 0.2;
// How far a cut reaches past the ends of an open chain, for the same reason. A fill stops at the ends:
// past them it would stick out of the model.
constexpr double kEndReach = 0.05;
// Arc segments of a fillet's profile.
constexpr int kArcSegments = 20;
// The faces of a blended edge must meet between these (degrees between the faces' surfaces).
constexpr double kMinFaceAngle = 20.;
constexpr double kMaxFaceAngle = 160.;
// A blend may reach this share of the way to the centre of a curved edge, no further.
constexpr double kMaxCurveReach = 0.9;
// A chain ends where it turns by more than this (cosine of the angle between the two edges at the vertex).
const double kMaxTurnCos = std::cos(M_PI - M_PI / 4.);

std::optional<Vec3d> face_normal(const indexed_triangle_set &its, int face)
{
    const stl_triangle_vertex_indices &f = its.indices[face];
    const Vec3d a = its.vertices[f[0]].cast<double>();
    const Vec3d n = (its.vertices[f[1]].cast<double>() - a).cross(its.vertices[f[2]].cast<double>() - a);
    const double length = n.norm();
    if (!(length > 1e-12))
        return std::nullopt;
    return n / length;
}

Vec3d face_centroid(const indexed_triangle_set &its, int face)
{
    const stl_triangle_vertex_indices &f = its.indices[face];
    return (its.vertices[f[0]] + its.vertices[f[1]] + its.vertices[f[2]]).cast<double>() / 3.;
}

struct SharpEdge
{
    int  a, b;        // face `left` holds the half-edge a -> b
    int  left, right;
    bool convex;
    bool used = false;
};

} // namespace

std::vector<EdgeChain> sharp_edges(const indexed_triangle_set &its, double min_angle)
{
    if (its.indices.empty())
        return {};
    const std::vector<Vec3i32> neighbors = its_face_neighbors(its);
    std::vector<std::optional<Vec3d>> normals(its.indices.size());
    for (size_t f = 0; f < its.indices.size(); ++f)
        normals[f] = face_normal(its, int(f));

    const double         max_cos = std::cos(min_angle * M_PI / 180.);
    std::vector<SharpEdge> edges;
    std::vector<std::vector<int>> vertex_edges(its.vertices.size());
    for (int f = 0; f < int(its.indices.size()); ++f) {
        if (!normals[f])
            continue;
        for (int j = 0; j < 3; ++j) {
            const int g = neighbors[f][j];
            // Each edge once, from its lower face; skip open edges and degenerate faces.
            if (g <= f || !normals[g] || normals[f]->dot(*normals[g]) > max_cos)
                continue;
            const Vec2i32 e      = its_triangle_edge(its.indices[f], j);
            const bool    convex = normals[f]->dot(face_centroid(its, g) - its.vertices[e[0]].cast<double>()) < 0.;
            vertex_edges[e[0]].push_back(int(edges.size()));
            vertex_edges[e[1]].push_back(int(edges.size()));
            edges.push_back({e[0], e[1], f, g, convex});
        }
    }

    auto other_end = [&edges](int edge, int vertex) { return edges[edge].a == vertex ? edges[edge].b : edges[edge].a; };
    std::vector<bool> is_break(its.vertices.size(), false);
    for (size_t v = 0; v < its.vertices.size(); ++v) {
        const std::vector<int> &adj = vertex_edges[v];
        if (adj.empty())
            continue;
        if (adj.size() != 2 || edges[adj[0]].convex != edges[adj[1]].convex) {
            is_break[v] = true;
            continue;
        }
        const Vec3d p  = its.vertices[v].cast<double>();
        const Vec3d d1 = (its.vertices[other_end(adj[0], int(v))].cast<double>() - p).normalized();
        const Vec3d d2 = (its.vertices[other_end(adj[1], int(v))].cast<double>() - p).normalized();
        // Straight on, d1 and d2 point opposite ways.
        is_break[v] = d1.dot(d2) > kMaxTurnCos;
    }

    std::vector<EdgeChain> chains;
    auto walk = [&](int start, int first_edge) {
        EdgeChain chain;
        chain.convex = edges[first_edge].convex;
        chain.vertices.push_back(start);
        int v = start, e = first_edge;
        for (;;) {
            SharpEdge &se      = edges[e];
            se.used            = true;
            const bool forward = se.a == v;
            const int  next    = forward ? se.b : se.a;
            chain.face_left.push_back(forward ? se.left : se.right);
            chain.face_right.push_back(forward ? se.right : se.left);
            if (next == start && !is_break[start]) {
                chain.closed = true;
                break;
            }
            chain.vertices.push_back(next);
            if (is_break[next])
                break;
            const std::vector<int> &adj = vertex_edges[next];
            const int following         = adj[0] == e ? adj[1] : adj[0];
            if (edges[following].used)
                break;
            v = next;
            e = following;
        }
        chains.push_back(std::move(chain));
    };
    // Open chains run between break vertices; whatever is left is closed loops.
    for (int v = 0; v < int(its.vertices.size()); ++v)
        if (is_break[v])
            for (int e : vertex_edges[v])
                if (!edges[e].used)
                    walk(v, e);
    for (int e = 0; e < int(edges.size()); ++e)
        if (!edges[e].used)
            walk(edges[e].a, e);
    return chains;
}

indexed_triangle_set edge_blend_mesh(const indexed_triangle_set &its, const EdgeChain &chain, EdgeBlend type, double size)
{
    const size_t segments = chain.segments();
    const size_t points   = chain.closed ? segments : segments + 1;
    if (!(size > 0.) || segments == 0 || chain.vertices.size() != points || chain.face_right.size() != segments)
        return {};
    for (int v : chain.vertices)
        if (v < 0 || size_t(v) >= its.vertices.size())
            return {};
    for (size_t k = 0; k < segments; ++k)
        if (chain.face_left[k] < 0 || size_t(chain.face_left[k]) >= its.indices.size() || chain.face_right[k] < 0 ||
            size_t(chain.face_right[k]) >= its.indices.size())
            return {};

    auto position = [&](size_t i) { return its.vertices[chain.vertices[i % points]].cast<double>(); };
    std::vector<Vec3d>  dir(segments), left(segments), right(segments);
    std::vector<double> length(segments);
    for (size_t k = 0; k < segments; ++k) {
        const std::optional<Vec3d> nl = face_normal(its, chain.face_left[k]);
        const std::optional<Vec3d> nr = face_normal(its, chain.face_right[k]);
        const Vec3d                d  = position(k + 1) - position(k);
        if (!nl || !nr || !(d.norm() > 1e-9))
            return {};
        dir[k]    = d.normalized();
        length[k] = d.norm();
        left[k]   = *nl;
        right[k] = *nr;
    }
    // Cut a convex edge (reach out of the material), fill a concave one (reach into it).
    const bool   convex = left[0].dot(face_centroid(its, chain.face_right[0]) - position(0)) < 0.;
    const double side   = convex ? 1. : -1.;

    std::vector<std::vector<Vec3d>> rings;
    std::vector<Vec3d>              centers;
    rings.reserve(points);
    for (size_t i = 0; i < points; ++i) {
        // The segments before and after this vertex.
        std::optional<size_t> before, after;
        if (chain.closed) {
            before = (i + segments - 1) % segments;
            after  = i;
        } else {
            if (i > 0) before = i - 1;
            if (i < segments) after = i;
        }
        Vec3d t = Vec3d::Zero(), nl = Vec3d::Zero(), nr = Vec3d::Zero();
        for (const std::optional<size_t> &k : {before, after})
            if (k) {
                t += dir[*k];
                nl += left[*k];
                nr += right[*k];
            }
        if (!(t.norm() > 1e-9))
            return {};
        t.normalize();
        nl = (nl - nl.dot(t) * t).normalized();
        nr = (nr - nr.dot(t) * t).normalized();
        // Along each face, square to the edge, away from it. The left face holds the edge in the chain's
        // direction, so (seen from outside) its surface lies to the left: n x t.
        const Vec3d  ul    = nl.cross(t).normalized();
        const Vec3d  ur    = t.cross(nr).normalized();
        const double angle = std::acos(std::clamp(ul.dot(ur), -1., 1.));
        if (!(angle >= kMinFaceAngle * M_PI / 180.) || !(angle <= kMaxFaceAngle * M_PI / 180.))
            return {};

        const Vec3d p = position(i);
        std::vector<Vec3d> ring;
        if (type == EdgeBlend::Fillet) {
            const double half     = 0.5 * angle;
            const Vec3d  bisector = (ul + ur).normalized();
            const Vec3d  across   = (ul - ul.dot(bisector) * bisector).normalized();
            const Vec3d  center   = p + size / std::sin(half) * bisector;
            // From the tangent point on the left face round to the one on the right, the near side of the circle.
            const double from = 0.5 * M_PI - half;
            for (int j = 0; j <= kArcSegments; ++j) {
                const double psi = from - 2. * from * j / kArcSegments;
                ring.push_back(center + size * (-std::cos(psi) * bisector + std::sin(psi) * across));
            }
        } else {
            ring.push_back(p + size * ul);
            ring.push_back(p + size * ur);
        }
        // Close the profile past the faces, round the edge itself.
        const Vec3d tl = ring.front(), tr = ring.back();
        ring.push_back(tr + side * kReach * nr);
        ring.push_back(p + side * kReach * (nl + nr) / (1. + nl.dot(nr)));
        ring.push_back(tl + side * kReach * nl);

        // Miter: where the chain turns, stretch the profile in the turn's plane so that it stays the
        // same size measured square to each segment.
        Vec3d center_point = p;
        if (before && after) {
            const double turn_cos = std::clamp(dir[*before].dot(dir[*after]), -1., 1.);
            if (turn_cos < 1. - 1e-12) {
                // k points to the centre of the curve. A profile reaching that far in would fold the sweep
                // over itself: the blend is too big for the curve (a fillet wider than a small hole's rim).
                const Vec3d  k      = (dir[*after] - dir[*before]).normalized();
                const double half   = 0.5 * std::acos(turn_cos);
                const double radius = std::min(length[*before], length[*after]) / (2. * std::sin(half));
                double       reach  = 0.;
                for (const Vec3d &q : ring)
                    reach = std::max(reach, (q - p).dot(k));
                if (reach > kMaxCurveReach * radius)
                    return {};
                const double scale = 1. / std::cos(half);
                for (Vec3d &q : ring) {
                    const Vec3d v = q - p;
                    q             = p + v + v.dot(k) * (scale - 1.) * k;
                }
            }
        }
        // A cut reaches a little past the ends of an open chain.
        if (!chain.closed && convex && (i == 0 || i + 1 == points)) {
            const Vec3d shift = (i == 0 ? -kEndReach : kEndReach) * t;
            for (Vec3d &q : ring)
                q += shift;
            center_point += shift;
        }
        rings.push_back(std::move(ring));
        centers.push_back(center_point);
    }

    indexed_triangle_set out;
    const int ring_size = int(rings.front().size());
    for (const std::vector<Vec3d> &ring : rings)
        for (const Vec3d &q : ring)
            out.vertices.emplace_back(q.cast<float>());
    const int ring_count = int(rings.size());
    const int spans      = chain.closed ? ring_count : ring_count - 1;
    for (int i = 0; i < spans; ++i) {
        const int i2 = (i + 1) % ring_count;
        for (int j = 0; j < ring_size; ++j) {
            const int j2 = (j + 1) % ring_size;
            const int a = i * ring_size + j, b = i * ring_size + j2, c = i2 * ring_size + j2, d = i2 * ring_size + j;
            out.indices.emplace_back(a, b, c);
            out.indices.emplace_back(a, c, d);
        }
    }
    if (!chain.closed) {
        // Fan caps from the edge point, which lies inside the profile and sees all of it.
        const int first = int(out.vertices.size());
        out.vertices.emplace_back(centers.front().cast<float>());
        const int last = int(out.vertices.size());
        out.vertices.emplace_back(centers.back().cast<float>());
        const int base = (ring_count - 1) * ring_size;
        for (int j = 0; j < ring_size; ++j) {
            const int j2 = (j + 1) % ring_size;
            out.indices.emplace_back(first, j2, j);
            out.indices.emplace_back(last, base + j, base + j2);
        }
    }
    if (its_volume(out) < 0.f)
        its_flip_triangles(out);
    return out;
}

} // namespace Slic3r::ModelTools
