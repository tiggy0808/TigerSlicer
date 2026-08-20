use std::collections::{HashMap, HashSet};

use common::container::ArrayCluster;
use nalgebra::Vector3;
use slicer::{
    geometry::{Ray, primitive, triangle::triangle_intersection},
    half_edge::HalfEdgeMesh,
    mesh::Mesh,
};

use crate::supports::{SupportGenerator, SupportPlacement, quazirandom::quazirandom_rect_2d};

impl<'a> SupportGenerator<'a> {
    pub fn overhanging_faces(&self, mesh: &Mesh) -> Vec<(usize, Vector3<f32>)> {
        let max_angle = self.config.max_angle.to_radians();
        let mut overhangs = Vec::new();

        for face in 0..mesh.face_count() {
            let normal = mesh.transform_normal(&mesh.normal(face));

            let angle = (-normal.z).acos();
            (angle <= max_angle).then(|| overhangs.push((face, normal)));
        }

        overhangs
    }

    /// Drop overhanging regions whose total area is under the configured
    /// minimum. Faces are grouped by shared edges, so "region" means one
    /// connected overhanging surface rather than one triangle - a large
    /// overhang made of many small triangles is kept, and a stray triangle is
    /// not, which is the distinction the setting is for.
    ///
    /// A zero minimum keeps everything and does no clustering work at all.
    pub fn keep_large_overhang_regions(
        &self,
        mesh: &Mesh,
        half_edge: &HalfEdgeMesh,
        overhangs: Vec<(usize, Vector3<f32>)>,
    ) -> Vec<(usize, Vector3<f32>)> {
        if !(self.config.min_overhang_area > 0.0) || overhangs.is_empty() {
            return overhangs;
        }

        let overhanging: HashSet<usize> = overhangs.iter().map(|(face, _)| *face).collect();
        let mut cluster = ArrayCluster::new(mesh.face_count());
        for (face, _) in overhangs.iter() {
            let verts = mesh.face(*face);
            for i in 0..3 {
                let Some(edge) = half_edge.edge_for((verts[i], verts[(i + 1) % 3])) else {
                    continue;
                };
                let Some(twin) = edge.twin else { continue };
                let neighbor = half_edge.get_edge(twin).face as usize;
                if overhanging.contains(&neighbor) {
                    cluster.union(*face as u32, neighbor as u32);
                }
            }
        }

        let mut area = HashMap::<u32, f32>::new();
        for (face, _) in overhangs.iter() {
            let root = cluster.find(*face as u32);
            *area.entry(root).or_default() += self.face_area(mesh, *face);
        }

        overhangs
            .into_iter()
            .filter(|(face, _)| {
                area.get(&cluster.find(*face as u32))
                    .copied()
                    .unwrap_or(0.0)
                    >= self.config.min_overhang_area
            })
            .collect()
    }

