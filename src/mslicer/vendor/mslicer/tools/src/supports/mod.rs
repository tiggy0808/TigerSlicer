use std::f32::consts::PI;

use common::{geometry::convex_hull, units::Milimeters};
use nalgebra::Vector2;
use nalgebra::Vector3;
use slicer::{builder::MeshBuilder, geometry::bvh::Bvh, half_edge::HalfEdgeMesh, mesh::Mesh};

pub mod detect;
pub mod orient;
pub mod quazirandom;

pub struct SupportGenerator<'a> {
    config: &'a SupportConfig,
    bed_size: Vector3<Milimeters>,
}

pub struct Support {
    pub start: Vector3<f32>,
    pub end: Vector3<f32>,
    pub radius: f32,
}

/// A closed painted volume that steers automatic placement: a blocker keeps
/// automatic contacts out, an enforcer (in enforcer-only mode) is the only
/// place they may land. Manual points are explicit user intent and are never
/// filtered by either.
pub struct SupportRegion {
    pub vertices: Vec<Vector3<f32>>,
    pub faces: Vec<[u32; 3]>,
}

impl SupportRegion {
    /// Ray-parity containment along +Z. The origin is nudged by a fixed
    /// sub-micrometre offset so a ray sliding exactly along a triangle edge
    /// of an axis-aligned box - the common painted volume - cannot double
    /// count. Deterministic: the nudge is a constant.
    pub fn contains(&self, point: &Vector3<f32>) -> bool {
        let origin = Vector3::new(point.x + 3.1e-5, point.y + 1.7e-5, point.z);
        let mut crossings = 0_u32;
        for face in &self.faces {
            let a = self.vertices[face[0] as usize];
            let b = self.vertices[face[1] as usize];
            let c = self.vertices[face[2] as usize];
            // Project on XY; count triangles whose XY footprint contains the
            // origin and whose plane lies above it.
            let d1 = (b.x - a.x) * (origin.y - a.y) - (b.y - a.y) * (origin.x - a.x);
            let d2 = (c.x - b.x) * (origin.y - b.y) - (c.y - b.y) * (origin.x - b.x);
            let d3 = (a.x - c.x) * (origin.y - c.y) - (a.y - c.y) * (origin.x - c.x);
            let has_neg = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
            let has_pos = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
            if has_neg && has_pos {
                continue;
            }
            // Interpolate the triangle plane's Z at the origin.
            let normal = (b - a).cross(&(c - a));
            if normal.z.abs() <= f32::EPSILON {
                continue; // vertical face: no crossing along +Z
            }
            let t = normal.dot(&(a - origin)) / normal.z;
            if t > 0.0 {
                crossings += 1;
            }
        }
        crossings % 2 == 1
    }
}

/// What a support column stands on. Every variant still routes each column
/// down to the build plate; this only decides what is built where it lands.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SupportBase {
    /// Nothing. The columns meet the plate directly.
    None,
    /// One convex-hull raft under every column, offset and lifted.
    Raft,
    /// An individual tapered pad under each column.
    Pads,
}

pub struct SupportConfig {
    /// Support generation
    pub support_radius: f32,
    pub tip_radius: f32,
    pub tip_length: f32,
    pub precision: u32,

    pub raft_height: f32,
    pub raft_offset: f32,

    pub max_angle: f32,
    pub face_support_spacing: f32,
    pub edge_support_spacing: f32,
    pub edge_angle_delta: f32,

