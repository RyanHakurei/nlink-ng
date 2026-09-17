#[cfg(not(target_os = "android"))]
pub mod cli;
#[cfg_attr(target_os = "android", path = "device_android.rs")]
pub mod device;
pub mod error;
pub mod ffi;
pub mod progress;
