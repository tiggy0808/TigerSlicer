use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

use common::{progress::Progress, slice::SliceConfig};

use crate::mesh::Mesh;

pub mod raster;
pub mod vector;

const SEGMENT_LAYERS: usize = 100;

/// Used to slice a mesh.
pub struct Slicer {
    pub slice_config: SliceConfig,
    models: Vec<SlicerModel>,

    layers: u32,
    progress: Progress,
    cancelled: Arc<AtomicBool>,
}

#[derive(Clone)]
pub struct SlicerModel {
    pub mesh: Mesh,
    pub exposure: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SliceError {
    Cancelled,
}

impl Slicer {
    /// Creates a new slicer given a slice config, list of models, and their relative exposures.
    pub fn new(slice_config: SliceConfig, models: Vec<SlicerModel>) -> Self {
        let max_z = models.iter().fold(0_f32, |max, model| {
            let verts = model.mesh.vertices().iter();
            let z = verts.fold(0_f32, |max, &f| max.max(model.mesh.transform(&f).z));
            max.max(z)
        });

        let slice = slice_config.slice_height;
        let max_layers = (slice_config.platform_size.z / slice).ceil() as u32;
        let layers = ((max_z / slice).raw().ceil() as u32).min(max_layers);

        let progress = Progress::new();
        progress.set_total(layers as u64);

        Self {
            slice_config,
            models,

            layers,
            progress,
            cancelled: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn layer_count(&self) -> u32 {
        self.layers
    }

    pub fn into_models(self) -> Vec<SlicerModel> {
        self.models
    }

    /// Gets an instance of the slicing [`Progress`] struct.
    pub fn progress(&self) -> Progress {
        self.progress.clone()
    }

    /// Replaces the operation controls used by an embedding application.
    /// This keeps progress and cancellation alive independently of the
    /// caller's synchronous slice invocation.
    pub fn with_control(mut self, progress: Progress, cancelled: Arc<AtomicBool>) -> Self {
        progress.set_total(self.layers as u64);
        self.progress = progress;
        self.cancelled = cancelled;
        self
    }

    /// Requests cooperative cancellation. Raster workers check this flag
    /// before starting each layer, so cancellation never returns partial data.
    pub fn request_cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    pub fn reset_cancel(&self) {
        self.cancelled.store(false, Ordering::Release);
    }

    pub(crate) fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}