    /// How far the tip apex is pushed into the model past the contact point.
    /// Zero leaves the apex exactly on the surface, which is what a support
    /// that only touches the model does. Larger values bite in, holding
    /// better and leaving a deeper mark.
    pub contact_penetration: f32,
    /// A contact point closer to the plate than this gets no support: there is
    /// no room for a tip under it, and a column shorter than its own tip
    /// prints as a blob against the plate rather than as a support.
    pub min_support_height: f32,
    /// Connected overhanging regions with less surface area than this are left
    /// unsupported. Isolated local minima are never dropped by this rule -
    /// those are the points that cannot print at all without a support, so
    /// there is no area at which ignoring one is right.
    pub min_overhang_area: f32,
    pub base: SupportBase,
    /// Diameter of an individual pad, used only by `SupportBase::Pads`.
    pub base_diameter: f32,
    /// A column may end on the model directly below its contact instead of
    /// routing to the plate, when there is room for a second tip there. The
    /// lower tip bites into the surface by the same contact penetration.
    pub on_model: bool,
    /// Diagonal X braces between neighbouring plate columns.
    pub cross_bracing: bool,
    /// Maximum distance between two feet that brace together; also the height
    /// of the vertical band one X of braces covers.
    pub brace_spacing: f32,
    /// The pillar radius grows with column height when set: one millimetre of
    /// radius for every 50 mm of height, capped at `max_radius`.
    pub adaptive_radius: bool,
    /// Upper bound for the adaptive pillar radius.
    pub max_radius: f32,
    /// Automatic contacts inside any of these volumes are dropped.
    pub blockers: Vec<SupportRegion>,
    /// With `enforcers_only`, automatic contacts must lie inside one of these.
    pub enforcers: Vec<SupportRegion>,
    pub enforcers_only: bool,
}

pub struct SupportPlacement {
    pub point: Vector3<f32>,
    pub normal: Vector3<f32>,
}

impl<'a> SupportGenerator<'a> {
    pub fn new(config: &'a SupportConfig, bed_size: Vector3<Milimeters>) -> Self {
        Self { config, bed_size }
    }

    pub fn generate_supports(
        &self,
        mesh: &Mesh,
        half_edge: &HalfEdgeMesh,
        bvh: &Bvh,
        manual: &[Vector3<f32>],
        manual_only: bool,
    ) -> Option<Mesh> {
        let mut overhangs = Vec::new();

        if !manual_only {
            let faces = self.overhanging_faces(mesh);
            // A region too small to be worth a column is dropped before
            // anything is placed on it, so the area rule cannot be undone
            // downstream.
            let faces = self.keep_large_overhang_regions(mesh, half_edge, faces);
            // Local minima are unconditional: nothing below can hold an
            // island up. Face and edge placements are pruned where the model
            // already supports them - see prune_self_supported.
            overhangs.extend(self.place_point_supports(mesh, half_edge));
            let mut carpet = self.place_face_supports(mesh, &faces);
            carpet.extend(self.place_edge_supports(mesh, half_edge, &faces));
            overhangs.extend(self.prune_self_supported(mesh, &faces, carpet));
            // Painted volumes steer only the automatic contacts; a manually
            // placed point is explicit intent and is exempt from both rules.
            if !self.config.blockers.is_empty() {
                overhangs.retain(|placement| {
                    !self
                        .config
                        .blockers
                        .iter()
                        .any(|region| region.contains(&placement.point))
                });
            }
            if self.config.enforcers_only {
                overhangs.retain(|placement| {
                    self.config
                        .enforcers
                        .iter()
                        .any(|region| region.contains(&placement.point))
                });
            }
        }
        overhangs.extend(self.place_manual_supports(mesh, bvh, manual));

        let mut builder = MeshBuilder::new();
        let feet = self.build_support_mesh(mesh, bvh, &overhangs, &mut builder);
        self.build_base_mesh(&feet, &mut builder);

        (!builder.is_empty()).then(|| builder.build())
    }

    /// A manually placed point is snapped to the nearest model surface and
    /// supported there. The tip leans along the face normal when the face
    /// points meaningfully downward - the same geometry an automatic contact
    /// gets - and straight down otherwise, so a point on a side wall becomes
    /// a vertical tip against that wall rather than a sideways spike.
    fn place_manual_supports(
        &self,
        mesh: &Mesh,
        bvh: &Bvh,
        manual: &[Vector3<f32>],
    ) -> Vec<SupportPlacement> {
        let mut out = Vec::new();
        for request in manual {
            if !request.iter().all(|c| c.is_finite()) {
                continue;
            }
            let Some(closest) = bvh.closest(mesh, *request) else {
                continue;
            };
            let point = mesh.transform(&closest.position);
            let face_normal = mesh.transform_normal(&mesh.normal(closest.face));
            let normal = if face_normal.iter().all(|c| c.is_finite()) && face_normal.z < -0.1 {
                face_normal
            } else {
                Vector3::new(0.0, 0.0, -1.0)
            };
            out.push(SupportPlacement { point, normal });
        }
        out
    }