    /// Drop placements the model already holds up. Resin can lean outward by
    /// roughly the overhang angle as it grows, so a spot within that cone of
    /// an *anchored* piece of surface - a vertex shared with a non-overhanging
    /// face, or one resting on the plate - prints without a column. Without
    /// this test the detector is purely local and carpets every down-facing
    /// face; a hull chine that curls up from a plate-resting bottom needs
    /// nothing, and only spots far from any anchor (a bow tip, the middle of
    /// a ceiling, an island) keep their supports.
    ///
    /// The straight-line distance to an anchor is an approximation of the
    /// surface path resin actually grows along; it is exact for convex curls
    /// like a hull and generous for geometry that folds back on itself, which
    /// errs toward keeping a support rather than dropping a needed one only
    /// where the fold is *lower* than the point - islands never reach here.
    pub fn prune_self_supported(
        &self,
        mesh: &Mesh,
        overhangs: &[(usize, Vector3<f32>)],
        placements: Vec<SupportPlacement>,
    ) -> Vec<SupportPlacement> {
        if placements.is_empty() || overhangs.is_empty() {
            return placements;
        }

        let overhanging: HashSet<usize> = overhangs.iter().map(|(face, _)| *face).collect();
        let plate_level = self.config.min_support_height.max(0.0);
        let world: Vec<Vector3<f32>> =
            mesh.vertices().iter().map(|v| mesh.transform(v)).collect();

        // An anchor must be *grounded*: connected to the plate through the
        // mesh, so cured material actually chains down to something that
        // stands. A floating body's own vertical sides are not overhanging,
        // but they hold nothing up - without this test the body would anchor
        // itself and lose every support.
        let mut ground = ArrayCluster::new(mesh.vertex_count());
        for face in 0..mesh.face_count() {
            let verts = mesh.face(face);
            ground.union(verts[0], verts[1]);
            ground.union(verts[1], verts[2]);
        }
        let mut grounded_roots = HashSet::new();
        for (index, position) in world.iter().enumerate() {
            if position.z <= plate_level {
                grounded_roots.insert(ground.find(index as u32));
            }
        }

        let mut anchored = vec![false; mesh.vertex_count()];
        for face in 0..mesh.face_count() {
            if !overhanging.contains(&face) {
                for vertex in mesh.face(face) {
                    anchored[*vertex as usize] = true;
                }
            }
        }
        let mut anchors: Vec<Vector3<f32>> = Vec::new();
        let mut min_anchor_z = f32::INFINITY;
        for (index, position) in world.iter().enumerate() {
            let on_plate = position.z <= plate_level;
            let grounded = grounded_roots.contains(&ground.find(index as u32));
            if on_plate || (anchored[index] && grounded) {
                min_anchor_z = min_anchor_z.min(position.z);
                anchors.push(*position);
            }
        }
        if anchors.is_empty() {
            return placements;
        }

        // Coarse xy-grid so each placement only tests nearby anchors.
        let base_reach = self.config.face_support_spacing.max(0.5);
        let cell = base_reach * 2.0;
        let key = |x: f32, y: f32| ((x / cell).floor() as i32, (y / cell).floor() as i32);
        let mut grid: HashMap<(i32, i32), Vec<u32>> = HashMap::new();
        for (index, anchor) in anchors.iter().enumerate() {
            grid.entry(key(anchor.x, anchor.y))
                .or_default()
                .push(index as u32);
        }

        let tangent = self.config.max_angle.to_radians().tan();
        placements
            .into_iter()
            .filter(|placement| {
                let point = placement.point;
                // The widest possible reach uses the lowest anchor there is;
                // real anchors that far down must also be that far away.
                let max_reach = base_reach + (point.z - min_anchor_z).max(0.0) * tangent;
                let cells = (max_reach / cell).ceil() as i32;
                let (cx, cy) = key(point.x, point.y);
                for dx in -cells..=cells {
                    for dy in -cells..=cells {
                        let Some(bucket) = grid.get(&(cx + dx, cy + dy)) else {
                            continue;
                        };
                        for &index in bucket {
                            let anchor = anchors[index as usize];
                            if anchor.z > point.z + 0.05 {
                                continue;
                            }
                            let reach = base_reach + (point.z - anchor.z) * tangent;
                            if (point - anchor).norm_squared() <= reach * reach {
                                // Held by the model: no column needed.
                                return false;
                            }
                        }
                    }
                }
                true
            })
            .collect()
    }

    /// World-space area of one triangle.
    fn face_area(&self, mesh: &Mesh, face: usize) -> f32 {
        let verts = mesh.face(face);
        let [a, b, c] =
            [verts[0], verts[1], verts[2]].map(|v| mesh.transform(&mesh.vertices()[v as usize]));
        (b - a).cross(&(c - a)).magnitude() * 0.5
    }

    // Find edges that border two faces with different angles, at least one of
    // the faces must be overhanging.
    pub fn overhanging_edges(
        &self,
        mesh: &Mesh,
        half_edge: &HalfEdgeMesh,
        overhangs: &[(usize, Vector3<f32>)],
    ) -> Vec<HashSet<u32>> {
        let mut cluster = ArrayCluster::new(mesh.vertex_count());

        for (face, normal) in overhangs.iter() {
            let verts = mesh.face(*face);
            for i in 0..3 {
                let Some(edge) = half_edge.edge_for((verts[i], verts[(i + 1) % 3])) else {
                    continue;
                };

                let Some(twin) = edge.twin else { continue }; // ✌️
                let neighbor = half_edge.get_edge(twin).face as usize;

                let neighbor_normal = mesh.transform_normal(&mesh.normal(neighbor));
                let angle_diff = normal.angle(&neighbor_normal);

                // 0.1 rad ≈ 5°
                if angle_diff > self.config.edge_angle_delta {
                    cluster.union(edge.origin_vertex, edge.vertex);
                }
            }
        }

        cluster
            .clusters()
            .map(|x| x.iter().copied().collect::<HashSet<_>>())
            .collect()
    }

