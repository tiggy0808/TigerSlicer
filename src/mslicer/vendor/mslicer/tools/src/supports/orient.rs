use nalgebra::{Rotation3, Unit, Vector3};
use slicer::mesh::Mesh;

/// How many directions of the fibonacci sphere are tried as "down".
const CANDIDATE_DIRECTIONS: usize = 64;
/// A down-facing face whose vertices are all within this of the lowest point
/// rests on the plate and needs no support.
const RESTING_BAND: f32 = 0.05;

/// One candidate's score. Less overhanging area wins; equal area prefers the
/// shorter print, since every layer costs a peel.
#[derive(Clone, Copy)]
struct Score {
    overhang_area: f32,
    height: f32,
}

impl Score {
    fn better_than(&self, other: &Score) -> bool {
        if (self.overhang_area - other.overhang_area).abs() > 1e-4 {
            return self.overhang_area < other.overhang_area;
        }
        self.height < other.height
    }
}

/// Rotation of the mesh, as euler XYZ radians, that minimises the area the
/// automatic support generator would have to hold up: the summed area of
/// downward faces steeper than `max_angle_degrees` from straight down,
/// excluding faces that would rest on the plate. The identity orientation is
/// always a candidate, so the suggestion never loses to leaving the model
/// alone.
pub fn suggest_orientation(mesh: &Mesh, max_angle_degrees: f32) -> Vector3<f32> {
    let max_angle = max_angle_degrees.to_radians();

    // World-space triangles, transformed once.
    let vertices: Vec<Vector3<f32>> =
        mesh.vertices().iter().map(|v| mesh.transform(v)).collect();
    let faces: Vec<[u32; 3]> = (0..mesh.face_count())
        .map(|face| *mesh.face(face))
        .collect();
    if faces.is_empty() {
        return Vector3::zeros();
    }

    let mut best_rotation = Rotation3::identity();
    let mut best_score = score(&vertices, &faces, &Rotation3::identity(), max_angle);

    for direction in candidate_directions() {
        let Some(rotation) = rotation_taking_down(direction) else {
            continue;
        };
        let candidate = score(&vertices, &faces, &rotation, max_angle);
        if candidate.better_than(&best_score) {
            best_score = candidate;
            best_rotation = rotation;
        }
    }

    let (roll, pitch, yaw) = best_rotation.euler_angles();
    Vector3::new(roll, pitch, yaw)
}

/// The rotation that maps `direction` onto straight down, so the faces that
/// point along `direction` become the plate side.
fn rotation_taking_down(direction: Vector3<f32>) -> Option<Rotation3<f32>> {
    let down = Vector3::new(0.0, 0.0, -1.0f32);
    let from = Unit::try_new(direction, 1e-6)?;
    Some(Rotation3::rotation_between(&from, &down).unwrap_or_else(|| {
        // Antipodal: any half turn about a horizontal axis works.
        Rotation3::from_axis_angle(&Vector3::x_axis(), std::f32::consts::PI)
    }))
}

fn candidate_directions() -> impl Iterator<Item = Vector3<f32>> {
    // Fibonacci sphere: near-uniform without randomness, so the suggestion is
    // deterministic for a given mesh.
    let golden = std::f32::consts::PI * (3.0 - 5.0f32.sqrt());
    (0..CANDIDATE_DIRECTIONS).map(move |index| {
        let z = 1.0 - 2.0 * (index as f32 + 0.5) / CANDIDATE_DIRECTIONS as f32;
        let radius = (1.0 - z * z).max(0.0).sqrt();
        let angle = golden * index as f32;
        Vector3::new(radius * angle.cos(), radius * angle.sin(), z)
    })
}

fn score(
    vertices: &[Vector3<f32>],
    faces: &[[u32; 3]],
    rotation: &Rotation3<f32>,
    max_angle: f32,
) -> Score {
    let rotated: Vec<Vector3<f32>> = vertices.iter().map(|v| rotation * v).collect();
    let mut min_z = f32::INFINITY;
    let mut max_z = f32::NEG_INFINITY;
    for vertex in &rotated {
        min_z = min_z.min(vertex.z);
        max_z = max_z.max(vertex.z);
    }

    let mut overhang_area = 0.0f32;
    for face in faces {
        let [a, b, c] = face.map(|v| rotated[v as usize]);
        let cross = (b - a).cross(&(c - a));
        let doubled = cross.magnitude();
        if doubled <= f32::EPSILON {
            continue;
        }
        let normal = cross / doubled;
        // Matches the generator's overhang test: within max_angle of straight
        // down.
        if (-normal.z).acos() > max_angle {
            continue;
        }
        // Down-facing and level with the lowest point: rests on the plate.
        if a.z - min_z < RESTING_BAND && b.z - min_z < RESTING_BAND && c.z - min_z < RESTING_BAND {
            continue;
        }
        overhang_area += doubled * 0.5;
    }

    Score { overhang_area, height: max_z - min_z }
}
