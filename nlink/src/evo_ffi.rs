use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::ptr;

use crate::link::evo_host;

fn set_str(out: *mut *mut c_char, value: &str) {
  if out.is_null() {
    return;
  }
  unsafe {
    *out = CString::new(value).map(|c| c.into_raw()).unwrap_or(ptr::null_mut());
  }
}

fn fail(out_err: *mut *mut c_char, err: impl ToString) -> c_int {
  set_str(out_err, &err.to_string());
  -1
}

fn cstr<'a>(p: *const c_char) -> Result<&'a str, &'static str> {
  if p.is_null() {
    return Err("null string");
  }
  unsafe { CStr::from_ptr(p) }.to_str().map_err(|_| "invalid utf-8")
}

#[no_mangle]
pub unsafe extern "C" fn nlink_evo_string_free(p: *mut c_char) {
  if !p.is_null() {
    drop(CString::from_raw(p));
  }
}

#[no_mangle]
pub unsafe extern "C" fn nlink_evo_buf_free(p: *mut u8, len: usize) {
  if !p.is_null() {
    drop(Vec::from_raw_parts(p, len, len));
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_connected(bus: u8, addr: u8) -> c_int {
  if evo_host::connected(bus, addr) {
    1
  } else {
    0
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_close(bus: u8, addr: u8) {
  evo_host::close(bus, addr);
}

#[no_mangle]
pub extern "C" fn nlink_evo_open(
  bus: u8,
  addr: u8,
  out_json: *mut *mut c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  #[cfg(target_os = "android")]
  {
    let _ = (bus, addr, out_json);
    return fail(out_err, "Evo open on Android uses nlink_evo_open_android");
  }
  #[cfg(not(target_os = "android"))]
  match evo_host::open_usb(bus, addr) {
    Ok(json) => {
      set_str(out_json, &json.to_string());
      0
    }
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_open_android(
  ep_in: u8,
  ep_out: u8,
  out_json: *mut *mut c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  #[cfg(not(target_os = "android"))]
  {
    let _ = (ep_in, ep_out, out_json);
    return fail(out_err, "nlink_evo_open_android is only available on Android");
  }
  #[cfg(target_os = "android")]
  match evo_host::open_android(ep_in, ep_out) {
    Ok(json) => {
      set_str(out_json, &json.to_string());
      0
    }
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_info(
  bus: u8,
  addr: u8,
  out_json: *mut *mut c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  match evo_host::info(bus, addr) {
    Ok(json) => {
      set_str(out_json, &json.to_string());
      0
    }
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_list(
  bus: u8,
  addr: u8,
  path: *const c_char,
  out_json: *mut *mut c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  let path = match cstr(path) {
    Ok(p) => p,
    Err(e) => return fail(out_err, e),
  };
  match evo_host::list_dir(bus, addr, path) {
    Ok(list) => match serde_json::to_string(&list) {
      Ok(json) => {
        set_str(out_json, &json);
        0
      }
      Err(e) => fail(out_err, e),
    },
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_download(
  bus: u8,
  addr: u8,
  remote: *const c_char,
  dest: *const c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  let remote = match cstr(remote) {
    Ok(p) => p,
    Err(e) => return fail(out_err, e),
  };
  let dest = match cstr(dest) {
    Ok(p) => p,
    Err(e) => return fail(out_err, e),
  };
  match evo_host::download(bus, addr, remote, std::path::Path::new(dest)) {
    Ok(()) => 0,
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_upload(
  bus: u8,
  addr: u8,
  _dest_dir: *const c_char,
  src: *const c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  let src = match cstr(src) {
    Ok(p) => p,
    Err(e) => return fail(out_err, e),
  };
  match evo_host::upload(bus, addr, std::path::Path::new(src)) {
    Ok(()) => 0,
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_remove(
  bus: u8,
  addr: u8,
  remote: *const c_char,
  out_err: *mut *mut c_char,
) -> c_int {
  let remote = match cstr(remote) {
    Ok(p) => p,
    Err(e) => return fail(out_err, e),
  };
  match evo_host::remove(bus, addr, remote) {
    Ok(()) => 0,
    Err(e) => fail(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_evo_screenshot(
  bus: u8,
  addr: u8,
  out_rgba: *mut *mut u8,
  out_w: *mut i32,
  out_h: *mut i32,
  out_err: *mut *mut c_char,
) -> c_int {
  match evo_host::screenshot(bus, addr) {
    Ok((w, h, rgba)) => {
      let len = rgba.len();
      let mut boxed = rgba.into_boxed_slice();
      let ptr = boxed.as_mut_ptr();
      std::mem::forget(boxed);
      unsafe {
        if !out_rgba.is_null() {
          *out_rgba = ptr;
        }
        if !out_w.is_null() {
          *out_w = w as i32;
        }
        if !out_h.is_null() {
          *out_h = h as i32;
        }
      }
      let _ = len;
      0
    }
    Err(e) => fail(out_err, e),
  }
}