    pub fn place_face_supports(
        &self,
        mesh: &Mesh,
        overhangs: &[(usize, Vector3<f32>)],
    ) -> Vec<SupportPlacement> {
        let mut out = Vec::new();
        let bed_size = self.bed_size.xy().map(|x| x.raw());

        for pos in quazirandom_rect_2d(bed_size, self.config.face_support_spacing) {
            let pos = pos - bed_size / 2.0;
            let mut intersections = Vec::new();
            for (idx, _angle) in overhangs.iter() {
                let ray = Ray {
                    origin: mesh.inv_transform(&pos.to_homogeneous()),
                    direction: mesh.inv_transform_normal(&Vector3::z()),
                };
                let face = (mesh.face_verts_raw(*idx), mesh.normal(*idx));
                if let Some(mut intersection) = triangle_intersection::<primitive::Ray>(face, ray) {
                    intersection.1 = mesh.transform(&intersection.1);
                    intersections.push(SupportPlacement {
                        point: intersection.1,
                        normal: face.1,
                    });
                }
            }

            out.extend(intersections);
        }

        out
    }

    pub fn place_edge_supports(
        &self,
        mesh: &Mesh,
        half_edge: &HalfEdgeMesh,
        overhangs: &[(usize, Vector3<f32>)],
    ) -> Vec<SupportPlacement> {
        let edge_runs = self.overhanging_edges(mesh, half_edge, overhangs);
        let mut out = Vec::new();

        for run in edge_runs.iter() {
            let mut seen = HashSet::new();
            let start = *run.iter().next().unwrap();
            let mut stack = vec![(start, 0.0)];

            while let Some((vertex, t)) = stack.pop() {
                let start_edge = half_edge.incident_edge(vertex).unwrap();
                let mut edge = start_edge;

                while {
                    if run.contains(&edge.vertex) {
                        let canonical = (
                            edge.origin_vertex.min(edge.vertex),
                            edge.origin_vertex.max(edge.vertex),
                        );

                        if seen.insert(canonical) {
                            let [a, b] = [edge.origin_vertex, edge.vertex]
                                .map(|x| mesh.transform(&mesh.vertices()[x as usize]));

                            let vector = b - a;
                            let len = vector.magnitude();
                            let unit = vector / len;

                            let normal =
                                (mesh.transform_normal(&mesh.normal(edge.face as usize))
                                    + mesh.transform_normal(&mesh.normal(
                                        half_edge.get_edge(edge.twin.unwrap()).face as usize,
                                    )))
                                .normalize();

                            let mut t = t;
                            while t < len {
                                let point = a + unit * t;
                                out.push(SupportPlacement { point, normal });
                                t += self.config.edge_support_spacing;
                            }

                            stack.push((edge.vertex, t - len));
                        }
                    }

                    edge = half_edge.get_edge(half_edge.get_edge(edge.twin.unwrap()).next);
                    edge != start_edge
                } {}
            }
        }

        out
    }

    /// Find all points that are both lower than their surrounding points and have down facing normals
    pub fn place_point_supports(
        &self,
        mesh: &Mesh,
        half_edge: &HalfEdgeMesh,
    ) -> Vec<SupportPlacement> {
        let mut overhangs = Vec::new();
        let mut seen = HashSet::new();

        let vertices = mesh.vertices();
        for edge in 0..half_edge.half_edge_count() {
            let origin = half_edge.get_edge(edge as u32);
            if !seen.insert(origin.origin_vertex) {
                continue;
            }

            // Ignore points that are not on the bottom of the mesh
            let origin_normal = mesh.transform_normal(&mesh.normal(origin.face as usize));
            if origin_normal.z >= 0.0 {
                continue;
            }

            // Only add to overhangs if the original point is lower than all connected points by one layer
            let origin_pos = mesh.transform(&vertices[origin.origin_vertex as usize]);
            let neighbors = half_edge.connected_vertices(edge as u32);
            if (neighbors.iter()).all(|connected| {
                origin_pos.z < mesh.transform(&vertices[connected.vertex as usize]).z
            }) {
                let point = mesh.transform(&mesh.vertices()[origin.vertex as usize]);
                let normal = (neighbors.iter())
                    .map(|x| mesh.normal(x.face as usize))
                    .sum::<Vector3<_>>()
                    .normalize();

                overhangs.push(SupportPlacement { point, normal });
            }
        }

        overhangs
    }
}
