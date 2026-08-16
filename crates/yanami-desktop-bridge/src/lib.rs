//! Narrow C ABI used by the Qt shell.
//!
//! Feature policy and presentation mapping live in `yanami-application`; this
//! crate owns only the runtime, cancellation, serialization and audited FFI.

mod backend;
mod codec;
#[allow(unsafe_code)]
mod ffi;

#[doc(hidden)]
pub use backend::YanamiBackend;
pub use ffi::*;
