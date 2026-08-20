use std::{
    ffi::{c_char, c_void},
    io::Cursor,
    mem,
    net::{Ipv4Addr, SocketAddrV4},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
};

use common::{
    container::rle::{self, Run},
    progress::Progress,
    serde::{DynamicSerializer, SliceDeserializer},
    slice::{ExposureConfig, Layer, SliceConfig, VectorLayer, format::RasterFormat},
    units::{Milimeters, Minutes, Second, Seconds},
};
use image::RgbaImage;
use nalgebra::{Matrix4, Vector2, Vector3};
use remote_print::{
    Resolution,
    probe::{ProbeError, probe_printer_with_cancel},
    session::{SessionError, UploadRequest, upload_artifact_with_cancel},
};
use serde_json::json;
use slicer::{
    geometry::bvh::Bvh,
    half_edge::HalfEdgeMesh,
    mesh::Mesh,
    slicer::{SliceError, Slicer, SlicerModel, vector::SvgFile},
    util,
};

// Compile the pinned upstream support generator into the bridge without
// pulling MSlicer's unrelated desktop-tools dependency graph into the product
// static library.
#[path = "../../tools/src/supports/mod.rs"]
mod supports;
use supports::{
    SupportBase, SupportConfig, SupportGenerator, SupportRegion, orient::suggest_orientation,
};