    /// Sample points around each foot, used to size the raft hull.
    fn foot_outline(&self, feet: &[Vector3<f32>]) -> Vec<Vector2<f32>> {
        let (r, p) = (self.config.support_radius, self.config.precision);
        let mut points = Vec::new();
        for foot in feet {
            for i in 0..(p * 2) {
                let angle = i as f32 / p as f32 * PI;
                let normal = Vector2::new(angle.cos(), angle.sin());
                points.push(foot.xy() + normal * r);
            }
        }
        points
    }

    fn build_base_mesh(&self, feet: &[Vector3<f32>], builder: &mut MeshBuilder) {
        match self.config.base {
            SupportBase::None => {}
            SupportBase::Raft => {
                let outline = self.foot_outline(feet);
                if !outline.is_empty() {
                    self.build_raft_mesh(&outline, builder);
                }
            }
            SupportBase::Pads => {
                // Each column gets its own tapered pad. Nothing is merged, so
                // one column landing far from the rest does not drag a raft
                // across the plate to reach it.
                let radius = (self.config.base_diameter * 0.5).max(self.config.support_radius);
                for foot in feet {
                    // A pad is wider than the column that stands on it, so it
                    // is held inside the plate by its own radius.
                    let foot = self.clamp_into_volume(*foot, radius, 0.0);
                    let bottom = Vector3::new(foot.x, foot.y, 0.0);
                    let top = Vector3::new(foot.x, foot.y, self.config.raft_height);
                    builder.add_cylinder(
                        (bottom, top),
                        (radius, self.config.support_radius),
                        self.config.precision,
                    );
                }
            }
        }
    }

    /// Radius the shaft of a column of this height gets. The adaptive rule is
    /// deliberately simple and documented: one millimetre of extra radius per
    /// 50 mm of column height, never above `max_radius` and never below the
    /// configured pillar radius.
    /// Keep a support position inside the build volume. Generator space is
    /// centred on the plate, so xy is bounded by half the bed inset by the
    /// radius of whatever is drawn there, and z never goes under the plate.
    /// A column leaning in by a fraction of a millimetre at the plate edge is
    /// how it stays printable; the alternative was refusing the whole
    /// generation because one foot crossed the boundary.
    /// `xy_margin` insets the plate boundary by the radius drawn around the
    /// position. `z_margin` does the same below the plate, and is zero for
    /// anything that is *meant* to touch it - a foot standing on the plate
    /// must stay at Z=0, not be lifted by its own radius.
    fn clamp_into_volume(&self, point: Vector3<f32>, xy_margin: f32, z_margin: f32) -> Vector3<f32> {
        let half_x = (self.bed_size.x.raw() * 0.5 - xy_margin).max(0.0);
        let half_y = (self.bed_size.y.raw() * 0.5 - xy_margin).max(0.0);
        let top = (self.bed_size.z.raw() - z_margin).max(0.0);
        Vector3::new(
            point.x.clamp(-half_x, half_x),
            point.y.clamp(-half_y, half_y),
            point.z.clamp(z_margin.min(top), top),
        )
    }

    fn column_radius(&self, height: f32) -> f32 {
        let r = self.config.support_radius;
        if !self.config.adaptive_radius {
            return r;
        }
        (r + height.max(0.0) / 50.0).min(self.config.max_radius.max(r))
    }

