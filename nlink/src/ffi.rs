use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::ptr;

use crate::device;
use crate::error::Result;

#[repr(C)]
pub struct NLinkString {
  pub data: *mut c_char,
  pub len: usize,
}

impl NLinkString {
  fn from_str(s: &str) -> Self {
    match CString::new(s) {
      Ok(c) => {
        let len = c.as_bytes().len();
        NLinkString {
          data: c.into_raw(),
          len,
        }
      }
      Err(_) => NLinkString {
        data: ptr::null_mut(),
        len: 0,
      },
    }
  }

  fn empty() -> Self {
    NLinkString {
      data: ptr::null_mut(),
      len: 0,
    }
  }
}

#[no_mangle]
pub unsafe extern "C" fn nlink_string_free(s: NLinkString) {
  if !s.data.is_null() {
    drop(CString::from_raw(s.data));
  }
}

fn cstr<'a>(p: *const c_char) -> Result<&'a str> {
  if p.is_null() {
    return Err("null string".into());
  }
  unsafe { CStr::from_ptr(p) }
    .to_str()
    .map_err(|_| crate::error::NlinkError::from("invalid utf-8"))
}

fn fill_ok(out: *mut NLinkString, value: &str) {
  if !out.is_null() {
    unsafe { *out = NLinkString::from_str(value) };
  }
}

fn fill_err(err: *mut NLinkString, e: impl ToString) -> c_int {
  if !err.is_null() {
    unsafe { *err = NLinkString::from_str(&e.to_string()) };
  }
  -1
}

fn fill_empty(p: *mut NLinkString) {
  if !p.is_null() {
    unsafe { *p = NLinkString::empty() };
  }
}

#[cfg(not(target_os = "android"))]
fn info_json(info: &libnspire::info::Info, bus: u8, addr: u8) -> std::result::Result<String, serde_json::Error> {
  let mut json = serde_json::to_string(info)?;
  if let Ok(serde_json::Value::Object(mut obj)) = serde_json::from_str::<serde_json::Value>(&json) {
    obj.insert("family".to_string(), serde_json::Value::String("nspire".into()));
    if let Some(ver) = device::detect_ndless(bus, addr) {
      obj.insert("ndless".to_string(), serde_json::Value::String(ver));
    }
    json = serde_json::Value::Object(obj).to_string();
  }
  Ok(json)
}

pub type NLinkProgressCb = Option<extern "C" fn(*mut c_void, u64, u64)>;

fn progress_fn(cb: NLinkProgressCb, user: *mut c_void) -> impl FnMut(usize) {
  let mut last_total = 0usize;
  move |remaining: usize| {
    if last_total < remaining {
      last_total = remaining;
    }
    let total = last_total.max(remaining) as u64;
    crate::progress::update(remaining as u64, total);
    if let Some(cb) = cb {
      cb(user, remaining as u64, total);
    }
  }
}

#[no_mangle]
pub extern "C" fn nlink_progress_get(out_done: *mut u64, out_total: *mut u64) {
  let (done, total) = crate::progress::get();
  if !out_done.is_null() {
    unsafe { *out_done = done };
  }
  if !out_total.is_null() {
    unsafe { *out_total = total };
  }
}

