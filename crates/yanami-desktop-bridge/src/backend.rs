use std::path::Path;

use tokio::{
    runtime::{Builder, Runtime},
    sync::watch,
};
use yanami_application::{
    Application, ApplicationError, ApplicationErrorCode, ApplicationOpenOptions,
};

pub struct YanamiBackend {
    // Drop order is deliberate: Application aborts its tasks before Runtime.
    pub(crate) application: Application,
    _runtime: Runtime,
    cancellation: watch::Sender<bool>,
}

impl YanamiBackend {
    pub(crate) fn open(data_dir: &Path) -> Result<Self, ApplicationError> {
        Self::open_with_options(data_dir, ApplicationOpenOptions::default())
    }

    pub(crate) fn open_with_options(
        data_dir: &Path,
        options: ApplicationOpenOptions,
    ) -> Result<Self, ApplicationError> {
        let runtime = Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(|error| {
                ApplicationError::new(ApplicationErrorCode::Internal, error.to_string())
            })?;
        let (cancellation, _) = watch::channel(false);
        let application = Application::open_with_options(
            data_dir,
            runtime.handle().clone(),
            cancellation.clone(),
            options,
        )?;
        Ok(Self {
            application,
            _runtime: runtime,
            cancellation,
        })
    }

    pub(crate) fn cancel_all(&self) {
        self.cancellation.send_replace(true);
        self.application.shutdown();
    }
}