    fn build_support_mesh(
        &self,
        mesh: &Mesh,
        bvh: &Bvh,
        overhangs: &[SupportPlacement],
        builder: &mut MeshBuilder,
    ) -> Vec<Vector3<f32>> {
        let (r, p) = (self.config.support_radius, self.config.precision);
        // A column standing on a raft or on pads ends where the base begins;
        // one standing on nothing ends on the plate itself. The previous code
        // ended every column at a fixed Z of one millimetre - exactly the
        // default base height - which left columns floating when the base was
        // None or its height was not 1.
        let base_z = match self.config.base {
            SupportBase::None => 0.0,
            SupportBase::Raft | SupportBase::Pads => self.config.raft_height,
        };

        let mut feet = Vec::new();
        // Vertical plate-column shafts, kept for cross-bracing: bottom and top
        // of the braceable segment plus the shaft radius.
        let mut shafts: Vec<(Vector3<f32>, f32, f32)> = Vec::new();
        for SupportPlacement { point, normal } in overhangs.iter() {
            // A zero-area sliver face has no defined normal; the placements it
            // contributed carry NaN. One is not a support candidate - it is
            // nothing - and letting it through would poison the whole mesh's
            // bounds validation.
            if !point.iter().chain(normal.iter()).all(|c| c.is_finite()) {
                continue;
            }
            // Down-facing triangles which already touch the build plate are
            // reported as overhangs by the generic detector, but supporting
            // them would create a tip below Z=0. They need no support.
            if point.z <= self.config.min_support_height {
                continue;
            }
            // The apex sits inside the model by the configured penetration, so
            // the tip bites in rather than resting on the surface.
            // A marker sphere of this radius is drawn at the apex, so the apex
            // is held that far inside the volume too.
            const APEX_MARKER_RADIUS: f32 = 0.2;
            let apex = self
                .clamp_into_volume(point - normal * self.config.contact_penetration,
                                   APEX_MARKER_RADIUS, APEX_MARKER_RADIUS);
            let mut start = point + normal * self.config.tip_length;
            // Very low overhangs can need a shortened tip. Keep the generated
            // preparation mesh on the physical side of the build plate.
            start.z = start.z.max(0.0);
            // The tip cone ends here with the full pillar radius. When the
            // contact normal leans, that end disc leans with it and reaches a
            // radius below the endpoint, so the endpoint is held a radius up.
            let start = self.clamp_into_volume(start, r, r);

            // Land on the model below when allowed and there is room for the
            // lower tip; such a column carries no base and no bracing, since
            // both belong to the plate.
            if self.config.on_model {
                if let Some(hit) = bvh.intersect_ray(mesh, start, -Vector3::z()) {
                    let land = mesh.transform(&hit.position);
                    if land.z > f32::EPSILON && start.z - land.z > self.config.tip_length {
                        let height = start.z - land.z;
                        let r_eff = self.column_radius(height);
                        let lower_top = self.clamp_into_volume(
                            Vector3::new(start.x, start.y, land.z + self.config.tip_length),
                            r_eff, 0.0);
                        let lower_apex = self.clamp_into_volume(
                            Vector3::new(start.x, start.y,
                                         land.z - self.config.contact_penetration),
                            APEX_MARKER_RADIUS, APEX_MARKER_RADIUS);
                        builder.add_cylinder((apex, start), (self.config.tip_radius, r), p);
                        builder.add_cylinder((start, lower_top), (r_eff, r_eff), p);
                        builder.add_cylinder((lower_top, lower_apex), (r_eff, self.config.tip_radius), p);
                        builder.add_sphere(apex, 0.2, p);
                        builder.add_sphere(start, r_eff, p);
                        builder.add_sphere(lower_top, r_eff, p);
                        continue;
                    }
                }
            }

            if let Some(lines) = route_support(mesh, bvh, start) {
                let foot = Vector3::new(lines[2].x, lines[2].y, base_z);
                let height = lines[1].z - foot.z;
                let r_eff = self.column_radius(height);
                // A bend at plate level would put its joint sphere and the
                // cylinder walls below the plate; keep the whole joint above.
                let bend = Vector3::new(lines[1].x, lines[1].y, lines[1].z.max(r_eff));
                // Everything below is drawn with a radius, so each position is
                // held that far inside the volume and the geometry - not just
                // its centre line - stays printable.
                let bend = self.clamp_into_volume(bend, r_eff, 0.0);
                // The last segment drops straight down from the bend. A
                // slanted final segment lands its end cap tilted, and half of
                // that disc is then below the plate - which is what made the
                // whole generation get refused. Vertical is also the shape a
                // support actually prints as where it meets the plate.
                let foot = self.clamp_into_volume(
                    Vector3::new(bend.x, bend.y, foot.z), r_eff, 0.0);
                // Joint spheres of this radius are drawn at both ends of the
                // escape walk, so both are held a full radius inside.
                let walk_top = self.clamp_into_volume(lines[0], r_eff, r_eff);
                builder.add_cylinder((apex, start), (self.config.tip_radius, r), p);
                builder.add_cylinder((walk_top, bend), (r_eff, r_eff), p);
                builder.add_cylinder((bend, foot), (r_eff, r_eff), p);

                feet.push(foot);
                shafts.push((foot, bend.z, r_eff));

                builder.add_sphere(apex, 0.2, p);
                builder.add_sphere(walk_top, r_eff, p);
                builder.add_sphere(bend, r_eff, p);
            }
        }

        if self.config.cross_bracing {
            self.build_braces(&shafts, builder);
        }

        feet
    }