#[no_mangle]
pub extern "C" fn nlink_enumerate(out_json: *mut NLinkString, out_err: *mut NLinkString) -> c_int {
  fill_empty(out_json);
  fill_empty(out_err);
  match device::enumerate() {
    Ok(list) => match serde_json::to_string(&list) {
      Ok(json) => {
        fill_ok(out_json, &json);
        0
      }
      Err(e) => fill_err(out_err, e),
    },
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_open(
  bus: u8,
  addr: u8,
  out_json: *mut NLinkString,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_json);
  fill_empty(out_err);
  #[cfg(target_os = "android")]
  {
    let _ = (bus, addr);
    return fill_err(out_err, "Use nlink_open_android on Android");
  }
  #[cfg(not(target_os = "android"))]
  match device::open(bus, addr) {
    Ok(device::Opened::Nspire(info)) => match info_json(&info, bus, addr) {
      Ok(json) => {
        fill_ok(out_json, &json);
        0
      }
      Err(e) => fill_err(out_err, e),
    },
    Ok(device::Opened::Link(json)) => {
      fill_ok(out_json, &json.to_string());
      0
    }
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_open_android(
  fd: i32,
  ep_in: u8,
  ep_out: u8,
  is_cx2: u8,
  out_json: *mut NLinkString,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_json);
  fill_empty(out_err);
  #[cfg(not(target_os = "android"))]
  {
    let _ = (fd, ep_in, ep_out, is_cx2);
    return fill_err(out_err, "nlink_open_android is only available on Android");
  }
  #[cfg(target_os = "android")]
  match device::open_android(fd, ep_in, ep_out, is_cx2 != 0) {
    Ok(json) => {
      fill_ok(out_json, &json.to_string());
      0
    }
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_open_android_product(
  fd: i32,
  ep_in: u8,
  ep_out: u8,
  product: u16,
  out_json: *mut NLinkString,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_json);
  fill_empty(out_err);
  #[cfg(not(target_os = "android"))]
  {
    let _ = (fd, ep_in, ep_out, product);
    return fill_err(out_err, "nlink_open_android_product is only available on Android");
  }
  #[cfg(target_os = "android")]
  match device::open_android_product(fd, ep_in, ep_out, product) {
    Ok(json) => {
      fill_ok(out_json, &json.to_string());
      0
    }
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_close(bus: u8, addr: u8, out_err: *mut NLinkString) -> c_int {
  fill_empty(out_err);
  match device::close(bus, addr) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_info(
  bus: u8,
  addr: u8,
  out_json: *mut NLinkString,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_json);
  fill_empty(out_err);
  if let Some(result) = crate::link::info_value(bus, addr) {
    return match result {
      Ok(json) => {
        fill_ok(out_json, &json.to_string());
        0
      }
      Err(e) => fill_err(out_err, e),
    };
  }
  #[cfg(target_os = "android")]
  match device::info(bus, addr) {
    Ok(json) => {
      fill_ok(out_json, &json.to_string());
      0
    }
    Err(e) => fill_err(out_err, e),
  }
  #[cfg(not(target_os = "android"))]
  match device::info(bus, addr) {
    Ok(info) => match info_json(&info, bus, addr) {
      Ok(json) => {
        fill_ok(out_json, &json);
        0
      }
      Err(e) => fill_err(out_err, e),
    },
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_list_dir(
  bus: u8,
  addr: u8,
  path: *const c_char,
  out_json: *mut NLinkString,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_json);
  fill_empty(out_err);
  let path = match cstr(path) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  match device::list_dir(bus, addr, path) {
    Ok(list) => match serde_json::to_string(&list) {
      Ok(json) => {
        fill_ok(out_json, &json);
        0
      }
      Err(e) => fill_err(out_err, e),
    },
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_download_file(
  bus: u8,
  addr: u8,
  remote: *const c_char,
  size: u64,
  dest_dir: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let remote = match cstr(remote) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  let dest = match cstr(dest_dir) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  match device::download_file(bus, addr, remote, size, &dest, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_download_dir(
  bus: u8,
  addr: u8,
  remote: *const c_char,
  dest_dir: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let remote = match cstr(remote) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  let dest = match cstr(dest_dir) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  match device::download_dir(bus, addr, remote, &dest, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_upload_file(
  bus: u8,
  addr: u8,
  dest_dir: *const c_char,
  src: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let dest_dir = match cstr(dest_dir) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  let src = match cstr(src) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  match device::upload_file(bus, addr, dest_dir, &src, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_mkdir(
  bus: u8,
  addr: u8,
  path: *const c_char,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let path = match cstr(path) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  match device::mkdir(bus, addr, path) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_rm(
  bus: u8,
  addr: u8,
  path: *const c_char,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let path = match cstr(path) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  match device::rm(bus, addr, path) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_rmdir(
  bus: u8,
  addr: u8,
  path: *const c_char,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let path = match cstr(path) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  match device::rmdir(bus, addr, path) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_move(
  bus: u8,
  addr: u8,
  src: *const c_char,
  dest: *const c_char,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let src = match cstr(src) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  let dest = match cstr(dest) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  match device::move_file(bus, addr, src, dest) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_copy(
  bus: u8,
  addr: u8,
  src: *const c_char,
  dest: *const c_char,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let src = match cstr(src) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  let dest = match cstr(dest) {
    Ok(p) => p,
    Err(e) => return fill_err(out_err, e),
  };
  match device::copy_file(bus, addr, src, dest) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_upload_os(
  bus: u8,
  addr: u8,
  src: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let src = match cstr(src) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  match device::upload_os(bus, addr, &src, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_backup(
  bus: u8,
  addr: u8,
  dest: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let dest = match cstr(dest) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  match device::backup(bus, addr, &dest, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_rom_dump(
  bus: u8,
  addr: u8,
  dest: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let dest = match cstr(dest) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  #[cfg(target_os = "android")]
  {
    let _ = (bus, addr, dest, progress, user);
    return fill_err(out_err, "Use nlink_rom_dump_android on Android");
  }
  #[cfg(not(target_os = "android"))]
  match device::rom_dump(bus, addr, &dest, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_rom_dump_android(
  fd: i32,
  ep_in: u8,
  ep_out: u8,
  dest: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let dest = match cstr(dest) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  #[cfg(not(target_os = "android"))]
  {
    let _ = (fd, ep_in, ep_out, dest, progress);
    return fill_err(out_err, "nlink_rom_dump_android is only available on Android");
  }
  #[cfg(target_os = "android")]
  match device::rom_dump(fd, ep_in, ep_out, &dest, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_restore(
  bus: u8,
  addr: u8,
  src: *const c_char,
  cb: NLinkProgressCb,
  user: *mut c_void,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  let src = match cstr(src) {
    Ok(p) => PathBuf::from(p),
    Err(e) => return fill_err(out_err, e),
  };
  let mut progress = progress_fn(cb, user);
  match device::restore(bus, addr, &src, &mut progress) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[repr(C)]
pub struct NLinkImage {
  pub rgba: *mut u8,
  pub width: i32,
  pub height: i32,
  pub stride: i32,
}

#[no_mangle]
pub unsafe extern "C" fn nlink_image_free(img: NLinkImage) {
  if img.rgba.is_null() || img.height <= 0 || img.stride <= 0 {
    return;
  }
  let len = (img.stride as usize) * (img.height as usize);
  drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(img.rgba, len)));
}

#[no_mangle]
pub extern "C" fn nlink_set_io_timeout(ms: u32) {
  unsafe { libnspire_sys::nspire_set_io_timeout(ms) }
}

#[no_mangle]
pub extern "C" fn nlink_screenshot(
  bus: u8,
  addr: u8,
  out: *mut NLinkImage,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  if !out.is_null() {
    unsafe {
      *out = NLinkImage {
        rgba: ptr::null_mut(),
        width: 0,
        height: 0,
        stride: 0,
      };
    }
  }
  match device::screenshot(bus, addr) {
    Ok(shot) => {
      if !out.is_null() {
        let width = shot.width as i32;
        let height = shot.height as i32;
        let stride = width * 4;
        let boxed = shot.rgba.into_boxed_slice();
        let ptr_rgba = Box::into_raw(boxed).cast::<u8>();
        unsafe {
          *out = NLinkImage {
            rgba: ptr_rgba,
            width,
            height,
            stride,
          };
        }
      }
      0
    }
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_view_frame(
  bus: u8,
  addr: u8,
  out: *mut NLinkImage,
  out_err: *mut NLinkString,
) -> c_int {
  fill_empty(out_err);
  if !out.is_null() {
    unsafe {
      *out = NLinkImage {
        rgba: ptr::null_mut(),
        width: 0,
        height: 0,
        stride: 0,
      };
    }
  }
  match device::view_frame(bus, addr) {
    Ok(shot) => {
      if !out.is_null() {
        let width = shot.width as i32;
        let height = shot.height as i32;
        let stride = width * 4;
        let boxed = shot.rgba.into_boxed_slice();
        let ptr_rgba = Box::into_raw(boxed).cast::<u8>();
        unsafe {
          *out = NLinkImage {
            rgba: ptr_rgba,
            width,
            height,
            stride,
          };
        }
      }
      0
    }
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_exit_exam_mode(bus: u8, addr: u8, out_err: *mut NLinkString) -> c_int {
  fill_empty(out_err);
  match device::exit_exam_mode(bus, addr) {
    Ok(()) => 0,
    Err(e) => fill_err(out_err, e),
  }
}

#[no_mangle]
pub extern "C" fn nlink_cli_run() -> c_int {
  #[cfg(target_os = "android")]
  {
    -1
  }
  #[cfg(not(target_os = "android"))]
  if crate::cli::run() {
    0
  } else {
    -1
  }
}
