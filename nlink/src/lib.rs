//! TI-84 Evo linking stays in Rust. Everything else lives in nlink/cpp.
pub mod error;
mod evo_ffi;
pub mod link;
pub mod progress;