    /// X-shaped diagonal braces between each plate column and its nearest
    /// neighbour within `brace_spacing`, one X per vertical band of that same
    /// height. Nearest-neighbour pairing keeps the brace count linear in the
    /// column count rather than quadratic in a dense forest.
    fn build_braces(&self, shafts: &[(Vector3<f32>, f32, f32)], builder: &mut MeshBuilder) {
        let spacing = self.config.brace_spacing;
        if !(spacing > 0.0) || shafts.len() < 2 {
            return;
        }
        let p = self.config.precision;
        let brace_r = self.config.tip_radius;

        let mut braced: Vec<(usize, usize)> = Vec::new();
        for (i, (foot_i, top_i, _)) in shafts.iter().enumerate() {
            let mut nearest: Option<(usize, f32)> = None;
            for (j, (foot_j, _, _)) in shafts.iter().enumerate() {
                if i == j {
                    continue;
                }
                let d = (foot_i.xy() - foot_j.xy()).magnitude();
                if d <= spacing && d > f32::EPSILON
                    && nearest.map_or(true, |(_, best)| d < best)
                {
                    nearest = Some((j, d));
                }
            }
            let Some((j, _)) = nearest else { continue };
            let pair = (i.min(j), i.max(j));
            if braced.contains(&pair) {
                continue;
            }
            braced.push(pair);

            let (foot_j, top_j, _) = &shafts[j];
            // A slanted brace's wall reaches up to its radius below the axis
            // endpoint; starting the band one radius up keeps it off the plate.
            let z0 = foot_i.z.max(foot_j.z).max(brace_r);
            let z1 = top_i.min(*top_j);
            let mut za = z0;
            while za + 0.5 < z1 {
                let zb = (za + spacing).min(z1);
                let a_lo = Vector3::new(foot_i.x, foot_i.y, za);
                let a_hi = Vector3::new(foot_i.x, foot_i.y, zb);
                let b_lo = Vector3::new(foot_j.x, foot_j.y, za);
                let b_hi = Vector3::new(foot_j.x, foot_j.y, zb);
                builder.add_cylinder((a_lo, b_hi), (brace_r, brace_r), p);
                builder.add_cylinder((b_lo, a_hi), (brace_r, brace_r), p);
                za = zb;
            }
        }
    }

