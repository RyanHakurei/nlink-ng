#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]

#[cfg(not(target_os = "android"))]
extern crate libusb1_sys;
include!("bindings.rs");