const ABI_VERSION: u32 = 11;
const ENGINE_VERSION: &[u8] = b"MSlicer 0.8.0-4-gbe44e0b / TigerSlicer ABI 6\0";
const MAX_AXIS_PIXELS: u32 = 32_768;
const MAX_LAYER_PIXELS: u64 = 1_000_000_000;
const MAX_LAYERS: f32 = 1_000_000.0;
const MAX_VERTICES: usize = 100_000_000;
const MAX_TRIANGLES: usize = 100_000_000;
const MAX_OUTPUT_BYTES: usize = 16 * 1024 * 1024 * 1024;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TsStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidConfig = 2,
    InvalidMesh = 3,
    Cancelled = 4,
    NotReady = 5,
    BufferTooSmall = 6,
    Unsupported = 7,
    EngineError = 8,
    Panic = 9,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum TsFormat {
    Ctb = 1,
    Goo = 2,
    NanoDlp = 3,
    Svg = 4,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TsExposure {
    exposure_seconds: f32,
    delay_seconds: f32,
    pwm: u8,
    reserved: [u8; 3],
    lift_distance_mm: f32,
    lift_speed_mm_min: f32,
    retract_distance_mm: f32,
    retract_speed_mm_min: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TsConfig {
    struct_size: u32,
    resolution_x: u32,
    resolution_y: u32,
    platform_x_mm: f32,
    platform_y_mm: f32,
    platform_z_mm: f32,
    layer_height_mm: f32,
    supersample: u8,
    orientation: u8,
    mirror_x: u8,
    mirror_y: u8,
    first_layers: u32,
    transition_layers: u32,
    normal: TsExposure,
    first: TsExposure,
    deterministic_seed: u64,
    // ABI 11: raster compensation. absolute_correction grows (positive) or
    // shrinks (negative) every layer; elefant_foot shrinks the first layer
    // only. Both are morphological operations in whole pixels: the requested
    // millimetres are rounded to the nearest pixel per axis, and a request
    // under half a pixel is a no-op.
    absolute_correction_mm: f32,
    elefant_foot_compensation_mm: f32,
}

#[repr(C)]
pub struct TsMesh {
    struct_size: u32,
    vertices_xyz: *const f32,
    vertex_count: usize,
    triangles: *const u32,
    triangle_count: usize,
    transform: [f32; 16],
    relative_exposure: u8,
    // ABI 11: 0 = printable model, 1 = support blocker volume, 2 = support
    // enforcer volume. Region volumes are never sliced; they steer automatic
    // support placement.
    role: u8,
    reserved: [u8; 6],
}

#[repr(C)]
pub struct TsSupportConfig {
    struct_size: u32,
    support_radius_mm: f32,
    tip_radius_mm: f32,
    tip_length_mm: f32,
    precision: u32,
    base_height_mm: f32,
    base_offset_mm: f32,
    max_overhang_angle_degrees: f32,
    face_support_spacing_mm: f32,
    edge_support_spacing_mm: f32,
    edge_angle_delta_radians: f32,
    // ABI 7.
    contact_penetration_mm: f32,
    min_support_height_mm: f32,
    min_overhang_area_mm2: f32,
    base_kind: u32,
    base_diameter_mm: f32,
    // ABI 8.
    support_on_model: u32,
    cross_bracing: u32,
    brace_spacing_mm: f32,
    adaptive_diameter: u32,
    max_pillar_diameter_mm: f32,
    // ABI 11: automatic contacts are restricted to enforcer volumes.
    enforcers_only: u32,
}

#[repr(C)]
pub struct TsBuffer {
    data: *mut u8,
    size: usize,
    capacity: usize,
}

#[repr(C)]
pub struct TsMetrics {
    struct_size: u32,
    layer_count: u32,
    lit_voxels: u64,
    resin_volume_mm3: f64,
    estimated_seconds: f64,
}

pub type TsCancelCallback = unsafe extern "C" fn(*mut c_void) -> u8;
pub type TsProgressCallback = unsafe extern "C" fn(*mut c_void, u64, u64);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TsRemoteProbeRequest {
    struct_size: u32,
    address: *const u8,
    address_size: usize,
    port: u16,
    reserved: u16,
    resolution_x: u32,
    resolution_y: u32,
    timeout_ms: u32,
    cancelled: Option<TsCancelCallback>,
    user_data: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TsRemoteUploadRequest {
    struct_size: u32,
    address: *const u8,
    address_size: usize,
    filename: *const u8,
    filename_size: usize,
    artifact: *const u8,
    artifact_size: usize,
    port: u16,
    reserved: u16,
    resolution_x: u32,
    resolution_y: u32,
    probe_timeout_ms: u32,
    connect_timeout_ms: u32,
    transfer_timeout_ms: u32,
    cancelled: Option<TsCancelCallback>,
    progress: Option<TsProgressCallback>,
    user_data: *mut c_void,
}

struct EngineState {
    config_ffi: TsConfig,
    config: SliceConfig,
    models: Vec<SlicerModel>,
    physical_models: Vec<Mesh>,
    // Painted support-steering volumes in build-volume millimetres.
    support_blockers: Vec<SupportRegion>,
    support_enforcers: Vec<SupportRegion>,
    // Manually placed support points in build-volume millimetres, honoured by
    // the next generate_supports call. manual_only suppresses detection.
    manual_support_points: Vec<Vector3<f32>>,
    manual_support_only: bool,
    prepared_support: Option<Mesh>,
    layers: Option<Vec<Layer>>,
    last_status: TsStatus,
    last_message: String,
    diagnostics: Vec<Diagnostic>,
}

struct Diagnostic {
    severity: &'static str,
    code: u32,
    message: String,
}

pub struct TsEngine {
    state: Mutex<EngineState>,
    progress: Progress,
    cancelled: Arc<AtomicBool>,
}

fn finite_positive(value: f32) -> bool {
    value.is_finite() && value > 0.0
}

fn validate_exposure(exposure: &TsExposure, name: &str) -> Result<(), String> {
    let nonnegative = [
        (exposure.exposure_seconds, "exposure time"),
        (exposure.delay_seconds, "delay"),
        (exposure.lift_distance_mm, "lift distance"),
        (exposure.retract_distance_mm, "retract distance"),
    ];
    for (value, field) in nonnegative {
        if !value.is_finite() || value < 0.0 {
            return Err(format!("{name} {field} must be finite and non-negative"));
        }
    }
    for (value, field) in [
        (exposure.lift_speed_mm_min, "lift speed"),
        (exposure.retract_speed_mm_min, "retract speed"),
    ] {
        if !finite_positive(value) {
            return Err(format!("{name} {field} must be finite and positive"));
        }
    }
    Ok(())
}

fn validate_config(config: &TsConfig) -> Result<(), String> {
    if config.struct_size as usize != mem::size_of::<TsConfig>() {
        return Err("configuration structure size does not match ABI 6".into());
    }
    if config.resolution_x == 0
        || config.resolution_y == 0
        || config.resolution_x > MAX_AXIS_PIXELS
        || config.resolution_y > MAX_AXIS_PIXELS
        || u64::from(config.resolution_x) * u64::from(config.resolution_y) > MAX_LAYER_PIXELS
    {
        return Err("mask resolution is zero or exceeds the ABI safety limit".into());
    }
    for (value, name) in [
        (config.platform_x_mm, "platform X"),
        (config.platform_y_mm, "platform Y"),
        (config.platform_z_mm, "platform Z"),
        (config.layer_height_mm, "layer height"),
    ] {
        if !finite_positive(value) {
            return Err(format!("{name} must be finite and positive"));
        }
    }
    if config.layer_height_mm > config.platform_z_mm {
        return Err("layer height exceeds the platform Z extent".into());
    }
    if !(1..=8).contains(&config.supersample) {
        return Err("supersample must be in the range 1 through 8".into());
    }
    if config.orientation > 1 || config.mirror_x > 1 || config.mirror_y > 1 {
        return Err("orientation and mirror fields must use the documented 0 or 1 values".into());
    }
    let supersample = u64::from(config.supersample);
    let sampled_pixels =
        u64::from(config.resolution_x) * u64::from(config.resolution_y) * supersample * supersample;
    if sampled_pixels > MAX_LAYER_PIXELS {
        return Err("supersampled mask exceeds the ABI safety limit".into());
    }
    if config.platform_z_mm / config.layer_height_mm > MAX_LAYERS {
        return Err("configured build height exceeds the layer-count safety limit".into());
    }
    if config
        .first_layers
        .checked_add(config.transition_layers)
        .is_none()
    {
        return Err("first and transition layer counts overflow".into());
    }
    if !config.absolute_correction_mm.is_finite() || config.absolute_correction_mm.abs() > 5.0 {
        return Err("absolute correction must be finite and within 5 mm".into());
    }
    if !config.elefant_foot_compensation_mm.is_finite()
        || config.elefant_foot_compensation_mm < 0.0
        || config.elefant_foot_compensation_mm > 5.0
    {
        return Err("elephant-foot compensation must be finite, non-negative and within 5 mm".into());
    }
    validate_exposure(&config.normal, "normal")?;
    validate_exposure(&config.first, "first-layer")?;
    Ok(())
}

fn convert_exposure(value: &TsExposure) -> ExposureConfig {
    ExposureConfig {
        exposure_time: Seconds::new(value.exposure_seconds),
        exposure_delay: Seconds::new(value.delay_seconds),
        pwm: value.pwm,
        lift_distance: Milimeters::new(value.lift_distance_mm),
        lift_speed: (Milimeters::new(value.lift_speed_mm_min) / Minutes::new(1.0)).convert(),
        retract_distance: Milimeters::new(value.retract_distance_mm),
        retract_speed: (Milimeters::new(value.retract_speed_mm_min) / Minutes::new(1.0)).convert(),
    }
}

fn convert_config(value: &TsConfig) -> SliceConfig {
    SliceConfig {
        supersample: value.supersample,
        platform_resolution: Vector2::new(value.resolution_x, value.resolution_y),
        platform_size: Vector3::new(
            value.platform_x_mm,
            value.platform_y_mm,
            value.platform_z_mm,
        )
        .map(Milimeters::new),
        slice_height: Milimeters::new(value.layer_height_mm),
        exposure_config: convert_exposure(&value.normal),
        first_exposure_config: convert_exposure(&value.first),
        first_layers: value.first_layers,
        transition_layers: value.transition_layers,
        ..SliceConfig::default()
    }
}

fn output_dimensions(config: &TsConfig) -> (usize, usize) {
    if config.orientation == 1 {
        (config.resolution_y as usize, config.resolution_x as usize)
    } else {
        (config.resolution_x as usize, config.resolution_y as usize)
    }
}

fn encode_pixels(pixels: &[u8]) -> Vec<Run> {
    if pixels.is_empty() {
        return Vec::new();
    }
    let mut runs = Vec::new();
    let mut value = pixels[0];
    let mut length = 1_u64;
    for &pixel in &pixels[1..] {
        if pixel == value {
            length += 1;
        } else {
            runs.push(Run::new(length, value));
            value = pixel;
            length = 1;
        }
    }
    runs.push(Run::new(length, value));
    runs
}

/// Van Herk / Gil-Werman running minimum or maximum over a window of
/// `2 * radius + 1`, O(1) amortised per pixel. Values outside the row count
/// as 0: for erosion the world beyond the mask is empty and eats the border,
/// for dilation it contributes nothing. Grayscale-correct, so anti-aliased
/// edges erode and dilate like the material they partially cover.
fn running_extreme_row(row: &mut [u8], radius: usize, maximum: bool) {
    if radius == 0 || row.is_empty() {
        return;
    }
    let n = row.len();
    let w = 2 * radius + 1;
    // Pad both borders with the outside value (0) and round the length up to
    // whole blocks, so every window is fully in range and every block is
    // full. For erosion the zero border eats edge pixels, which is what
    // "outside the mask is empty" means; for dilation zero is neutral.
    let padded_len = ((n + 2 * radius) + w - 1) / w * w;
    let mut padded = vec![0_u8; padded_len];
    padded[radius..radius + n].copy_from_slice(row);
    let pick = |a: u8, b: u8| if maximum { a.max(b) } else { a.min(b) };
    let mut g = vec![0_u8; padded_len];
    let mut h = vec![0_u8; padded_len];
    for block_start in (0..padded_len).step_by(w) {
        let block_end = block_start + w;
        g[block_start] = padded[block_start];
        for i in block_start + 1..block_end {
            g[i] = pick(g[i - 1], padded[i]);
        }
        h[block_end - 1] = padded[block_end - 1];
        for i in (block_start..block_end - 1).rev() {
            h[i] = pick(h[i + 1], padded[i]);
        }
    }
    for (i, out) in row.iter_mut().enumerate() {
        // The window [i - radius, i + radius] in original coordinates is
        // [i, i + 2 * radius] in padded coordinates.
        *out = pick(h[i], g[i + 2 * radius]);
    }
}

fn morph_mask(data: &mut [u8], width: usize, height: usize, rx: usize, ry: usize, maximum: bool) {
    if rx > 0 {
        for y in 0..height {
            running_extreme_row(&mut data[y * width..(y + 1) * width], rx, maximum);
        }
    }
    if ry > 0 {
        let mut column = vec![0_u8; height];
        for x in 0..width {
            for y in 0..height {
                column[y] = data[y * width + x];
            }
            running_extreme_row(&mut column, ry, maximum);
            for y in 0..height {
                data[y * width + x] = column[y];
            }
        }
    }
}

fn compensation_radii(config: &TsConfig, millimetres: f32) -> (usize, usize) {
    let px_x = config.platform_x_mm / config.resolution_x as f32;
    let px_y = config.platform_y_mm / config.resolution_y as f32;
    (
        (millimetres / px_x).round().max(0.0) as usize,
        (millimetres / px_y).round().max(0.0) as usize,
    )
}

fn apply_raster_compensation(config: &TsConfig, layers: Vec<Layer>) -> Vec<Layer> {
    let (abs_rx, abs_ry) = compensation_radii(config, config.absolute_correction_mm.abs());
    let (foot_rx, foot_ry) = compensation_radii(config, config.elefant_foot_compensation_mm);
    let absolute_any = abs_rx > 0 || abs_ry > 0;
    let foot_any = foot_rx > 0 || foot_ry > 0;
    if !absolute_any && !foot_any {
        return layers;
    }
    let width = config.resolution_x as usize;
    let height = config.resolution_y as usize;
    let dilate = config.absolute_correction_mm > 0.0;
    layers
        .into_iter()
        .enumerate()
        .map(|(index, layer)| {
            let elephant = foot_any && index == 0;
            if !absolute_any && !elephant {
                return layer;
            }
            let mut mask = rle::decode_vec(layer.data.iter().copied());
            if absolute_any {
                morph_mask(&mut mask, width, height, abs_rx, abs_ry, dilate);
            }
            if elephant {
                morph_mask(&mut mask, width, height, foot_rx, foot_ry, false);
            }
            Layer {
                data: encode_pixels(&mask),
                height: layer.height,
                exposure: layer.exposure,
            }
        })
        .collect()
}

/// Lit x-spans `[start, end)` per row, decoded straight from the RLE without
/// materializing the bitmap. Spans are emitted in ascending order per row.
fn lit_spans(runs: &[Run], width: usize, height: usize) -> Vec<Vec<(u32, u32)>> {
    let mut rows = vec![Vec::new(); height];
    let mut pos = 0_u64;
    let w = width as u64;
    for run in runs {
        if run.value > 0 {
            let mut start = pos;
            let end = pos + run.length;
            while start < end {
                let row = (start / w) as usize;
                if row >= height {
                    break;
                }
                let row_end = (row as u64 + 1) * w;
                let span_end = end.min(row_end);
                rows[row].push(((start % w) as u32, ((span_end - 1) % w + 1) as u32));
                start = span_end;
            }
        }
        pos += run.length;
    }
    rows
}

fn spans_touch(a: (u32, u32), b: (u32, u32), slack: u32) -> bool {
    a.0 < b.1.saturating_add(slack) && b.0 < a.1.saturating_add(slack)
}

fn find_root(parent: &mut [u32], mut index: u32) -> u32 {
    while parent[index as usize] != index {
        parent[index as usize] = parent[parent[index as usize] as usize];
        index = parent[index as usize];
    }
    index
}

fn union(parent: &mut [u32], a: u32, b: u32) {
    let (ra, rb) = (find_root(parent, a), find_root(parent, b));
    if ra != rb {
        parent[ra as usize] = rb;
    }
}

struct IslandReport {
    layer: usize,
    x_mm: f32,
    y_mm: f32,
    area_px: u64,
}

/// Unsupported islands: a connected component of layer N with no material
/// within one pixel of it anywhere in layer N-1 starts printing in mid-air.
/// Layer 0 rests on the plate and cannot contain one. Supports count as
/// material because they are sliced into the same masks.
fn detect_islands(config: &TsConfig, layers: &[Layer]) -> Vec<IslandReport> {
    let width = config.resolution_x as usize;
    let height = config.resolution_y as usize;
    let px_x = config.platform_x_mm / config.resolution_x as f32;
    let px_y = config.platform_y_mm / config.resolution_y as f32;
    let mut islands = Vec::new();
    let mut previous: Option<Vec<Vec<(u32, u32)>>> = None;

    for (layer_index, layer) in layers.iter().enumerate() {
        let rows = lit_spans(&layer.data, width, height);
        if let Some(prev) = &previous {
            // Span ids flatten row-major; offsets[y] is the id of rows[y][0].
            let mut offsets = Vec::with_capacity(height + 1);
            let mut total = 0_u32;
            for row in &rows {
                offsets.push(total);
                total += row.len() as u32;
            }
            offsets.push(total);
            let mut parent: Vec<u32> = (0..total).collect();

            // Vertical connectivity between consecutive rows, two-pointer over
            // the sorted spans; slack 1 gives 8-connectivity.
            for y in 1..height {
                let (mut i, mut j) = (0_usize, 0_usize);
                while i < rows[y].len() && j < rows[y - 1].len() {
                    let a = rows[y][i];
                    let b = rows[y - 1][j];
                    if spans_touch(a, b, 1) {
                        union(&mut parent, offsets[y] + i as u32, offsets[y - 1] + j as u32);
                    }
                    if a.1 <= b.1 { i += 1 } else { j += 1 }
                }
            }

            // A span is grounded when the previous layer has material within
            // one pixel in its own row or either neighbouring row.
            let mut grounded = vec![false; total as usize];
            for y in 0..height {
                for (i, span) in rows[y].iter().enumerate() {
                    let id = (offsets[y] + i as u32) as usize;
                    'search: for ny in y.saturating_sub(1)..(y + 2).min(height) {
                        for below in &prev[ny] {
                            if below.0 >= span.1.saturating_add(1) {
                                break;
                            }
                            if spans_touch(*span, *below, 1) {
                                grounded[id] = true;
                                break 'search;
                            }
                        }
                    }
                }
            }

            #[derive(Default, Clone)]
            struct Component {
                grounded: bool,
                area: u64,
                x_weighted: f64,
                y_weighted: f64,
            }
            let mut components = vec![Component::default(); total as usize];
            for y in 0..height {
                for (i, span) in rows[y].iter().enumerate() {
                    let id = offsets[y] + i as u32;
                    let root = find_root(&mut parent, id) as usize;
                    let len = u64::from(span.1 - span.0);
                    let component = &mut components[root];
                    component.grounded |= grounded[id as usize];
                    component.area += len;
                    component.x_weighted += f64::from(span.0 + span.1) * 0.5 * len as f64;
                    component.y_weighted += y as f64 * len as f64;
                }
            }
            for (id, component) in components.iter().enumerate() {
                if component.area == 0 || component.grounded {
                    continue;
                }
                if find_root(&mut parent, id as u32) as usize != id {
                    continue;
                }
                islands.push(IslandReport {
                    layer: layer_index,
                    x_mm: (component.x_weighted / component.area as f64) as f32 * px_x,
                    y_mm: (component.y_weighted / component.area as f64) as f32 * px_y,
                    area_px: component.area,
                });
            }
        }
        previous = Some(rows);
    }
    islands
}

/// Complement of the lit spans per row: the air spans `[start, end)`.
fn gap_spans(rows: &[Vec<(u32, u32)>], width: u32) -> Vec<Vec<(u32, u32)>> {
    rows.iter()
        .map(|row| {
            let mut gaps = Vec::with_capacity(row.len() + 1);
            let mut cursor = 0_u32;
            for span in row {
                if span.0 > cursor {
                    gaps.push((cursor, span.0));
                }
                cursor = cursor.max(span.1);
            }
            if cursor < width {
                gaps.push((cursor, width));
            }
            gaps
        })
        .collect()
}

struct VoidReport {
    /// true: suction cup (vacuum against the film on every peel);
    /// false: sealed void (traps uncured resin on all sides).
    cup: bool,
    bottom_layer: usize,
    top_layer: usize,
    x_mm: f32,
    y_mm: f32,
    volume_px: u64,
}

/// Air pockets that cannot vent, on the same compensated masks as island
/// detection. A pocket is a stack of 2D-enclosed air regions (fully
/// surrounded by material within their layer) linked across layers by
/// vertical overlap. The print hangs from the plate and grows toward the
/// film, so in model coordinates the film side is the top of the stack:
///
/// - a pocket still open at the top of the model faces the film, and every
///   peel pulls a vacuum on it - a suction cup. A pocket that is enclosed
///   already at layer 0 has the plate for a lid and caps the same way.
/// - a pocket with a material floor and a material cap never vents at all
///   and traps uncured resin - a sealed void.
/// - a pocket whose air reaches the raster border in any layer, or that
///   opens downward onto open air above the plate, can equalize sideways
///   and is not reported. A drilled drain hole vents exactly this way: at
///   the hole's layers the interior air connects to the outside, so the
///   region stops being enclosed there.
fn detect_trapped_voids(config: &TsConfig, layers: &[Layer]) -> Vec<VoidReport> {
    let width = config.resolution_x as usize;
    let height = config.resolution_y as usize;
    let px_x = config.platform_x_mm / config.resolution_x as f32;
    let px_y = config.platform_y_mm / config.resolution_y as f32;

    #[derive(Clone, Default)]
    struct Pocket {
        vented: bool,
        plate_sealed: bool,
        bottom_layer: usize,
        volume_px: u64,
        /// Top cross-section spans as (row, start, end), sorted by row.
        top: Vec<(u32, u32, u32)>,
        top_area: u64,
        top_x_weighted: f64,
        top_y_weighted: f64,
    }

    let mut pockets: Vec<Pocket> = Vec::new();
    let mut previous_gaps: Option<Vec<Vec<(u32, u32)>>> = None;
    let mut reports: Vec<VoidReport> = Vec::new();

    let mut finish = |pocket: &Pocket, top_layer: usize, cup: bool| {
        if pocket.vented || pocket.top_area == 0 {
            return;
        }
        reports.push(VoidReport {
            cup,
            bottom_layer: pocket.bottom_layer,
            top_layer,
            x_mm: (pocket.top_x_weighted / pocket.top_area as f64) as f32 * px_x,
            y_mm: (pocket.top_y_weighted / pocket.top_area as f64) as f32 * px_y,
            volume_px: pocket.volume_px,
        });
    };

    for (layer_index, layer) in layers.iter().enumerate() {
        let gaps = gap_spans(&lit_spans(&layer.data, width, height), width as u32);

        // Label this layer's air with 4-connectivity (slack 0): the
        // complement of 8-connected material must not leak through
        // diagonal corners.
        let mut offsets = Vec::with_capacity(height + 1);
        let mut total = 0_u32;
        for row in &gaps {
            offsets.push(total);
            total += row.len() as u32;
        }
        offsets.push(total);
        let mut parent: Vec<u32> = (0..total).collect();
        for y in 1..height {
            let (mut i, mut j) = (0_usize, 0_usize);
            while i < gaps[y].len() && j < gaps[y - 1].len() {
                let a = gaps[y][i];
                let b = gaps[y - 1][j];
                if spans_touch(a, b, 0) {
                    union(&mut parent, offsets[y] + i as u32, offsets[y - 1] + j as u32);
                }
                if a.1 <= b.1 { i += 1 } else { j += 1 }
            }
        }

        // Cluster the carried pockets with this layer's air components:
        // ids [0, pockets) are pockets, [pockets, pockets + total) spans.
        // Air that stays connected through lower layers is one pocket, so
        // splits and merges both resolve to a single cluster.
        let pocket_count = pockets.len() as u32;
        let mut cluster: Vec<u32> = (0..pocket_count + total).collect();
        let mut prev_rows: Vec<Vec<(u32, u32, u32)>> = vec![Vec::new(); height];
        for (pocket_id, pocket) in pockets.iter().enumerate() {
            for &(y, s, e) in &pocket.top {
                prev_rows[y as usize].push((s, e, pocket_id as u32));
            }
        }
        for row in &mut prev_rows {
            row.sort_unstable();
        }
        for y in 0..height {
            let (mut i, mut j) = (0_usize, 0_usize);
            while i < gaps[y].len() && j < prev_rows[y].len() {
                let a = gaps[y][i];
                let (bs, be, pocket_id) = prev_rows[y][j];
                if spans_touch(a, (bs, be), 0) {
                    union(&mut cluster, pocket_count + offsets[y] + i as u32, pocket_id);
                }
                if a.1 <= be { i += 1 } else { j += 1 }
            }
        }

        // Resolve span roots within the layer into the cluster forest too.
        for id in 0..total {
            let root = find_root(&mut parent, id);
            if root != id {
                union(&mut cluster, pocket_count + id, pocket_count + root);
            }
        }

        #[derive(Clone, Default)]
        struct Next {
            open2d: bool,
            linked_air_below: bool,
            pocket: Pocket,
            seen: bool,
        }
        let mut next: std::collections::HashMap<u32, Next> =
            std::collections::HashMap::new();

        // Fold this layer's spans into their clusters.
        for y in 0..height {
            for (i, span) in gaps[y].iter().enumerate() {
                let id = pocket_count + offsets[y] + i as u32;
                let root = find_root(&mut cluster, id);
                let entry = next.entry(root).or_default();
                entry.seen = true;
                let len = u64::from(span.1 - span.0);
                if span.0 == 0
                    || span.1 == width as u32
                    || y == 0
                    || y == height - 1
                {
                    entry.open2d = true;
                }
                entry.pocket.top.push((y as u32, span.0, span.1));
                entry.pocket.top_area += len;
                entry.pocket.top_x_weighted +=
                    f64::from(span.0 + span.1) * 0.5 * len as f64;
                entry.pocket.top_y_weighted += y as f64 * len as f64;
                entry.pocket.volume_px += len;
            }
        }

        // Fold the carried pockets into their clusters; a pocket whose
        // cluster has no span this layer is capped by material above.
        let carried = mem::take(&mut pockets);
        for (pocket_id, pocket) in carried.into_iter().enumerate() {
            let root = find_root(&mut cluster, pocket_id as u32);
            match next.get_mut(&root) {
                Some(entry) if entry.seen => {
                    entry.pocket.vented |= pocket.vented;
                    entry.pocket.plate_sealed |= pocket.plate_sealed;
                    entry.pocket.volume_px += pocket.volume_px;
                    entry.pocket.bottom_layer = if entry.linked_air_below {
                        entry.pocket.bottom_layer.min(pocket.bottom_layer)
                    } else {
                        pocket.bottom_layer
                    };
                    entry.linked_air_below = true;
                }
                _ => finish(&pocket, layer_index.saturating_sub(1), pocket.plate_sealed),
            }
        }

        for (_, mut entry) in next.into_iter() {
            if !entry.seen {
                continue;
            }
            if !entry.linked_air_below {
                // Born this layer: the plate is the lid at layer 0; open
                // air below vents it; a material floor keeps it sealed.
                entry.pocket.bottom_layer = layer_index;
                if layer_index == 0 {
                    entry.pocket.plate_sealed = true;
                } else if let Some(prev) = &previous_gaps {
                    'birth: for &(y, s, e) in &entry.pocket.top {
                        for below in &prev[y as usize] {
                            if below.0 >= e {
                                break;
                            }
                            if spans_touch((s, e), *below, 0) {
                                entry.pocket.vented = true;
                                break 'birth;
                            }
                        }
                    }
                }
            }
            if entry.open2d {
                // Connected to the raster border: open air, not a pocket.
                continue;
            }
            pockets.push(entry.pocket);
        }

        previous_gaps = Some(gaps);
    }

    // Still open at the top of the model: the opening faces the film.
    let last = layers.len().saturating_sub(1);
    for pocket in &pockets {
        finish(pocket, last, true);
    }
    reports.sort_by_key(|report| report.bottom_layer);
    reports
}

fn transform_layers(config: &TsConfig, layers: Vec<Layer>) -> Vec<Layer> {
    if config.orientation == 0 && config.mirror_x == 0 && config.mirror_y == 0 {
        return layers;
    }
    let source_width = config.resolution_x as usize;
    let source_height = config.resolution_y as usize;
    let (output_width, output_height) = output_dimensions(config);
    layers
        .into_iter()
        .map(|layer| {
            let source = rle::decode_vec(layer.data.iter().copied());
            let mut output = vec![0_u8; source.len()];
            for y in 0..source_height {
                for x in 0..source_width {
                    let (mut output_x, mut output_y) = if config.orientation == 1 {
                        (source_height - 1 - y, x)
                    } else {
                        (x, y)
                    };
                    if config.mirror_x == 1 {
                        output_x = output_width - 1 - output_x;
                    }
                    if config.mirror_y == 1 {
                        output_y = output_height - 1 - output_y;
                    }
                    output[output_y * output_width + output_x] = source[y * source_width + x];
                }
            }
            Layer {
                data: encode_pixels(&output),
                height: layer.height,
                exposure: layer.exposure,
            }
        })
        .collect()
}

fn output_config(config: &TsConfig, source: &SliceConfig) -> SliceConfig {
    let mut output = source.clone();
    if config.orientation == 1 {
        output.platform_resolution = Vector2::new(config.resolution_y, config.resolution_x);
        output.platform_size = Vector3::new(
            Milimeters::new(config.platform_y_mm),
            Milimeters::new(config.platform_x_mm),
            Milimeters::new(config.platform_z_mm),
        );
    }
    output
}

fn transform_vector_layers(config: &TsConfig, layers: Vec<VectorLayer>) -> Vec<VectorLayer> {
    let source_size = Vector2::new(config.platform_x_mm, config.platform_y_mm);
    let source_resolution = Vector2::new(config.resolution_x as f32, config.resolution_y as f32);
    let output_size = if config.orientation == 1 {
        Vector2::new(source_size.y, source_size.x)
    } else {
        source_size
    };
    layers
        .into_iter()
        .map(|layer| {
            layer
                .into_iter()
                .map(|polygon| {
                    polygon
                        .into_iter()
                        .map(|point| {
                            let mm = point
                                .component_mul(&source_size)
                                .component_div(&source_resolution);
                            let mut output = if config.orientation == 1 {
                                Vector2::new(source_size.y - mm.y, mm.x)
                            } else {
                                mm
                            };
                            if config.mirror_x == 1 {
                                output.x = output_size.x - output.x;
                            }
                            if config.mirror_y == 1 {
                                output.y = output_size.y - output.y;
                            }
                            output
                        })
                        .collect()
                })
                .collect()
        })
        .collect()
}

fn set_error(state: &mut EngineState, status: TsStatus, message: impl Into<String>) -> TsStatus {
    let message = message.into();
    state.last_status = status;
    state.last_message = message.clone();
    state.diagnostics.push(Diagnostic {
        severity: "error",
        code: status as u32,
        message,
    });
    status
}

fn set_ok(state: &mut EngineState) -> TsStatus {
    state.last_status = TsStatus::Ok;
    state.last_message.clear();
    TsStatus::Ok
}

fn catch_status(action: impl FnOnce() -> TsStatus) -> TsStatus {
    catch_unwind(AssertUnwindSafe(action)).unwrap_or(TsStatus::Panic)
}

unsafe fn engine_ref<'a>(engine: *const TsEngine) -> Option<&'a TsEngine> {
    unsafe { engine.as_ref() }
}

fn copy_message(text: &str, output: *mut c_char, capacity: usize) -> usize {
    let required = text.len() + 1;
    if !output.is_null() && capacity > 0 {
        let count = text.len().min(capacity - 1);
        unsafe {
            ptr::copy_nonoverlapping(text.as_ptr(), output.cast::<u8>(), count);
            *output.add(count) = 0;
        }
    }
    required
}

fn owned_buffer(mut data: Vec<u8>) -> TsBuffer {
    let output = TsBuffer {
        data: data.as_mut_ptr(),
        size: data.len(),
        capacity: data.capacity(),
    };
    mem::forget(data);
    output
}

fn lit_voxels(layers: &[Layer]) -> u64 {
    layers
        .iter()
        .flat_map(|layer| layer.data.iter())
        .filter(|run| run.value != 0)
        .map(|run| run.length)
        .sum()
}

fn inspect_output_bytes(
    state: &EngineState,
    format: u32,
    data: &[u8],
) -> Result<serde_json::Value, String> {
    if data.is_empty() || data.len() > MAX_OUTPUT_BYTES {
        return Err("encoded output is empty or exceeds the inspection safety limit".into());
    }
    let expected_layers = state.layers.as_ref().map_or(0, Vec::len);
    let (expected_x, expected_y) = output_dimensions(&state.config_ffi);
    let expected_pixels = expected_x
        .checked_mul(expected_y)
        .ok_or_else(|| "expected layer size overflow".to_string())?
        as u64;

    let report = match format {
        value if value == TsFormat::Ctb as u32 => {
            if data.len() < 1024 || !data.starts_with(&0x12FD0107_u32.to_le_bytes()) {
                return Err("CTB file is truncated or has an invalid v5 magic".into());
            }
            let mut des = SliceDeserializer::new(data);
            let file = ctb_format::File::deserialize(&mut des).map_err(|e| e.to_string())?;
            if file.layers.len() != expected_layers
                || file.resolution.x as usize != expected_x
                || file.resolution.y as usize != expected_y
                || (file.layer_height.raw() - state.config_ffi.layer_height_mm).abs() > 1.0e-5
                || (file.exposure_time.raw() - state.config_ffi.normal.exposure_seconds).abs()
                    > 1.0e-5
                || (file.bottom_exposure_time.raw() - state.config_ffi.first.exposure_seconds).abs()
                    > 1.0e-5
                || file.bottom_layer_count != state.config_ffi.first_layers
                || file.transition_layer_count != state.config_ffi.transition_layers
            {
                return Err(
                    "CTB dimensions or exposure metadata do not match the active job".into(),
                );
            }
            for layer in &file.layers {
                let pixels: u64 = ctb_format::LayerDecoder::new(&layer.data)
                    .map(|run| run.length)
                    .sum();
                if pixels != expected_pixels {
                    return Err("CTB layer payload has an invalid decoded pixel count".into());
                }
            }
            let preview = file.large_preview.size();
            if preview.x == 0 || preview.y == 0 || file.small_preview.size().x == 0 {
                return Err("CTB preview metadata is empty".into());
            }
            json!({"format":"ctb","version":5,"compatibility":"CTB v5 structural","layers":file.layers.len(),"resolution":[expected_x,expected_y],"thumbnail":[preview.x,preview.y],"integrity":"encrypted settings/signature and previews decoded"})
        }
        value if value == TsFormat::Goo as u32 => {
            const GOO_END: &[u8] = &[0, 0, 0, 7, 0, 0, 0, 0x44, 0x4c, 0x50, 0];
            if data.len() < 1024 || !data.ends_with(GOO_END) {
                return Err("GOO file is truncated or missing its end marker".into());
            }
            let mut des = SliceDeserializer::new(data);
            let file = goo_format::File::deserialize(&mut des).map_err(|e| e.to_string())?;
            if file.layers.len() != expected_layers
                || file.header.x_resolution as usize != expected_x
                || file.header.y_resolution as usize != expected_y
                || file.header.version.to_string() != "V3.0"
                || (file.header.layer_thickness.raw() - state.config_ffi.layer_height_mm).abs()
                    > 1.0e-5
                || file.header.bottom_layers != state.config_ffi.first_layers
                || u32::from(file.header.transition_layers) != state.config_ffi.transition_layers
            {
                return Err(
                    "GOO version, dimensions, or layer metadata do not match the active job".into(),
                );
            }
            for layer in &file.layers {
                let decoder = goo_format::LayerDecoder::new(&layer.data);
                if decoder.checksum() != layer.checksum {
                    return Err("GOO layer checksum mismatch".into());
                }
                let pixels: u64 = decoder.map(|run| run.length).sum();
                if pixels != expected_pixels {
                    return Err("GOO layer payload has an invalid decoded pixel count".into());
                }
            }
            if file.header.small_preview.inner_data().len() != 116 * 116
                || file.header.big_preview.inner_data().len() != 290 * 290
            {
                return Err("GOO fixed preview dimensions are invalid".into());
            }
            json!({"format":"goo","version":"V3.0","compatibility":"Elegoo GOO v3 structural","layers":file.layers.len(),"resolution":[expected_x,expected_y],"thumbnail":[290,290],"integrity":"all layer checksums and previews verified"})
        }
        value if value == TsFormat::NanoDlp as u32 => {
            let file =
                nanodlp_format::File::deserialize(Cursor::new(data)).map_err(|e| e.to_string())?;
            if file.layers.len() != expected_layers
                || file.options.p_width as usize != expected_x
                || file.options.p_height as usize != expected_y
                || file.layer_info.len() != expected_layers
            {
                return Err(
                    "NanoDLP members, layer count, or resolution do not match the active job"
                        .into(),
                );
            }
            let nanodlp_pixels = expected_x.div_ceil(3) * 3 * expected_y;
            for layer in &file.layers {
                let pixels: u64 = nanodlp_format::LayerDecoder::new(layer)
                    .runs()
                    .map(|run| run.length)
                    .sum();
                if pixels != nanodlp_pixels as u64 {
                    return Err("NanoDLP PNG layer has an invalid decoded pixel count".into());
                }
            }
            if file.preview.width() == 0 || file.preview.height() == 0 {
                return Err("NanoDLP preview is empty".into());
            }
            json!({"format":"nanodlp","version":"documented archive","compatibility":"NanoDLP documented field subset","layers":file.layers.len(),"resolution":[expected_x,expected_y],"thumbnail":[file.preview.width(),file.preview.height()],"integrity":"ZIP, JSON, preview and layer PNG members decoded"})
        }
        value if value == TsFormat::Svg as u32 => {
            let text =
                std::str::from_utf8(data).map_err(|_| "SVG output is not UTF-8".to_string())?;
            if !text.starts_with("<svg") || text.matches("<rect").count() != expected_layers {
                return Err("SVG structure or native vector layer count is invalid".into());
            }
            json!({"format":"svg","version":"SVG 1.x XML","compatibility":"diagnostic vector layer sheet; not a printer file","layers":expected_layers,"physical_sheet":true,"integrity":"UTF-8 structure and layer frames verified"})
        }
        _ => return Err("unknown output format".into()),
    };
    Ok(report)
}

#[unsafe(no_mangle)]
pub extern "C" fn ts_mslicer_abi_version() -> u32 {
    ABI_VERSION
}

#[unsafe(no_mangle)]
pub extern "C" fn ts_mslicer_engine_version() -> *const c_char {
    ENGINE_VERSION.as_ptr().cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn ts_mslicer_capabilities() -> u64 {
    (1 << 15) - 1
}

fn remote_probe_status(error: &ProbeError) -> TsStatus {
    match error {
        ProbeError::InvalidAddress
        | ProbeError::MalformedResponse
        | ProbeError::InvalidIdentity
        | ProbeError::IdentityMismatch
        | ProbeError::OversizedResponse => TsStatus::InvalidArgument,
        ProbeError::Cancelled => TsStatus::Cancelled,
        ProbeError::ResolutionMismatch => TsStatus::InvalidConfig,
        ProbeError::FileTransferUnsupported => TsStatus::Unsupported,
        ProbeError::Timeout | ProbeError::WrongSource | ProbeError::Io(_) => TsStatus::EngineError,
    }
}

#[unsafe(no_mangle)]
/// Probe one explicitly configured private-LAN SDCP endpoint. No network I/O
/// occurs before this call. `request` and zero-initialized `result_json` must
/// remain exclusively owned by the caller for the duration of the call.
pub unsafe extern "C" fn ts_mslicer_remote_probe(
    request: *const TsRemoteProbeRequest,
    result_json: *mut TsBuffer,
) -> TsStatus {
    catch_status(|| {
        let (Some(request), Some(output)) =
            (unsafe { request.as_ref() }, unsafe { result_json.as_mut() })
        else {
            return TsStatus::InvalidArgument;
        };
        if request.struct_size as usize != mem::size_of::<TsRemoteProbeRequest>()
            || request.address.is_null()
            || request.address_size == 0
            || request.address_size > 15
            || request.port == 0
            || request.reserved != 0
            || request.resolution_x == 0
            || request.resolution_x > u16::MAX as u32
            || request.resolution_y == 0
            || request.resolution_y > u16::MAX as u32
            || !(1..=120_000).contains(&request.timeout_ms)
            || !output.data.is_null()
            || output.size != 0
            || output.capacity != 0
        {
            return TsStatus::InvalidArgument;
        }
        let address_bytes = unsafe { slice::from_raw_parts(request.address, request.address_size) };
        let address = match std::str::from_utf8(address_bytes)
            .ok()
            .and_then(|text| text.parse::<Ipv4Addr>().ok())
        {
            Some(address) => address,
            None => return TsStatus::InvalidArgument,
        };
        let cancelled = || {
            request
                .cancelled
                .is_some_and(|callback| unsafe { callback(request.user_data) != 0 })
        };
        let result = probe_printer_with_cancel(
            SocketAddrV4::new(address, request.port),
            Resolution {
                x: request.resolution_x as u16,
                y: request.resolution_y as u16,
            },
            std::time::Duration::from_millis(request.timeout_ms.into()),
            cancelled,
        );
        match result {
            Ok(identity) => {
                let json = serde_json::to_vec(&json!({
                    "name": identity.name,
                    "machine_name": identity.machine_name,
                    "mainboard_id": identity.mainboard_id,
                    "firmware_version": identity.firmware_version,
                    "protocol_version": identity.protocol_version,
                    "resolution": [identity.resolution.x, identity.resolution.y],
                    "file_transfer": identity.file_transfer,
                    "print_control": identity.print_control,
                }))
                .unwrap_or_else(|_| b"{\"error\":\"identity serialization failed\"}".to_vec());
                *output = owned_buffer(json);
                TsStatus::Ok
            }
            Err(error) => {
                let status = remote_probe_status(&error);
                *output = owned_buffer(error.to_string().into_bytes());
                status
            }
        }
    })
}

fn remote_upload_status(error: &SessionError) -> TsStatus {
    match error {
        SessionError::Cancelled => TsStatus::Cancelled,
        SessionError::Probe(ProbeError::InvalidAddress)
        | SessionError::Probe(ProbeError::MalformedResponse)
        | SessionError::Probe(ProbeError::OversizedResponse)
        | SessionError::Probe(ProbeError::InvalidIdentity)
        | SessionError::Probe(ProbeError::IdentityMismatch) => TsStatus::InvalidArgument,
        SessionError::Probe(ProbeError::ResolutionMismatch) => TsStatus::InvalidConfig,
        SessionError::Probe(ProbeError::FileTransferUnsupported) => TsStatus::Unsupported,
        SessionError::Artifact(remote_print::artifact::ArtifactError::InvalidArtifact)
        | SessionError::Artifact(remote_print::artifact::ArtifactError::InvalidFilename)
        | SessionError::Protocol(_) => TsStatus::InvalidArgument,
        SessionError::Artifact(remote_print::artifact::ArtifactError::Cancelled) => {
            TsStatus::Cancelled
        }
        SessionError::Probe(ProbeError::Cancelled) => TsStatus::Cancelled,
        SessionError::Probe(_)
        | SessionError::Artifact(_)
        | SessionError::Timeout(_)
        | SessionError::Io(_)
        | SessionError::Transfer(_) => TsStatus::EngineError,
    }
}

#[unsafe(no_mangle)]
/// Upload one immutable CTB/GOO artifact to an explicitly configured private
/// SDCP endpoint. The call is synchronous and owns no resources after return.
pub unsafe extern "C" fn ts_mslicer_remote_upload(
    request: *const TsRemoteUploadRequest,
    result_json: *mut TsBuffer,
) -> TsStatus {
    catch_status(|| {
        let (Some(request), Some(output)) =
            (unsafe { request.as_ref() }, unsafe { result_json.as_mut() })
        else {
            return TsStatus::InvalidArgument;
        };
        if request.struct_size as usize != mem::size_of::<TsRemoteUploadRequest>()
            || request.address.is_null()
            || request.address_size == 0
            || request.address_size > 15
            || request.filename.is_null()
            || request.filename_size == 0
            || request.filename_size > 128
            || request.artifact.is_null()
            || request.artifact_size == 0
            || request.artifact_size > u32::MAX as usize
            || request.port == 0
            || request.reserved != 0
            || request.resolution_x == 0
            || request.resolution_x > u16::MAX as u32
            || request.resolution_y == 0
            || request.resolution_y > u16::MAX as u32
            || !(1..=600_000).contains(&request.probe_timeout_ms)
            || !(1..=600_000).contains(&request.connect_timeout_ms)
            || !(1..=600_000).contains(&request.transfer_timeout_ms)
            || !output.data.is_null()
            || output.size != 0
            || output.capacity != 0
        {
            return TsStatus::InvalidArgument;
        }

        let address = match std::str::from_utf8(unsafe {
            slice::from_raw_parts(request.address, request.address_size)
        })
        .ok()
        .and_then(|text| text.parse::<Ipv4Addr>().ok())
        {
            Some(value) => value,
            None => return TsStatus::InvalidArgument,
        };
        let filename = match std::str::from_utf8(unsafe {
            slice::from_raw_parts(request.filename, request.filename_size)
        }) {
            Ok(value) => value.to_owned(),
            Err(_) => return TsStatus::InvalidArgument,
        };
        let artifact: Arc<[u8]> =
            Arc::from(unsafe { slice::from_raw_parts(request.artifact, request.artifact_size) });
        let cancelled_callback = request.cancelled;
        let progress_callback = request.progress;
        let user_data = request.user_data as usize;
        let result = upload_artifact_with_cancel(
            UploadRequest {
                target: SocketAddrV4::new(address, request.port),
                resolution: Resolution {
                    x: request.resolution_x as u16,
                    y: request.resolution_y as u16,
                },
                filename,
                artifact,
                probe_timeout: std::time::Duration::from_millis(request.probe_timeout_ms.into()),
                connect_timeout: std::time::Duration::from_millis(
                    request.connect_timeout_ms.into(),
                ),
                transfer_timeout: std::time::Duration::from_millis(
                    request.transfer_timeout_ms.into(),
                ),
            },
            || {
                cancelled_callback
                    .is_some_and(|callback| unsafe { callback(user_data as *mut c_void) != 0 })
            },
            move |done, total| {
                if let Some(callback) = progress_callback {
                    unsafe { callback(user_data as *mut c_void, done, total) }
                }
            },
        );
        match result {
            Ok(receipt) => {
                let json = serde_json::to_vec(&json!({
                    "name": receipt.printer.name,
                    "machine_name": receipt.printer.machine_name,
                    "mainboard_id": receipt.printer.mainboard_id,
                    "filename": receipt.filename,
                    "bytes": receipt.bytes,
                    "md5": receipt.md5,
                }))
                .unwrap_or_else(|_| {
                    b"{\"error\":\"upload receipt serialization failed\"}".to_vec()
                });
                *output = owned_buffer(json);
                TsStatus::Ok
            }
            Err(error) => {
                let status = remote_upload_status(&error);
                *output = owned_buffer(error.to_string().into_bytes());
                status
            }
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ts_mslicer_status_name(status: u32) -> *const c_char {
    const NAMES: [&[u8]; 10] = [
        b"ok\0",
        b"invalid argument\0",
        b"invalid config\0",
        b"invalid mesh\0",
        b"cancelled\0",
        b"not ready\0",
        b"buffer too small\0",
        b"unsupported\0",
        b"engine error\0",
        b"panic contained\0",
    ];
    NAMES
        .get(status as usize)
        .copied()
        .unwrap_or(b"unknown\0")
        .as_ptr()
        .cast()
}

#[unsafe(no_mangle)]
/// # Safety
/// `config` must point to a readable ABI-6 config. `message`, when non-null,
/// must be writable for `message_capacity` bytes.
pub unsafe extern "C" fn ts_mslicer_config_validate(
    config: *const TsConfig,
    message: *mut c_char,
    message_capacity: usize,
) -> TsStatus {
    catch_status(|| {
        let Some(config) = (unsafe { config.as_ref() }) else {
            copy_message("configuration pointer is null", message, message_capacity);
            return TsStatus::InvalidArgument;
        };
        match validate_config(config) {
            Ok(()) => {
                copy_message("", message, message_capacity);
                TsStatus::Ok
            }
            Err(error) => {
                copy_message(&error, message, message_capacity);
                TsStatus::InvalidConfig
            }
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `config` must point to a readable ABI-6 config for the duration of the call.
pub unsafe extern "C" fn ts_mslicer_engine_create(config: *const TsConfig) -> *mut TsEngine {
    catch_unwind(AssertUnwindSafe(|| {
        let config = unsafe { config.as_ref() }?;
        validate_config(config).ok()?;
        Some(Box::into_raw(Box::new(TsEngine {
            state: Mutex::new(EngineState {
                config_ffi: *config,
                config: convert_config(config),
                models: Vec::new(),
                physical_models: Vec::new(),
                support_blockers: Vec::new(),
                support_enforcers: Vec::new(),
                manual_support_points: Vec::new(),
                manual_support_only: false,
                prepared_support: None,
                layers: None,
                last_status: TsStatus::Ok,
                last_message: String::new(),
                diagnostics: Vec::new(),
            }),
            progress: Progress::new(),
            cancelled: Arc::new(AtomicBool::new(false)),
        })))
    }))
    .ok()
    .flatten()
    .unwrap_or(ptr::null_mut())
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live handle returned by `ts_mslicer_engine_create`
/// that has not already been destroyed.
pub unsafe extern "C" fn ts_mslicer_engine_destroy(engine: *mut TsEngine) {
    if !engine.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(engine)) }));
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live engine handle.
pub unsafe extern "C" fn ts_mslicer_engine_last_status(engine: *const TsEngine) -> TsStatus {
    let Some(engine) = (unsafe { engine_ref(engine) }) else {
        return TsStatus::InvalidArgument;
    };
    engine
        .state
        .lock()
        .map(|state| state.last_status)
        .unwrap_or(TsStatus::Panic)
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or live. `message`, when non-null, must be writable
/// for `message_capacity` bytes.
pub unsafe extern "C" fn ts_mslicer_engine_last_message(
    engine: *const TsEngine,
    message: *mut c_char,
    message_capacity: usize,
) -> usize {
    let Some(engine) = (unsafe { engine_ref(engine) }) else {
        return copy_message("engine pointer is null", message, message_capacity);
    };
    match engine.state.lock() {
        Ok(state) => copy_message(&state.last_message, message, message_capacity),
        Err(_) => copy_message("engine state lock is poisoned", message, message_capacity),
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. `mesh` and every buffer described by it must remain
/// readable for the duration of this call and match its declared counts.
pub unsafe extern "C" fn ts_mslicer_engine_add_mesh(
    engine: *mut TsEngine,
    mesh: *const TsMesh,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(mesh)) = (unsafe { engine_ref(engine) }, unsafe { mesh.as_ref() })
        else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        if mesh.struct_size as usize != mem::size_of::<TsMesh>() {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh structure size does not match ABI 6",
            );
        }
        if mesh.role > 2 {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh role must be 0 (model), 1 (support blocker) or 2 (support enforcer)",
            );
        }
        if mesh.vertex_count == 0
            || mesh.triangle_count == 0
            || mesh.vertex_count > MAX_VERTICES
            || mesh.triangle_count > MAX_TRIANGLES
            || mesh.vertices_xyz.is_null()
            || mesh.triangles.is_null()
        {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh counts or pointers are invalid",
            );
        }
        let Some(vertex_values) = mesh.vertex_count.checked_mul(3) else {
            return set_error(&mut state, TsStatus::InvalidMesh, "vertex count overflow");
        };
        let Some(index_values) = mesh.triangle_count.checked_mul(3) else {
            return set_error(&mut state, TsStatus::InvalidMesh, "triangle count overflow");
        };
        let vertices = unsafe { slice::from_raw_parts(mesh.vertices_xyz, vertex_values) };
        let indices = unsafe { slice::from_raw_parts(mesh.triangles, index_values) };
        if vertices.iter().any(|value| !value.is_finite())
            || mesh.transform.iter().any(|value| !value.is_finite())
        {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh contains non-finite coordinates or transform values",
            );
        }
        if indices
            .iter()
            .any(|&index| index as usize >= mesh.vertex_count)
        {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "triangle index is outside the vertex array",
            );
        }
        if mesh.transform[3] != 0.0
            || mesh.transform[7] != 0.0
            || mesh.transform[11] != 0.0
            || mesh.transform[15] != 1.0
        {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh transform must be affine",
            );
        }
        let transform = Matrix4::from_column_slice(&mesh.transform);
        let vertices = vertices
            .chunks_exact(3)
            .map(|v| (transform * Vector3::new(v[0], v[1], v[2]).to_homogeneous()).xyz())
            .collect::<Vec<_>>();
        if vertices
            .iter()
            .any(|value| value.iter().any(|x| !x.is_finite()))
        {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh transform produced non-finite coordinates",
            );
        }
        // Real-world STLs routinely carry a few degenerate triangles - the
        // reference Benchy has them - and refusing the whole mesh for one
        // empty face made such models unusable. An empty triangle rasterizes
        // to nothing, so nothing is approximated by tolerating it:
        // - a face with a repeated index has no interior and no usable
        //   topology, and is dropped;
        // - a zero-area sliver with three distinct vertices is kept, because
        //   dropping it would tear the surface's edge pairing; the support
        //   generator guards against the NaN normal it produces.
        let mut dropped_faces = 0usize;
        let mut sliver_faces = 0usize;
        let faces = indices
            .chunks_exact(3)
            .map(|f| [f[0], f[1], f[2]])
            .filter(|face| {
                if face[0] == face[1] || face[1] == face[2] || face[0] == face[2] {
                    dropped_faces += 1;
                    return false;
                }
                let a = vertices[face[0] as usize];
                let b = vertices[face[1] as usize];
                let c = vertices[face[2] as usize];
                if (b - a).cross(&(c - a)).norm_squared() <= f32::EPSILON {
                    sliver_faces += 1;
                }
                true
            })
            .collect::<Vec<_>>();
        if faces.is_empty() {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "mesh contains no non-degenerate triangles",
            );
        }
        if dropped_faces != 0 || sliver_faces != 0 {
            state.diagnostics.push(Diagnostic {
                severity: "warning",
                code: 1002,
                message: format!(
                    "mesh contains degenerate triangles: {dropped_faces} with repeated vertices \
                     dropped, {sliver_faces} zero-area slivers kept"
                ),
            });
        }
        let min = vertices
            .iter()
            .fold(Vector3::repeat(f32::INFINITY), |a, b| a.inf(b));
        let max = vertices
            .iter()
            .fold(Vector3::repeat(f32::NEG_INFINITY), |a, b| a.sup(b));
        const BOUNDS_EPSILON_MM: f32 = 1.0e-4;
        if min.x < -BOUNDS_EPSILON_MM
            || min.y < -BOUNDS_EPSILON_MM
            || min.z < -BOUNDS_EPSILON_MM
            || max.x > state.config_ffi.platform_x_mm + BOUNDS_EPSILON_MM
            || max.y > state.config_ffi.platform_y_mm + BOUNDS_EPSILON_MM
            || max.z > state.config_ffi.platform_z_mm + BOUNDS_EPSILON_MM
        {
            state.diagnostics.push(Diagnostic {
                severity: "warning",
                code: 1001,
                message: format!(
                    "mesh bounds X {:.3}..{:.3}, Y {:.3}..{:.3}, Z {:.3}..{:.3} mm extend outside the configured resin build volume and will be clipped",
                    min.x, max.x, min.y, max.y, min.z, max.z
                ),
            });
        }
        if mesh.role != 0 {
            // A support-steering volume: kept in build-volume millimetres,
            // never sliced, consumed by automatic support generation.
            let region = SupportRegion {
                vertices,
                faces,
            };
            if mesh.role == 1 {
                state.support_blockers.push(region);
            } else {
                state.support_enforcers.push(region);
            }
            state.prepared_support = None;
            return set_ok(&mut state);
        }
        // MSlicer's scan converter operates in display-pixel X/Y coordinates
        // while Z remains millimeters. The public TigerSlicer ABI deliberately
        // accepts meshes and transforms in millimeters, so convert only after
        // validating the physical build-volume bounds above.
        let mm_to_pixels = Vector2::new(
            state.config_ffi.resolution_x as f32 / state.config_ffi.platform_x_mm,
            state.config_ffi.resolution_y as f32 / state.config_ffi.platform_y_mm,
        );
        let physical_mesh = Mesh::new_uncentred(vertices.clone(), faces.clone());
        let vertices = vertices
            .into_iter()
            .map(|vertex| {
                Vector3::new(
                    vertex.x * mm_to_pixels.x,
                    vertex.y * mm_to_pixels.y,
                    vertex.z,
                )
            })
            .collect();
        state.models.push(SlicerModel {
            mesh: Mesh::new_uncentred(vertices, faces),
            exposure: mesh.relative_exposure,
        });
        state.physical_models.push(physical_mesh);
        state.prepared_support = None;
        state.layers = None;
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live and `config` must remain readable for this call.
/// All scene meshes must have been added before this function is called.
pub unsafe extern "C" fn ts_mslicer_engine_generate_supports(
    engine: *mut TsEngine,
    config: *const TsSupportConfig,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(config)) =
            (unsafe { engine_ref(engine) }, unsafe { config.as_ref() })
        else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        if config.struct_size as usize != mem::size_of::<TsSupportConfig>() {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "support configuration structure size does not match ABI 8",
            );
        }
        let positive = [
            (config.support_radius_mm, "support radius"),
            (config.tip_radius_mm, "tip radius"),
            (config.tip_length_mm, "tip length"),
            (config.face_support_spacing_mm, "face support spacing"),
            (config.edge_support_spacing_mm, "edge support spacing"),
            (config.edge_angle_delta_radians, "edge angle delta"),
        ];
        if positive
            .iter()
            .any(|(value, _)| !value.is_finite() || *value <= 0.0)
        {
            let field = positive
                .iter()
                .find(|(value, _)| !value.is_finite() || *value <= 0.0)
                .map(|(_, name)| *name)
                .unwrap_or("support value");
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                format!("{field} must be finite and positive"),
            );
        }
        if !config.base_height_mm.is_finite()
            || config.base_height_mm < 0.0
            || !config.base_offset_mm.is_finite()
            || config.base_offset_mm < 0.0
            || !config.max_overhang_angle_degrees.is_finite()
            || !(0.0..90.0).contains(&config.max_overhang_angle_degrees)
            || config.precision < 4
            || config.precision > 64
            || config.tip_radius_mm > config.support_radius_mm
        {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "support dimensions, angle, or precision are outside the safe MSlicer range",
            );
        }
        // ABI 7 fields. Each is a quantity that may legitimately be zero -
        // "no penetration", "no minimum" - so they are checked separately
        // from the strictly positive set above rather than folded into it.
        let base = match config.base_kind {
            0 => SupportBase::None,
            1 => SupportBase::Raft,
            2 => SupportBase::Pads,
            other => {
                return set_error(
                    &mut state,
                    TsStatus::InvalidConfig,
                    format!("support base kind {other} is not one of none, raft, or pads"),
                );
            }
        };
        if !config.contact_penetration_mm.is_finite()
            || config.contact_penetration_mm < 0.0
            || config.contact_penetration_mm > config.tip_length_mm
            || !config.min_support_height_mm.is_finite()
            || config.min_support_height_mm < 0.0
            || !config.min_overhang_area_mm2.is_finite()
            || config.min_overhang_area_mm2 < 0.0
            || !config.base_diameter_mm.is_finite()
            || config.base_diameter_mm < 0.0
        {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "contact penetration, minimum support height, minimum overhang area, or base \
                 diameter is negative, not finite, or deeper than the tip is long",
            );
        }
        if base == SupportBase::Pads && config.base_diameter_mm <= 0.0 {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "individual support pads need a base diameter greater than zero",
            );
        }
        // ABI 8 fields. Refused by name rather than clamped, matching the
        // ABI 7 policy: a value the generator cannot honour must not be
        // silently approximated.
        if config.cross_bracing != 0
            && (!config.brace_spacing_mm.is_finite() || config.brace_spacing_mm <= 0.0)
        {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "cross bracing needs a brace spacing greater than zero",
            );
        }
        if config.adaptive_diameter != 0
            && (!config.max_pillar_diameter_mm.is_finite()
                || config.max_pillar_diameter_mm < config.support_radius_mm * 2.0)
        {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "the adaptive maximum pillar diameter must be at least the pillar diameter",
            );
        }
        if state.physical_models.is_empty() {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "add all printable scene meshes before generating supports",
            );
        }
        state.prepared_support = None;

        // Generate against a single scene mesh so a route cannot silently pass
        // through another printable object. MSlicer's support tool uses a
        // bed-centred workspace; TigerSlicer's public ABI uses build-volume
        // coordinates, so centre only for generation and translate the result
        // back before rasterization.
        let mut vertices = Vec::new();
        let mut faces = Vec::new();
        let centre = Vector2::new(
            state.config_ffi.platform_x_mm * 0.5,
            state.config_ffi.platform_y_mm * 0.5,
        );
        for mesh in &state.physical_models {
            if vertices.len().saturating_add(mesh.vertex_count()) > MAX_VERTICES
                || faces.len().saturating_add(mesh.face_count()) > MAX_TRIANGLES
            {
                return set_error(
                    &mut state,
                    TsStatus::InvalidMesh,
                    "combined scene is too large for automatic support analysis",
                );
            }
            let base = vertices.len() as u32;
            vertices.extend(mesh.vertices().iter().map(|vertex| {
                Vector3::new(vertex.x - centre.x, vertex.y - centre.y, vertex.z)
            }));
            faces.extend(
                mesh.faces()
                    .iter()
                    .map(|face| [face[0] + base, face[1] + base, face[2] + base]),
            );
        }
        let scene = Mesh::new_uncentred(vertices, faces);
        if !scene.is_manifold(Progress::new()) {
            return set_error(
                &mut state,
                TsStatus::Unsupported,
                "automatic MSLA supports require closed manifold model geometry",
            );
        }
        let half_edge = HalfEdgeMesh::build(scene.inner());
        let bvh = Bvh::build(scene.inner(), Progress::new());
        let native = SupportConfig {
            support_radius: config.support_radius_mm,
            tip_radius: config.tip_radius_mm,
            tip_length: config.tip_length_mm,
            precision: config.precision,
            raft_height: config.base_height_mm,
            raft_offset: config.base_offset_mm,
            max_angle: config.max_overhang_angle_degrees,
            face_support_spacing: config.face_support_spacing_mm,
            edge_support_spacing: config.edge_support_spacing_mm,
            edge_angle_delta: config.edge_angle_delta_radians,
            contact_penetration: config.contact_penetration_mm,
            min_support_height: config.min_support_height_mm,
            min_overhang_area: config.min_overhang_area_mm2,
            base,
            base_diameter: config.base_diameter_mm,
            on_model: config.support_on_model != 0,
            cross_bracing: config.cross_bracing != 0,
            brace_spacing: config.brace_spacing_mm,
            adaptive_radius: config.adaptive_diameter != 0,
            // The ABI carries a diameter; the generator works in radii.
            max_radius: config.max_pillar_diameter_mm * 0.5,
            // The generation scene is bed-centred; centre the painted
            // volumes the same way so containment tests line up.
            blockers: state
                .support_blockers
                .iter()
                .map(|region| SupportRegion {
                    vertices: region
                        .vertices
                        .iter()
                        .map(|v| Vector3::new(v.x - centre.x, v.y - centre.y, v.z))
                        .collect(),
                    faces: region.faces.clone(),
                })
                .collect(),
            enforcers: state
                .support_enforcers
                .iter()
                .map(|region| SupportRegion {
                    vertices: region
                        .vertices
                        .iter()
                        .map(|v| Vector3::new(v.x - centre.x, v.y - centre.y, v.z))
                        .collect(),
                    faces: region.faces.clone(),
                })
                .collect(),
            enforcers_only: config.enforcers_only != 0,
        };
        let bed_size = Vector3::new(
            state.config_ffi.platform_x_mm,
            state.config_ffi.platform_y_mm,
            state.config_ffi.platform_z_mm,
        )
        .map(Milimeters::new);
        // Manual points arrive in build-volume millimetres; the generation
        // scene is bed-centred, so centre them the same way.
        let manual_centred: Vec<Vector3<f32>> = state
            .manual_support_points
            .iter()
            .map(|p| Vector3::new(p.x - centre.x, p.y - centre.y, p.z))
            .collect();
        let manual_only = state.manual_support_only;
        let Some(supports) = SupportGenerator::new(&native, bed_size).generate_supports(
            &scene,
            &half_edge,
            &bvh,
            &manual_centred,
            manual_only,
        ) else {
            state.prepared_support = Some(Mesh::new_uncentred(Vec::new(), Vec::new()));
            state.diagnostics.push(Diagnostic {
                severity: "info",
                code: 1100,
                message: "automatic support analysis found no support geometry to add".into(),
            });
            return set_ok(&mut state);
        };
        let support_bounds = supports.vertices().iter().fold(
            (
                Vector3::repeat(f32::INFINITY),
                Vector3::repeat(f32::NEG_INFINITY),
            ),
            |(minimum, maximum), vertex| {
                (
                    minimum.zip_map(vertex, f32::min),
                    maximum.zip_map(vertex, f32::max),
                )
            },
        );
        const SUPPORT_BOUND_TOLERANCE_MM: f32 = 0.001;
        let nonfinite_vertices = supports
            .vertices()
            .iter()
            .filter(|vertex| vertex.iter().any(|coordinate| !coordinate.is_finite()))
            .count();
        if supports.vertex_count() > MAX_VERTICES
            || supports.face_count() > MAX_TRIANGLES
            || nonfinite_vertices != 0
            || support_bounds.0.x + centre.x < -SUPPORT_BOUND_TOLERANCE_MM
            || support_bounds.0.y + centre.y < -SUPPORT_BOUND_TOLERANCE_MM
            || support_bounds.0.z < -SUPPORT_BOUND_TOLERANCE_MM
            || support_bounds.1.x + centre.x
                > state.config_ffi.platform_x_mm + SUPPORT_BOUND_TOLERANCE_MM
            || support_bounds.1.y + centre.y
                > state.config_ffi.platform_y_mm + SUPPORT_BOUND_TOLERANCE_MM
            || support_bounds.1.z
                > state.config_ffi.platform_z_mm + SUPPORT_BOUND_TOLERANCE_MM
        {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                format!(
                    "generated support geometry is invalid or extends outside the build volume \
                     (bounds {:.6},{:.6},{:.6} to {:.6},{:.6},{:.6} mm; \
                     {} non-finite vertices; {} vertices, {} triangles)",
                    support_bounds.0.x + centre.x,
                    support_bounds.0.y + centre.y,
                    support_bounds.0.z,
                    support_bounds.1.x + centre.x,
                    support_bounds.1.y + centre.y,
                    support_bounds.1.z,
                    nonfinite_vertices,
                    supports.vertex_count(),
                    supports.face_count(),
                ),
            );
        }

        let mm_to_pixels = Vector2::new(
            state.config_ffi.resolution_x as f32 / state.config_ffi.platform_x_mm,
            state.config_ffi.resolution_y as f32 / state.config_ffi.platform_y_mm,
        );
        let prepared_vertices: Vec<Vector3<f32>> = supports
            .vertices()
            .iter()
            .map(|vertex| {
                Vector3::new(
                    (vertex.x + centre.x).clamp(0.0, state.config_ffi.platform_x_mm),
                    (vertex.y + centre.y).clamp(0.0, state.config_ffi.platform_y_mm),
                    vertex.z.clamp(0.0, state.config_ffi.platform_z_mm),
                )
            })
            .collect();
        let support_vertices = prepared_vertices
            .iter()
            .map(|vertex| Vector3::new(vertex.x * mm_to_pixels.x, vertex.y * mm_to_pixels.y, vertex.z))
            .collect();
        state.prepared_support = Some(Mesh::new_uncentred(
            prepared_vertices,
            supports.faces().to_vec(),
        ));
        state.models.push(SlicerModel {
            mesh: Mesh::new_uncentred(support_vertices, supports.faces().to_vec()),
            exposure: u8::MAX,
        });
        state.diagnostics.push(Diagnostic {
            severity: "info",
            code: 1101,
            message: format!(
                "generated automatic MSLA supports as preparation geometry ({} triangles)",
                supports.face_count()
            ),
        });
        state.layers = None;
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. Non-null count pointers must be writable.
pub unsafe extern "C" fn ts_mslicer_engine_generated_support_mesh_info(
    engine: *const TsEngine,
    vertex_count: *mut usize,
    triangle_count: *mut usize,
) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let Some(mesh) = state.prepared_support.as_ref() else {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "automatic support geometry has not been generated",
            );
        };
        if !vertex_count.is_null() {
            unsafe { *vertex_count = mesh.vertex_count() };
        }
        if !triangle_count.is_null() {
            unsafe { *triangle_count = mesh.face_count() };
        }
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live and `rotation_xyz_radians` writable for three floats.
/// All scene meshes must have been added before this function is called.
pub unsafe extern "C" fn ts_mslicer_engine_suggest_orientation(
    engine: *mut TsEngine,
    max_overhang_angle_degrees: f32,
    rotation_xyz_radians: *mut f32,
) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        if rotation_xyz_radians.is_null() {
            return TsStatus::InvalidArgument;
        }
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        if !max_overhang_angle_degrees.is_finite()
            || max_overhang_angle_degrees <= 0.0
            || max_overhang_angle_degrees >= 90.0
        {
            return set_error(
                &mut state,
                TsStatus::InvalidConfig,
                "orientation suggestion needs an overhang angle between 0 and 90 degrees",
            );
        }
        if state.physical_models.is_empty() {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "add all printable scene meshes before suggesting an orientation",
            );
        }
        // Score the union of the loaded meshes; a caller that wants one
        // object's own suggestion loads a job holding only that object.
        // Manifoldness is not required: scoring reads faces, not topology.
        let mut vertices = Vec::new();
        let mut faces = Vec::new();
        for mesh in &state.physical_models {
            let base = vertices.len() as u32;
            vertices.extend(mesh.vertices().iter().copied());
            faces.extend(
                mesh.faces()
                    .iter()
                    .map(|face| [face[0] + base, face[1] + base, face[2] + base]),
            );
        }
        let scene = Mesh::new_uncentred(vertices, faces);
        let rotation = suggest_orientation(&scene, max_overhang_angle_degrees);
        let output = unsafe { slice::from_raw_parts_mut(rotation_xyz_radians, 3) };
        output[0] = rotation.x;
        output[1] = rotation.y;
        output[2] = rotation.z;
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. Output pointers must be writable for the supplied
/// element capacities.
pub unsafe extern "C" fn ts_mslicer_engine_copy_generated_support_mesh(
    engine: *const TsEngine,
    vertices_xyz: *mut f32,
    vertex_float_capacity: usize,
    triangles: *mut u32,
    triangle_index_capacity: usize,
) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let Some(mesh) = state.prepared_support.as_ref() else {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "automatic support geometry has not been generated",
            );
        };
        let required_vertex_floats = mesh.vertex_count().saturating_mul(3);
        let required_triangle_indices = mesh.face_count().saturating_mul(3);
        if (required_vertex_floats != 0 && vertices_xyz.is_null())
            || (required_triangle_indices != 0 && triangles.is_null())
            || vertex_float_capacity < required_vertex_floats
            || triangle_index_capacity < required_triangle_indices
        {
            return TsStatus::BufferTooSmall;
        }
        if required_vertex_floats != 0 {
            let vertex_output =
                unsafe { slice::from_raw_parts_mut(vertices_xyz, required_vertex_floats) };
            for (output, vertex) in vertex_output.chunks_exact_mut(3).zip(mesh.vertices()) {
                output.copy_from_slice(vertex.as_slice());
            }
        }
        if required_triangle_indices != 0 {
            let triangle_output =
                unsafe { slice::from_raw_parts_mut(triangles, required_triangle_indices) };
            for (output, face) in triangle_output.chunks_exact_mut(3).zip(mesh.faces()) {
                output.copy_from_slice(face);
            }
        }
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be a live handle not concurrently used by another state-
/// changing call.
pub unsafe extern "C" fn ts_mslicer_engine_clear_meshes(engine: *mut TsEngine) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        state.diagnostics.clear();
        state.models.clear();
        state.physical_models.clear();
        state.support_blockers.clear();
        state.support_enforcers.clear();
        state.manual_support_points.clear();
        state.manual_support_only = false;
        state.prepared_support = None;
        state.layers = None;
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live and `points_xyz` readable for `point_count` XYZ
/// triples when `point_count` is nonzero.
pub unsafe extern "C" fn ts_mslicer_engine_set_manual_support_points(
    engine: *mut TsEngine,
    points_xyz: *const f32,
    point_count: usize,
    manual_only: u32,
) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        if point_count != 0 && points_xyz.is_null() {
            return TsStatus::InvalidArgument;
        }
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let Some(float_count) = point_count.checked_mul(3) else {
            return TsStatus::InvalidArgument;
        };
        let values = if float_count == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(points_xyz, float_count) }
        };
        if values.iter().any(|value| !value.is_finite()) {
            return set_error(
                &mut state,
                TsStatus::InvalidArgument,
                "manual support points must have finite coordinates",
            );
        }
        state.manual_support_points = values
            .chunks_exact(3)
            .map(|p| Vector3::new(p[0], p[1], p[2]))
            .collect();
        state.manual_support_only = manual_only != 0;
        // The prepared mesh no longer reflects the requested points.
        state.prepared_support = None;
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be a live handle. The call may run on a worker while only
/// progress and cancellation functions access the same handle concurrently.
pub unsafe extern "C" fn ts_mslicer_engine_slice(engine: *mut TsEngine) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        if state.models.is_empty() {
            return set_error(
                &mut state,
                TsStatus::InvalidMesh,
                "at least one mesh is required",
            );
        }
        if engine.cancelled.swap(false, Ordering::AcqRel) {
            state.layers = None;
            return set_error(
                &mut state,
                TsStatus::Cancelled,
                "slicing was cancelled before it started",
            );
        }
        engine.progress.set_complete(0);
        let slicer = Slicer::new(state.config.clone(), mem::take(&mut state.models))
            .with_control(engine.progress.clone(), engine.cancelled.clone());
        let result = slicer.try_slice_raster();
        state.models = slicer.into_models();
        engine.cancelled.store(false, Ordering::Release);
        match result {
            Ok(layers) => {
                let compensated = apply_raster_compensation(&state.config_ffi, layers);
                // Pre-flight: what prints in mid-air fails on the film. The
                // analysis runs on the compensated masks - what will actually
                // print - and before the orientation transform, so positions
                // stay in build-volume millimetres.
                let islands = detect_islands(&state.config_ffi, &compensated);
                if !islands.is_empty() {
                    const REPORTED: usize = 8;
                    let px_area = f64::from(
                        state.config_ffi.platform_x_mm / state.config_ffi.resolution_x as f32,
                    ) * f64::from(
                        state.config_ffi.platform_y_mm / state.config_ffi.resolution_y as f32,
                    );
                    for island in islands.iter().take(REPORTED) {
                        let z_mm =
                            island.layer as f32 * state.config_ffi.layer_height_mm;
                        state.diagnostics.push(Diagnostic {
                            severity: "warning",
                            code: 1200,
                            message: format!(
                                "unsupported island: layer {} (Z {:.2} mm) near X {:.1}, Y {:.1} mm, \
                                 about {:.2} mm^2 - it starts printing in mid-air; add a support \
                                 or reorient",
                                island.layer,
                                z_mm,
                                island.x_mm,
                                island.y_mm,
                                island.area_px as f64 * px_area
                            ),
                        });
                    }
                    if islands.len() > REPORTED {
                        state.diagnostics.push(Diagnostic {
                            severity: "warning",
                            code: 1201,
                            message: format!(
                                "{} more unsupported islands not listed",
                                islands.len() - REPORTED
                            ),
                        });
                    }
                }
                let voids = detect_trapped_voids(&state.config_ffi, &compensated);
                if !voids.is_empty() {
                    const REPORTED: usize = 8;
                    let px_area = f64::from(
                        state.config_ffi.platform_x_mm / state.config_ffi.resolution_x as f32,
                    ) * f64::from(
                        state.config_ffi.platform_y_mm / state.config_ffi.resolution_y as f32,
                    );
                    let layer_h = state.config_ffi.layer_height_mm;
                    for pocket in voids.iter().take(REPORTED) {
                        let z0 = pocket.bottom_layer as f32 * layer_h;
                        let z1 = (pocket.top_layer + 1) as f32 * layer_h;
                        let volume =
                            pocket.volume_px as f64 * px_area * f64::from(layer_h);
                        state.diagnostics.push(if pocket.cup {
                            Diagnostic {
                                severity: "warning",
                                code: 1202,
                                message: format!(
                                    "suction cup: an air pocket enclosed from Z {:.2} to \
                                     {:.2} mm near X {:.1}, Y {:.1} mm, about {:.2} mm^3, \
                                     opens only toward the film - every peel pulls a vacuum \
                                     on it; drill a drain hole through its wall or reorient",
                                    z0, z1, pocket.x_mm, pocket.y_mm, volume
                                ),
                            }
                        } else {
                            Diagnostic {
                                severity: "warning",
                                code: 1203,
                                message: format!(
                                    "sealed void: an air pocket fully enclosed from Z {:.2} \
                                     to {:.2} mm near X {:.1}, Y {:.1} mm, about {:.2} mm^3 \
                                     - it traps uncured resin and pressure; drill a drain \
                                     hole through its wall",
                                    z0, z1, pocket.x_mm, pocket.y_mm, volume
                                ),
                            }
                        });
                    }
                    if voids.len() > REPORTED {
                        state.diagnostics.push(Diagnostic {
                            severity: "warning",
                            code: 1204,
                            message: format!(
                                "{} more trapped air pockets not listed",
                                voids.len() - REPORTED
                            ),
                        });
                    }
                }
                state.layers = Some(transform_layers(&state.config_ffi, compensated));
                set_ok(&mut state)
            }
            Err(SliceError::Cancelled) => {
                state.layers = None;
                set_error(&mut state, TsStatus::Cancelled, "slicing was cancelled")
            }
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live handle. It may be called concurrently with
/// `ts_mslicer_engine_slice`.
pub unsafe extern "C" fn ts_mslicer_engine_request_cancel(engine: *mut TsEngine) {
    if let Some(engine) = unsafe { engine_ref(engine) } {
        engine.cancelled.store(true, Ordering::Release);
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live handle. It may be called concurrently with
/// `ts_mslicer_engine_slice`.
pub unsafe extern "C" fn ts_mslicer_engine_progress(engine: *const TsEngine) -> f32 {
    unsafe { engine_ref(engine) }
        .map(|engine| engine.progress.progress())
        .unwrap_or(0.0)
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be null or a live handle.
pub unsafe extern "C" fn ts_mslicer_engine_layer_count(engine: *const TsEngine) -> u32 {
    let Some(engine) = (unsafe { engine_ref(engine) }) else {
        return 0;
    };
    engine
        .state
        .lock()
        .ok()
        .and_then(|state| state.layers.as_ref().map(|layers| layers.len() as u32))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. `pixels`, when non-null, must be writable for
/// `pixel_capacity` bytes; `required_size`, when non-null, must be writable.
pub unsafe extern "C" fn ts_mslicer_engine_copy_layer(
    engine: *const TsEngine,
    layer: u32,
    pixels: *mut u8,
    pixel_capacity: usize,
    required_size: *mut usize,
) -> TsStatus {
    catch_status(|| {
        let Some(engine) = (unsafe { engine_ref(engine) }) else {
            return TsStatus::InvalidArgument;
        };
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let (width, height) = output_dimensions(&state.config_ffi);
        let required = width * height;
        if !required_size.is_null() {
            unsafe {
                *required_size = required;
            }
        }
        let Some(layer) = state
            .layers
            .as_ref()
            .and_then(|layers| layers.get(layer as usize))
        else {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "requested layer is not available",
            );
        };
        if pixels.is_null() || pixel_capacity < required {
            return TsStatus::BufferTooSmall;
        }
        let output = unsafe { slice::from_raw_parts_mut(pixels, required) };
        rle::decode_into(layer.data.iter().copied(), output);
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live and `metrics` must point to a writable ABI-4 metrics
/// structure.
pub unsafe extern "C" fn ts_mslicer_engine_metrics(
    engine: *const TsEngine,
    metrics: *mut TsMetrics,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(metrics)) =
            (unsafe { engine_ref(engine) }, unsafe { metrics.as_mut() })
        else {
            return TsStatus::InvalidArgument;
        };
        if metrics.struct_size as usize != mem::size_of::<TsMetrics>() {
            return TsStatus::InvalidArgument;
        }
        let state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let Some(layers) = state.layers.as_ref() else {
            return TsStatus::NotReady;
        };
        let voxels = lit_voxels(layers);
        let pixel_area = state.config_ffi.platform_x_mm as f64
            / state.config_ffi.resolution_x as f64
            * state.config_ffi.platform_y_mm as f64
            / state.config_ffi.resolution_y as f64;
        metrics.layer_count = layers.len() as u32;
        metrics.lit_voxels = voxels;
        metrics.resin_volume_mm3 =
            voxels as f64 * pixel_area * state.config_ffi.layer_height_mm as f64;
        metrics.estimated_seconds =
            state.config.print_time(layers.len() as u32).get::<Second>() as f64;
        TsStatus::Ok
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. `output` must point to a writable, zero-initialized
/// buffer descriptor that will later be released by this ABI.
pub unsafe extern "C" fn ts_mslicer_engine_manifest_json(
    engine: *const TsEngine,
    output: *mut TsBuffer,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(output)) =
            (unsafe { engine_ref(engine) }, unsafe { output.as_mut() })
        else {
            return TsStatus::InvalidArgument;
        };
        if !output.data.is_null() || output.size != 0 || output.capacity != 0 {
            return TsStatus::InvalidArgument;
        }
        let state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let Some(layers) = state.layers.as_ref() else {
            return TsStatus::NotReady;
        };
        let value = json!({
            "abi_version": ABI_VERSION,
            "engine": "MSlicer",
            "engine_commit": "be44e0b1734b62b012056f3590996bb2ccca3147",
            "deterministic_seed": state.config_ffi.deterministic_seed,
            "resolution": output_dimensions(&state.config_ffi),
            "orientation": if state.config_ffi.orientation == 1 { "portrait" } else { "landscape" },
            "mirror_x": state.config_ffi.mirror_x == 1,
            "mirror_y": state.config_ffi.mirror_y == 1,
            "layer_height_mm": state.config_ffi.layer_height_mm,
            "layer_count": layers.len(),
            "lit_voxels": lit_voxels(layers),
            "mesh_count": state.models.len(),
        });
        match serde_json::to_vec(&value) {
            Ok(data) => {
                *output = owned_buffer(data);
                TsStatus::Ok
            }
            Err(_) => TsStatus::EngineError,
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. `output` must point to a writable, zero-initialized
/// buffer descriptor that will later be released by this ABI.
pub unsafe extern "C" fn ts_mslicer_engine_diagnostics_json(
    engine: *const TsEngine,
    output: *mut TsBuffer,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(output)) =
            (unsafe { engine_ref(engine) }, unsafe { output.as_mut() })
        else {
            return TsStatus::InvalidArgument;
        };
        if !output.data.is_null() || output.size != 0 || output.capacity != 0 {
            return TsStatus::InvalidArgument;
        }
        let state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let value = state
            .diagnostics
            .iter()
            .map(|item| {
                json!({
                    "severity": item.severity,
                    "code": item.code,
                    "message": item.message,
                })
            })
            .collect::<Vec<_>>();
        match serde_json::to_vec(&value) {
            Ok(data) => {
                *output = owned_buffer(data);
                TsStatus::Ok
            }
            Err(_) => TsStatus::EngineError,
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live. A non-null preview must contain exactly width Ã—
/// height Ã— 4 readable bytes. `output` must be writable and zero-initialized.
pub unsafe extern "C" fn ts_mslicer_engine_encode(
    engine: *const TsEngine,
    format: u32,
    preview_rgba: *const u8,
    preview_width: u32,
    preview_height: u32,
    output: *mut TsBuffer,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(output)) =
            (unsafe { engine_ref(engine) }, unsafe { output.as_mut() })
        else {
            return TsStatus::InvalidArgument;
        };
        if !output.data.is_null() || output.size != 0 || output.capacity != 0 {
            return TsStatus::InvalidArgument;
        }
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        let Some(layers) = state.layers.as_ref() else {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "slice before encoding output",
            );
        };
        if format == TsFormat::Svg as u32 {
            let slicer = Slicer::new(state.config.clone(), state.models.clone());
            let layers = transform_vector_layers(&state.config_ffi, slicer.slice_vector());
            let output_size = if state.config_ffi.orientation == 1 {
                Vector2::new(
                    state.config_ffi.platform_y_mm,
                    state.config_ffi.platform_x_mm,
                )
            } else {
                Vector2::new(
                    state.config_ffi.platform_x_mm,
                    state.config_ffi.platform_y_mm,
                )
            };
            let file = SvgFile::new_physical(output_size, Arc::new(layers));
            let mut serializer = DynamicSerializer::new();
            file.serialize(&mut serializer);
            let bytes = serializer.into_inner();
            match catch_unwind(AssertUnwindSafe(|| {
                inspect_output_bytes(&state, format, &bytes)
            })) {
                Ok(Ok(_)) => *output = owned_buffer(bytes),
                Ok(Err(error)) => return set_error(&mut state, TsStatus::EngineError, error),
                Err(_) => {
                    return set_error(
                        &mut state,
                        TsStatus::EngineError,
                        "SVG output inspection failed",
                    );
                }
            }
            return set_ok(&mut state);
        }
        let raster_format = match format {
            value if value == TsFormat::Ctb as u32 => RasterFormat::Ctb,
            value if value == TsFormat::Goo as u32 => RasterFormat::Goo,
            value if value == TsFormat::NanoDlp as u32 => RasterFormat::NanoDLP,
            _ => return set_error(&mut state, TsStatus::Unsupported, "unknown output format"),
        };
        let voxels = lit_voxels(layers);
        let format_config = output_config(&state.config_ffi, &state.config);
        let mut file = util::export_raster(&format_config, layers.iter(), voxels, raster_format);
        let preview = if preview_rgba.is_null() || preview_width == 0 || preview_height == 0 {
            // CTB creates a 3/4-scale secondary preview. A 1x1 placeholder
            // rounds to zero and trips its encoder, so use the smallest safe
            // deterministic placeholder when the caller supplies no preview.
            RgbaImage::new(4, 4)
        } else {
            let Some(bytes) = (preview_width as usize)
                .checked_mul(preview_height as usize)
                .and_then(|v| v.checked_mul(4))
            else {
                return set_error(
                    &mut state,
                    TsStatus::InvalidArgument,
                    "preview size overflow",
                );
            };
            let data = unsafe { slice::from_raw_parts(preview_rgba, bytes) }.to_vec();
            let Some(image) = RgbaImage::from_raw(preview_width, preview_height, data) else {
                return set_error(
                    &mut state,
                    TsStatus::InvalidArgument,
                    "invalid RGBA preview buffer",
                );
            };
            image
        };
        file.set_preview(&preview);
        let mut serializer = DynamicSerializer::new();
        file.serialize(&mut serializer, Progress::new());
        let bytes = serializer.into_inner();
        match catch_unwind(AssertUnwindSafe(|| {
            inspect_output_bytes(&state, format, &bytes)
        })) {
            Ok(Ok(_)) => *output = owned_buffer(bytes),
            Ok(Err(error)) => return set_error(&mut state, TsStatus::EngineError, error),
            Err(_) => {
                return set_error(
                    &mut state,
                    TsStatus::EngineError,
                    "output decoder rejected encoded bytes",
                );
            }
        }
        set_ok(&mut state)
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `engine` must be live; `data` must describe `size` readable bytes; and
/// `details_json` must be writable and zero-initialized.
pub unsafe extern "C" fn ts_mslicer_engine_inspect_output(
    engine: *const TsEngine,
    format: u32,
    data: *const u8,
    size: usize,
    details_json: *mut TsBuffer,
) -> TsStatus {
    catch_status(|| {
        let (Some(engine), Some(details_json)) = (unsafe { engine_ref(engine) }, unsafe {
            details_json.as_mut()
        }) else {
            return TsStatus::InvalidArgument;
        };
        if data.is_null()
            || size == 0
            || size > MAX_OUTPUT_BYTES
            || !details_json.data.is_null()
            || details_json.size != 0
            || details_json.capacity != 0
        {
            return TsStatus::InvalidArgument;
        }
        let mut state = match engine.state.lock() {
            Ok(state) => state,
            Err(_) => return TsStatus::Panic,
        };
        if state.layers.is_none() {
            return set_error(
                &mut state,
                TsStatus::NotReady,
                "slice before inspecting output",
            );
        }
        let bytes = unsafe { slice::from_raw_parts(data, size) };
        match catch_unwind(AssertUnwindSafe(|| {
            inspect_output_bytes(&state, format, bytes)
        })) {
            Ok(Ok(report)) => match serde_json::to_vec(&report) {
                Ok(json) => {
                    *details_json = owned_buffer(json);
                    set_ok(&mut state)
                }
                Err(_) => set_error(
                    &mut state,
                    TsStatus::EngineError,
                    "failed to serialize output inspection report",
                ),
            },
            Ok(Err(error)) => set_error(&mut state, TsStatus::EngineError, error),
            Err(_) => set_error(
                &mut state,
                TsStatus::EngineError,
                "malformed output was rejected by the decoder",
            ),
        }
    })
}

#[unsafe(no_mangle)]
/// # Safety
/// `buffer` must be null, zero-initialized, or an unchanged descriptor returned
/// by this ABI. No other thread may access its data during release.
pub unsafe extern "C" fn ts_mslicer_buffer_release(buffer: *mut TsBuffer) {
    let Some(buffer) = (unsafe { buffer.as_mut() }) else {
        return;
    };
    if !buffer.data.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
            drop(Vec::from_raw_parts(
                buffer.data,
                buffer.size,
                buffer.capacity,
            ));
        }));
    }
    *buffer = TsBuffer {
        data: ptr::null_mut(),
        size: 0,
        capacity: 0,
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn ts_mslicer_reserved(_unused: *mut c_void) {}

#[cfg(test)]
mod tests {
    use super::*;

    fn exposure() -> TsExposure {
        TsExposure {
            exposure_seconds: 2.5,
            delay_seconds: 0.5,
            pwm: 255,
            reserved: [0; 3],
            lift_distance_mm: 5.0,
            lift_speed_mm_min: 120.0,
            retract_distance_mm: 5.0,
            retract_speed_mm_min: 180.0,
        }
    }

    fn config() -> TsConfig {
        TsConfig {
            struct_size: mem::size_of::<TsConfig>() as u32,
            resolution_x: 64,
            resolution_y: 64,
            platform_x_mm: 20.0,
            platform_y_mm: 20.0,
            platform_z_mm: 20.0,
            layer_height_mm: 1.0,
            supersample: 1,
            orientation: 0,
            mirror_x: 0,
            mirror_y: 0,
            first_layers: 1,
            transition_layers: 0,
            normal: exposure(),
            first: exposure(),
            deterministic_seed: 42,
            absolute_correction_mm: 0.0,
            elefant_foot_compensation_mm: 0.0,
        }
    }

    #[test]
    fn rejects_invalid_config_without_panicking() {
        let mut config = config();
        config.supersample = 0;
        assert!(
            validate_config(&config)
                .unwrap_err()
                .contains("supersample")
        );
        config = super::tests::config();
        config.resolution_x = 0;
        assert!(validate_config(&config).is_err());
    }

    #[test]
    fn buffer_round_trip_and_double_release_are_safe() {
        let mut buffer = owned_buffer(vec![1, 2, 3]);
        unsafe { ts_mslicer_buffer_release(&mut buffer) };
        assert!(buffer.data.is_null());
        unsafe { ts_mslicer_buffer_release(&mut buffer) };
    }

    #[test]
    fn abi_identity_is_stable() {
        assert_eq!(ts_mslicer_abi_version(), ABI_VERSION);
        assert_ne!(ts_mslicer_capabilities(), 0);
        assert_ne!(ts_mslicer_capabilities() & (1 << 13), 0);
        assert_ne!(ts_mslicer_capabilities() & (1 << 14), 0);
        assert!(!ts_mslicer_engine_version().is_null());
    }

    #[test]
    fn island_detection_flags_midair_components_only() {
        let mut config = config();
        config.resolution_x = 16;
        config.resolution_y = 16;
        config.platform_x_mm = 16.0;
        config.platform_y_mm = 16.0;
        let layer_from = |mask: &[u8]| Layer {
            data: encode_pixels(mask),
            height: Milimeters::new(1.0),
            exposure: convert_exposure(&config.normal),
        };
        // Layer 0: a 4x4 block near the origin. Layer 1: the same block (a
        // grounded tower) plus a separate 2x2 block far away (an island).
        let mut base = vec![0_u8; 256];
        for y in 2..6 {
            for x in 2..6 {
                base[y * 16 + x] = 255;
            }
        }
        let mut second = base.clone();
        for y in 12..14 {
            for x in 12..14 {
                second[y * 16 + x] = 255;
            }
        }
        let islands = detect_islands(&config, &[layer_from(&base), layer_from(&second)]);
        assert_eq!(islands.len(), 1);
        assert_eq!(islands[0].layer, 1);
        assert_eq!(islands[0].area_px, 4);
        assert!((islands[0].x_mm - 13.0).abs() < 0.6);
        assert!((islands[0].y_mm - 12.5).abs() < 0.6);

        // A one-pixel step outward is still connected to material below.
        let mut third = vec![0_u8; 256];
        for y in 1..7 {
            for x in 1..7 {
                third[y * 16 + x] = 255;
            }
        }
        let islands = detect_islands(&config, &[layer_from(&base), layer_from(&third)]);
        assert!(islands.is_empty());

        // The very first layer can never be an island.
        let islands = detect_islands(&config, &[layer_from(&second)]);
        assert!(islands.is_empty());
    }

    #[test]
    fn trapped_void_detection_classifies_pockets() {
        let mut config = config();
        config.resolution_x = 16;
        config.resolution_y = 16;
        config.platform_x_mm = 16.0;
        config.platform_y_mm = 16.0;
        let layer_from = |mask: &[u8]| Layer {
            data: encode_pixels(mask),
            height: Milimeters::new(1.0),
            exposure: convert_exposure(&config.normal),
        };
        // Building blocks on a 16x16 raster: a solid 8x8 slab at (2..10),
        // a ring of the same footprint with a 4x4 hollow (4..8), and a ring
        // with a one-pixel channel cut through its right wall.
        let mut slab = vec![0_u8; 256];
        for y in 2..10 {
            for x in 2..10 {
                slab[y * 16 + x] = 255;
            }
        }
        let mut ring = slab.clone();
        for y in 4..8 {
            for x in 4..8 {
                ring[y * 16 + x] = 0;
            }
        }
        let mut vented_ring = ring.clone();
        for x in 8..10 {
            vented_ring[5 * 16 + x] = 0;
        }

        // Sealed void: floor, hollow rings, cap. Fully enclosed.
        let voids = detect_trapped_voids(
            &config,
            &[
                layer_from(&slab),
                layer_from(&ring),
                layer_from(&ring),
                layer_from(&slab),
            ],
        );
        assert_eq!(voids.len(), 1);
        assert!(!voids[0].cup);
        assert_eq!(voids[0].bottom_layer, 1);
        assert_eq!(voids[0].top_layer, 2);
        assert_eq!(voids[0].volume_px, 32);
        assert!((voids[0].x_mm - 6.0).abs() < 0.6);
        assert!((voids[0].y_mm - 5.5).abs() < 0.6);

        // Suction cup: floor then rings, never capped - the opening faces
        // the film and every peel pulls a vacuum on it.
        let voids = detect_trapped_voids(
            &config,
            &[layer_from(&slab), layer_from(&ring), layer_from(&ring)],
        );
        assert_eq!(voids.len(), 1);
        assert!(voids[0].cup);

        // Plate-sealed tube: rings from layer 0 up, capped by a slab. The
        // plate is the lid, the cap seals the far end - a suction cup.
        let voids = detect_trapped_voids(
            &config,
            &[layer_from(&ring), layer_from(&ring), layer_from(&slab)],
        );
        assert_eq!(voids.len(), 1);
        assert!(voids[0].cup);

        // A channel through the wall vents the pocket - a drilled drain
        // hole looks exactly like this in the masks. No report.
        let voids = detect_trapped_voids(
            &config,
            &[
                layer_from(&slab),
                layer_from(&ring),
                layer_from(&vented_ring),
                layer_from(&slab),
            ],
        );
        assert!(voids.is_empty());

        // A cavity born over open air above the plate vents downward and
        // sideways: rings floating with nothing beneath them (their walls
        // would be islands, but that is the other analysis's concern).
        let empty = vec![0_u8; 256];
        let voids = detect_trapped_voids(
            &config,
            &[
                layer_from(&empty),
                layer_from(&ring),
                layer_from(&ring),
                layer_from(&slab),
            ],
        );
        assert!(voids.is_empty());
    }

    #[test]
    fn support_region_containment_is_watertight_for_a_box() {
        // A unit box from (0,0,0) to (2,2,2).
        let vertices = vec![
            Vector3::new(0.0, 0.0, 0.0),
            Vector3::new(2.0, 0.0, 0.0),
            Vector3::new(2.0, 2.0, 0.0),
            Vector3::new(0.0, 2.0, 0.0),
            Vector3::new(0.0, 0.0, 2.0),
            Vector3::new(2.0, 0.0, 2.0),
            Vector3::new(2.0, 2.0, 2.0),
            Vector3::new(0.0, 2.0, 2.0),
        ];
        let faces = vec![
            [0, 2, 1],
            [0, 3, 2],
            [4, 5, 6],
            [4, 6, 7],
            [0, 1, 5],
            [0, 5, 4],
            [1, 2, 6],
            [1, 6, 5],
            [2, 3, 7],
            [2, 7, 6],
            [3, 0, 4],
            [3, 4, 7],
        ];
        let region = SupportRegion { vertices, faces };
        assert!(region.contains(&Vector3::new(1.0, 1.0, 1.0)));
        assert!(region.contains(&Vector3::new(0.1, 1.9, 0.1)));
        assert!(!region.contains(&Vector3::new(3.0, 1.0, 1.0)));
        assert!(!region.contains(&Vector3::new(1.0, 1.0, 2.5)));
        assert!(!region.contains(&Vector3::new(-0.5, -0.5, -0.5)));
    }

    #[test]
    fn running_extreme_matches_the_naive_window() {
        // Deterministic pseudo-random rows; every (length, radius) pair is
        // checked against the obvious O(n * w) reference.
        let mut seed = 0x1234_5678_u64;
        let mut next = move || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            (seed >> 33) as u8
        };
        for n in [1_usize, 2, 5, 7, 8, 31, 64] {
            for radius in [1_usize, 2, 3, 5] {
                for maximum in [false, true] {
                    let reference_input: Vec<u8> = (0..n).map(|_| next()).collect();
                    let mut fast = reference_input.clone();
                    running_extreme_row(&mut fast, radius, maximum);
                    for i in 0..n {
                        let mut value = if maximum { 0 } else { 255 };
                        for j in i.saturating_sub(radius)..=(i + radius) {
                            let sample = if j < n { reference_input[j] } else { 0 };
                            value = if maximum { value.max(sample) } else { value.min(sample) };
                        }
                        if i < radius && !maximum {
                            value = 0; // the window pokes past the left border
                        }
                        assert_eq!(fast[i], value, "n={n} r={radius} max={maximum} i={i}");
                    }
                }
            }
        }
    }

    #[test]
    fn raster_compensation_erodes_and_dilates_the_masks() {
        let mut config = config();
        config.resolution_x = 8;
        config.resolution_y = 8;
        config.platform_x_mm = 8.0; // 1 mm per pixel
        config.platform_y_mm = 8.0;
        // A solid 4x4 block in the middle of an 8x8 mask.
        let mut mask = vec![0_u8; 64];
        for y in 2..6 {
            for x in 2..6 {
                mask[y * 8 + x] = 255;
            }
        }
        let layer = || Layer {
            data: encode_pixels(&mask),
            height: Milimeters::new(1.0),
            exposure: convert_exposure(&config.normal),
        };
        let lit = |layers: &[Layer], index: usize| -> usize {
            rle::decode_vec(layers[index].data.iter().copied())
                .iter()
                .filter(|value| **value > 0)
                .count()
        };

        // Elephant foot: first layer erodes by one pixel per side, the rest stay.
        config.elefant_foot_compensation_mm = 1.0;
        let compensated = apply_raster_compensation(&config, vec![layer(), layer()]);
        assert_eq!(lit(&compensated, 0), 4);
        assert_eq!(lit(&compensated, 1), 16);

        // Positive absolute correction dilates every layer by one pixel per side.
        config.elefant_foot_compensation_mm = 0.0;
        config.absolute_correction_mm = 1.0;
        let compensated = apply_raster_compensation(&config, vec![layer(), layer()]);
        assert_eq!(lit(&compensated, 0), 36);
        assert_eq!(lit(&compensated, 1), 36);

        // Negative absolute correction erodes every layer instead.
        config.absolute_correction_mm = -1.0;
        let compensated = apply_raster_compensation(&config, vec![layer(), layer()]);
        assert_eq!(lit(&compensated, 0), 4);
        assert_eq!(lit(&compensated, 1), 4);

        // Below half a pixel nothing changes.
        config.absolute_correction_mm = 0.4;
        let compensated = apply_raster_compensation(&config, vec![layer()]);
        assert_eq!(lit(&compensated, 0), 16);
    }

    #[test]
    fn applies_orientation_and_mirroring_to_the_shared_output_layers() {
        let mut config = config();
        config.resolution_x = 2;
        config.resolution_y = 3;
        config.orientation = 1;
        config.mirror_x = 1;
        let layer = Layer {
            data: (1_u8..=6).map(|value| Run::new(1, value)).collect(),
            height: Milimeters::new(1.0),
            exposure: convert_exposure(&config.normal),
        };
        let transformed = transform_layers(&config, vec![layer]);
        assert_eq!(output_dimensions(&config), (3, 2));
        assert_eq!(
            rle::decode_vec(transformed[0].data.iter().copied()),
            vec![1, 3, 5, 2, 4, 6]
        );
    }

    fn cube_mesh() -> (Vec<f32>, Vec<u32>, TsMesh) {
        let vertices = vec![
            5.0, 5.0, 0.0, 15.0, 5.0, 0.0, 15.0, 15.0, 0.0, 5.0, 15.0, 0.0, 5.0, 5.0, 2.0, 15.0,
            5.0, 2.0, 15.0, 15.0, 2.0, 5.0, 15.0, 2.0,
        ];
        let triangles = vec![
            0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7,
            6, 3, 0, 4, 3, 4, 7,
        ];
        let mesh = TsMesh {
            struct_size: mem::size_of::<TsMesh>() as u32,
            vertices_xyz: vertices.as_ptr(),
            vertex_count: vertices.len() / 3,
            triangles: triangles.as_ptr(),
            triangle_count: triangles.len() / 3,
            transform: [
                1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
            ],
            relative_exposure: 255,
            role: 0,
            reserved: [0; 6],
        };
        (vertices, triangles, mesh)
    }

    #[test]
    fn slices_previews_metrics_manifests_and_encodes_through_abi() {
        let config = config();
        let engine = unsafe { ts_mslicer_engine_create(&config) };
        assert!(!engine.is_null());
        let (_vertices, _triangles, mesh) = cube_mesh();
        assert_eq!(
            unsafe { ts_mslicer_engine_add_mesh(engine, &mesh) },
            TsStatus::Ok
        );
        assert_eq!(unsafe { ts_mslicer_engine_slice(engine) }, TsStatus::Ok);
        assert_eq!(unsafe { ts_mslicer_engine_layer_count(engine) }, 2);

        let mut required = 0;
        assert_eq!(
            unsafe { ts_mslicer_engine_copy_layer(engine, 0, ptr::null_mut(), 0, &mut required) },
            TsStatus::BufferTooSmall
        );
        assert_eq!(required, 64 * 64);
        let mut pixels = vec![0; required];
        assert_eq!(
            unsafe {
                ts_mslicer_engine_copy_layer(
                    engine,
                    0,
                    pixels.as_mut_ptr(),
                    pixels.len(),
                    &mut required,
                )
            },
            TsStatus::Ok
        );
        assert!(pixels.iter().any(|&pixel| pixel != 0));

        let mut metrics = TsMetrics {
            struct_size: mem::size_of::<TsMetrics>() as u32,
            layer_count: 0,
            lit_voxels: 0,
            resin_volume_mm3: 0.0,
            estimated_seconds: 0.0,
        };
        assert_eq!(
            unsafe { ts_mslicer_engine_metrics(engine, &mut metrics) },
            TsStatus::Ok
        );
        assert_eq!(metrics.layer_count, 2);
        assert!(metrics.lit_voxels > 0);
        assert!((metrics.resin_volume_mm3 - 200.0).abs() < 1.0);

        let mut manifest = TsBuffer {
            data: ptr::null_mut(),
            size: 0,
            capacity: 0,
        };
        assert_eq!(
            unsafe { ts_mslicer_engine_manifest_json(engine, &mut manifest) },
            TsStatus::Ok
        );
        let manifest_bytes = unsafe { slice::from_raw_parts(manifest.data, manifest.size) };
        let value: serde_json::Value = serde_json::from_slice(manifest_bytes).unwrap();
        assert_eq!(
            value["engine_commit"],
            "be44e0b1734b62b012056f3590996bb2ccca3147"
        );
        unsafe { ts_mslicer_buffer_release(&mut manifest) };

        for format in [
            TsFormat::Ctb,
            TsFormat::Goo,
            TsFormat::NanoDlp,
            TsFormat::Svg,
        ] {
            let mut encoded = TsBuffer {
                data: ptr::null_mut(),
                size: 0,
                capacity: 0,
            };
            assert_eq!(
                unsafe {
                    ts_mslicer_engine_encode(engine, format as u32, ptr::null(), 0, 0, &mut encoded)
                },
                TsStatus::Ok
            );
            assert!(encoded.size > 0);
            let mut report = TsBuffer {
                data: ptr::null_mut(),
                size: 0,
                capacity: 0,
            };
            assert_eq!(
                unsafe {
                    ts_mslicer_engine_inspect_output(
                        engine,
                        format as u32,
                        encoded.data,
                        encoded.size,
                        &mut report,
                    )
                },
                TsStatus::Ok
            );
            assert!(report.size > 0);
            unsafe {
                ts_mslicer_buffer_release(&mut report);
                ts_mslicer_buffer_release(&mut encoded);
            }
        }
        unsafe { ts_mslicer_engine_destroy(engine) };
    }

    #[test]
    fn slices_translated_cube_at_generic_printer_resolution_without_losing_area() {
        let mut config = config();
        config.resolution_x = 3840;
        config.resolution_y = 2400;
        config.platform_x_mm = 192.0;
        config.platform_y_mm = 120.0;
        config.platform_z_mm = 200.0;
        config.layer_height_mm = 0.05;
        config.supersample = 1;

        let engine = unsafe { ts_mslicer_engine_create(&config) };
        assert!(!engine.is_null());
        let (vertices, triangles, _) = cube_mesh();
        let mut split_vertices = Vec::with_capacity(triangles.len() * 3);
        let mut split_triangles = Vec::with_capacity(triangles.len());
        for (new_index, old_index) in triangles.iter().copied().enumerate() {
            let old = old_index as usize * 3;
            split_vertices.extend_from_slice(&vertices[old..old + 3]);
            split_triangles.push(new_index as u32);
        }
        let mut mesh = TsMesh {
            struct_size: mem::size_of::<TsMesh>() as u32,
            vertices_xyz: split_vertices.as_ptr(),
            vertex_count: split_vertices.len() / 3,
            triangles: split_triangles.as_ptr(),
            triangle_count: split_triangles.len() / 3,
            transform: [
                1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
            ],
            relative_exposure: 255,
            role: 0,
            reserved: [0; 6],
        };
        mesh.transform[0] = 2.0;
        mesh.transform[5] = 2.0;
        mesh.transform[10] = 10.0;
        mesh.transform[12] = 76.0;
        mesh.transform[13] = 40.0;
        assert_eq!(
            unsafe { ts_mslicer_engine_add_mesh(engine, &mesh) },
            TsStatus::Ok
        );
        assert_eq!(unsafe { ts_mslicer_engine_slice(engine) }, TsStatus::Ok);

        let mut metrics = TsMetrics {
            struct_size: mem::size_of::<TsMetrics>() as u32,
            layer_count: 0,
            lit_voxels: 0,
            resin_volume_mm3: 0.0,
            estimated_seconds: 0.0,
        };
        assert_eq!(
            unsafe { ts_mslicer_engine_metrics(engine, &mut metrics) },
            TsStatus::Ok
        );
        assert_eq!(metrics.layer_count, 400);
        assert!((metrics.resin_volume_mm3 - 8000.0).abs() < 1.0);
        unsafe { ts_mslicer_engine_destroy(engine) };
    }

    #[test]
    fn cancellation_never_returns_partial_layers() {
        let config = config();
        let engine = unsafe { ts_mslicer_engine_create(&config) };
        let (_vertices, _triangles, mesh) = cube_mesh();
        assert_eq!(
            unsafe { ts_mslicer_engine_add_mesh(engine, &mesh) },
            TsStatus::Ok
        );
        unsafe { ts_mslicer_engine_request_cancel(engine) };
        assert_eq!(
            unsafe { ts_mslicer_engine_slice(engine) },
            TsStatus::Cancelled
        );
        assert_eq!(unsafe { ts_mslicer_engine_layer_count(engine) }, 0);
        assert_eq!(unsafe { ts_mslicer_engine_slice(engine) }, TsStatus::Ok);
        unsafe { ts_mslicer_engine_destroy(engine) };
    }

    #[test]
    fn rejects_invalid_affine_mesh_and_unknown_format() {
        let config = config();
        let engine = unsafe { ts_mslicer_engine_create(&config) };
        let (_vertices, _triangles, mut mesh) = cube_mesh();
        mesh.transform[15] = 2.0;
        assert_eq!(
            unsafe { ts_mslicer_engine_add_mesh(engine, &mesh) },
            TsStatus::InvalidMesh
        );
        let mut diagnostics = TsBuffer {
            data: ptr::null_mut(),
            size: 0,
            capacity: 0,
        };
        assert_eq!(
            unsafe { ts_mslicer_engine_diagnostics_json(engine, &mut diagnostics) },
            TsStatus::Ok
        );
        let value: serde_json::Value = serde_json::from_slice(unsafe {
            slice::from_raw_parts(diagnostics.data, diagnostics.size)
        })
        .unwrap();
        assert_eq!(value[0]["severity"], "error");
        assert_eq!(value[0]["code"], TsStatus::InvalidMesh as u32);
        unsafe { ts_mslicer_buffer_release(&mut diagnostics) };

        mesh.transform[15] = 1.0;
        assert_eq!(
            unsafe { ts_mslicer_engine_add_mesh(engine, &mesh) },
            TsStatus::Ok
        );
        assert_eq!(unsafe { ts_mslicer_engine_slice(engine) }, TsStatus::Ok);
        let mut output = TsBuffer {
            data: ptr::null_mut(),
            size: 0,
            capacity: 0,
        };
        assert_eq!(
            unsafe { ts_mslicer_engine_encode(engine, u32::MAX, ptr::null(), 0, 0, &mut output) },
            TsStatus::Unsupported
        );
        unsafe { ts_mslicer_engine_destroy(engine) };
    }
}