    fn build_raft_mesh(&self, points: &[Vector2<f32>], builder: &mut MeshBuilder) {
        let hull = convex_hull(points);
        let idx = builder.next_idx();
        for i in 0..hull.len() {
            let point = hull[i];
            let next = hull[(i + 1) % hull.len()];
            let prev = hull[(i + hull.len() - 1) % hull.len()];

            let edge_1 = next - point;
            let edge_2 = point - prev;
            let edge_1_normal =
                Vector2::new(edge_1.y, -edge_1.x).try_normalize(f32::EPSILON);
            let edge_2_normal =
                Vector2::new(edge_2.y, -edge_2.x).try_normalize(f32::EPSILON);
            let offset = match (edge_1_normal, edge_2_normal) {
                (Some(first), Some(second)) => (first + second)
                    .try_normalize(f32::EPSILON)
                    .unwrap_or(first),
                (Some(normal), None) | (None, Some(normal)) => normal,
                (None, None) => Vector2::zeros(),
            };

            // The raft is offset outward from the hull of the feet, which is
            // exactly the direction that leaves the plate when a column lands
            // near the edge. Both rings are held inside.
            builder.add_vertex(self.clamp_into_volume(point.push(0.0), 0.0, 0.0));
            builder.add_vertex(self.clamp_into_volume(
                (point + offset * self.config.raft_offset).push(self.config.raft_height),
                0.0, 0.0,
            ));
        }

        let verts = builder.next_idx() - idx;
        for i in (0..verts).step_by(2) {
            if i != 0 && i + 3 < verts {
                builder.add_face([idx, idx + i + 2, idx + i]);
                builder.add_face([idx + 1, idx + i + 1, idx + i + 3]);
            }

            builder.add_quad([
                idx + i % verts,
                idx + (i + 1) % verts,
                idx + (i + 2) % verts,
                idx + (i + 3) % verts,
            ]);
        }
    }
}

pub fn route_support(mesh: &Mesh, bvh: &Bvh, position: Vector3<f32>) -> Option<[Vector3<f32>; 3]> {
    let mut point = position;
    let mut momentum = Vector3::zeros();
    let beta = 0.9;

    for _ in 0..100 {
        let closest = bvh.closest(mesh, point).unwrap();
        let grad = point - mesh.transform(&closest.position);

        momentum = beta * momentum + (1.0 - beta) * grad;
        let Some(direction) = momentum
            .xy()
            .push(momentum.z.min(0.0))
            .try_normalize(f32::EPSILON)
        else {
            // A point exactly on the closest feature has no useful escape
            // gradient. Skipping this candidate is safer than emitting NaN
            // support geometry; neighboring candidates still cover the face.
            return None;
        };
        point += direction * closest.t.min(1.0);
        // The walk escapes downward and outward; below the plate there is
        // nothing to escape from, and a bend under the plate would generate
        // geometry the build volume refuses.
        point.z = point.z.max(0.0);
        if !point.iter().all(|coordinate| coordinate.is_finite()) {
            return None;
        }

        if bvh.intersect_ray(mesh, point, -Vector3::z()).is_none() {
            // The foot is reported on the plate; the caller raises it to the
            // top of whatever base the column stands on.
            return Some([position, point, point.xy().push(0.0)]);
        }
    }

    None
}

impl Default for SupportConfig {
    fn default() -> Self {
        Self {
            support_radius: 1.0,
            tip_radius: 0.2,
            tip_length: 3.0,
            raft_height: 1.0,
            raft_offset: 1.0,
            precision: 10,
            max_angle: 30.0,
            face_support_spacing: 50.0,
            edge_angle_delta: 0.1,
            edge_support_spacing: 20.0,
            // Defaults reproduce the behaviour these fields replaced, so an
            // unchanged configuration generates exactly what it used to.
            contact_penetration: 0.0,
            min_support_height: 3.0 + 0.2,
            min_overhang_area: 0.0,
            base: SupportBase::Raft,
            base_diameter: 3.0,
            on_model: false,
            cross_bracing: false,
            brace_spacing: 10.0,
            adaptive_radius: false,
            max_radius: 3.0,
            blockers: Vec::new(),
            enforcers: Vec::new(),
            enforcers_only: false,
        }
    }
}
