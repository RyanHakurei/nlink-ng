use std::ffi::{CStr, CString};
use std::fs::File;
use std::io::{Read, Write};
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;
use std::ptr;
use std::sync::{Mutex, OnceLock};

use libnspire_sys::{
  nspire_attr, nspire_device_info, nspire_devinfo, nspire_dir_create, nspire_dir_delete,
  nspire_dir_info, nspire_dir_item, nspire_dir_type_NSPIRE_DIR, nspire_dirlist, nspire_dirlist_free,
  nspire_file_copy,
  nspire_file_delete, nspire_file_move, nspire_file_read, nspire_file_write, nspire_free,
  nspire_handle_t, nspire_image, nspire_init, nspire_os_send, nspire_screenshot, nspire_strerror,
};
use serde::Serialize;

use crate::error::{NlinkError, Result, MAX_FILE_SIZE};

#[derive(Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct ListedDevice {
  pub bus_number: u8,
  pub address: u8,
  pub name: String,
  pub is_cx_ii: bool,
  pub needs_drivers: bool,
}

#[derive(Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct FileInfo {
  pub path: String,
  pub is_dir: bool,
  pub date: u64,
  pub size: u64,
}

struct AndroidSession {
  handle: *mut nspire_handle_t,
}

unsafe impl Send for AndroidSession {}

static SESSION: OnceLock<Mutex<Option<AndroidSession>>> = OnceLock::new();

fn session() -> &'static Mutex<Option<AndroidSession>> {
  SESSION.get_or_init(|| Mutex::new(None))
}

extern "C" {
  fn nspire_android_setup(fd: c_int, ep_in: u8, ep_out: u8) -> c_int;
}

unsafe extern "C" fn report_progress(remaining: usize, _: *mut c_void) {
  crate::progress::set_remaining(remaining as u64);
}

fn nsp_err(rc: c_int) -> Result<()> {
  if rc == 0 {
    Ok(())
  } else {
    let raw = unsafe {
      let p = nspire_strerror(rc);
      if p.is_null() {
        format!("libnspire error {rc}")
      } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
      }
    };
    let msg = if raw.eq_ignore_ascii_case("busy") {
      "Calculator is busy (often after a failed transfer). Unplug it, wait a few seconds, plug it back in, then connect again.".to_string()
    } else {
      raw
    };
    Err(NlinkError(msg))
  }
}

fn with_handle<T>(f: impl FnOnce(*mut nspire_handle_t) -> Result<T>) -> Result<T> {
  let guard = session()
    .lock()
    .map_err(|_| NlinkError::from("Device lock poisoned"))?;
  let sess = guard.as_ref().ok_or_else(|| NlinkError::from("Device closed"))?;
  f(sess.handle)
}

pub fn enumerate() -> Result<Vec<ListedDevice>> {
  Ok(vec![])
}

pub fn open(_bus: u8, _addr: u8) -> Result<serde_json::Value> {
  Err("Use nlink_open_android on Android".into())
}

pub fn open_android(fd: i32, ep_in: u8, ep_out: u8, is_cx2: bool) -> Result<serde_json::Value> {
  if let Some(old) = session().lock().unwrap().take() {
    unsafe { nspire_free(old.handle) };
  }
  nsp_err(unsafe { nspire_android_setup(fd, ep_in, ep_out) })?;
  let mut handle: *mut nspire_handle_t = ptr::null_mut();
  let mut last = NlinkError::from("Failed to open calculator");
  for attempt in 0..5 {
    if attempt > 0 {
      std::thread::sleep(std::time::Duration::from_millis(700 * attempt as u64));
    }
    match nsp_err(unsafe { nspire_init(&mut handle, ptr::null_mut(), is_cx2) }) {
      Ok(()) => break,
      Err(e) => {
        last = e;
        handle = ptr::null_mut();
        if attempt == 4 {
          return Err(last);
        }
      }
    }
  }
  let mut info: nspire_devinfo = unsafe { std::mem::zeroed() };
  if let Err(e) = nsp_err(unsafe { nspire_device_info(handle, &mut info) }) {
    unsafe { nspire_free(handle) };
    return Err(e);
  }
  let json = info_to_json(&info, is_cx2);
  *session().lock().unwrap() = Some(AndroidSession { handle });
  Ok(json)
}

fn c_array_str(buf: &[c_char]) -> String {
  unsafe { CStr::from_ptr(buf.as_ptr()) }
    .to_string_lossy()
    .into_owned()
}

fn info_to_json(info: &nspire_devinfo, is_cx2: bool) -> serde_json::Value {
  let v = &info.versions[0];
  serde_json::json!({
    "name": c_array_str(&info.device_name),
    "id": c_array_str(&info.electronic_id),
    "free_storage": info.storage.free,
    "total_storage": info.storage.total,
    "free_ram": info.ram.free,
    "total_ram": info.ram.total,
    "clock_speed": info.clock_speed,
    "is_cx_ii": is_cx2,
    "version": {
      "major": v.major,
      "minor": v.minor / 10,
      "patch": v.minor % 10,
      "build": v.build,
    }
  })
}

pub fn close(_bus: u8, _addr: u8) -> Result<()> {
  if let Some(sess) = session().lock().unwrap().take() {
    unsafe { nspire_free(sess.handle) };
  }
  Ok(())
}

pub fn info(_bus: u8, _addr: u8) -> Result<serde_json::Value> {
  with_handle(|h| {
    let mut inf: nspire_devinfo = unsafe { std::mem::zeroed() };
    nsp_err(unsafe { nspire_device_info(h, &mut inf) })?;
    Ok(info_to_json(&inf, true))
  })
}

pub fn list_dir(_bus: u8, _addr: u8, path: &str) -> Result<Vec<FileInfo>> {
  let path = if path.is_empty() { "/" } else { path };
  let cpath = CString::new(path)?;
  with_handle(|h| {
    let mut dir: *mut nspire_dir_info = ptr::null_mut();
    nsp_err(unsafe { nspire_dirlist(h, cpath.as_ptr(), &mut dir) })?;
    if dir.is_null() {
      return Ok(vec![]);
    }
    let num = unsafe { (*dir).num } as usize;
    let items = unsafe { (*dir).items.as_slice(num) };
    let mut out = Vec::with_capacity(num);
    for item in items {
      let name = unsafe { CStr::from_ptr(item.name.as_ptr()) }
        .to_string_lossy()
        .into_owned();
      out.push(FileInfo {
        path: name,
        is_dir: item.type_ == nspire_dir_type_NSPIRE_DIR,
        date: item.date,
        size: item.size,
      });
    }
    unsafe { nspire_dirlist_free(dir) };
    Ok(out)
  })
}

pub fn download_file(
  _bus: u8,
  _addr: u8,
  remote: &str,
  size: u64,
  dest_dir: &Path,
  progress: &mut dyn FnMut(usize),
) -> Result<()> {
  if size > MAX_FILE_SIZE {
    return Err(format!("File is {size} bytes, which exceeds the safety limit.").into());
  }
  let cpath = CString::new(remote)?;
  let listed = usize::try_from(size).map_err(|_| NlinkError::from("too large"))?;
  with_handle(|h| {
    let mut known = listed;
    let mut item: nspire_dir_item = unsafe { std::mem::zeroed() };
    if nsp_err(unsafe { nspire_attr(h, cpath.as_ptr(), &mut item) }).is_ok() {
      known = known.max(item.size as usize);
    }
    // Pad so a slightly-wrong listing size cannot truncate the USB session.
    let mut len = known.saturating_add(256 * 1024);
    if known == 0 {
      len = len.max(1024 * 1024);
    }
    if len as u64 > MAX_FILE_SIZE {
      len = MAX_FILE_SIZE as usize;
    }
    crate::progress::reset(known as u64);
    let mut buf = vec![0u8; len];
    let mut read = 0usize;
    nsp_err(unsafe {
      nspire_file_read(
        h,
        cpath.as_ptr(),
        buf.as_mut_ptr() as _,
        buf.len(),
        &mut read,
        Some(report_progress),
        ptr::null_mut(),
      )
    })?;
    crate::progress::finish();
    std::fs::create_dir_all(dest_dir)?;
    let name = remote
      .rsplit('/')
      .find(|s| !s.is_empty())
      .ok_or_else(|| NlinkError::from("invalid remote path"))?;
    File::create(dest_dir.join(name))?.write_all(&buf[..read])?;
    progress(0);
    Ok(())
  })
}

pub fn download_dir(
  bus: u8,
  addr: u8,
  remote: &str,
  dest_dir: &Path,
  progress: &mut dyn FnMut(usize),
) -> Result<()> {
  std::fs::create_dir_all(dest_dir)?;
  let entries = list_dir(bus, addr, remote)?;
  for entry in entries {
    if entry.path.is_empty() || entry.path == "." || entry.path == ".." {
      continue;
    }
    let child_remote = if remote == "/" {
      format!("/{}", entry.path)
    } else {
      format!("{}/{}", remote.trim_end_matches('/'), entry.path)
    };
    let child_local = dest_dir.join(&entry.path);
    if entry.is_dir {
      download_dir(bus, addr, &child_remote, &child_local, progress)?;
    } else {
      download_file(bus, addr, &child_remote, entry.size, dest_dir, progress)?;
    }
  }
  Ok(())
}

pub fn upload_file(
  _bus: u8,
  _addr: u8,
  dest_dir: &str,
  src: &Path,
  progress: &mut dyn FnMut(usize),
) -> Result<()> {
  let mut buf = vec![];
  File::open(src)?.read_to_end(&mut buf)?;
  let name = src
    .file_name()
    .ok_or_else(|| NlinkError::from("Failed to get file name"))?
    .to_string_lossy();
  let remote = format!("{}/{}", dest_dir.trim_end_matches('/'), name);
  let cpath = CString::new(remote)?;
  crate::progress::reset(buf.len() as u64);
  with_handle(|h| {
    nsp_err(unsafe {
      nspire_file_write(
        h,
        cpath.as_ptr(),
        buf.as_ptr() as *mut c_void,
        buf.len(),
        Some(report_progress),
        ptr::null_mut(),
      )
    })?;
    crate::progress::finish();
    progress(0);
    Ok(())
  })
}

pub fn mkdir(_bus: u8, _addr: u8, path: &str) -> Result<()> {
  let cpath = CString::new(path)?;
  with_handle(|h| nsp_err(unsafe { nspire_dir_create(h, cpath.as_ptr()) }))
}

pub fn rm(_bus: u8, _addr: u8, path: &str) -> Result<()> {
  let cpath = CString::new(path)?;
  with_handle(|h| nsp_err(unsafe { nspire_file_delete(h, cpath.as_ptr()) }))
}

pub fn rmdir(_bus: u8, _addr: u8, path: &str) -> Result<()> {
  let cpath = CString::new(path)?;
  with_handle(|h| nsp_err(unsafe { nspire_dir_delete(h, cpath.as_ptr()) }))
}

pub fn move_file(_bus: u8, _addr: u8, src: &str, dest: &str) -> Result<()> {
  let csrc = CString::new(src)?;
  let cdst = CString::new(dest)?;
  with_handle(|h| nsp_err(unsafe { nspire_file_move(h, csrc.as_ptr(), cdst.as_ptr()) }))
}

pub fn copy_file(_bus: u8, _addr: u8, src: &str, dest: &str) -> Result<()> {
  let csrc = CString::new(src)?;
  let cdst = CString::new(dest)?;
  with_handle(|h| nsp_err(unsafe { nspire_file_copy(h, csrc.as_ptr(), cdst.as_ptr()) }))
}

pub fn upload_os(
  _bus: u8,
  _addr: u8,
  src: &Path,
  progress: &mut dyn FnMut(usize),
) -> Result<()> {
  let mut buf = vec![];
  File::open(src)?.read_to_end(&mut buf)?;
  crate::progress::reset(buf.len() as u64);
  with_handle(|h| {
    nsp_err(unsafe {
      nspire_os_send(
        h,
        buf.as_mut_ptr() as *mut c_void,
        buf.len(),
        Some(report_progress),
        ptr::null_mut(),
      )
    })?;
    crate::progress::finish();
    progress(0);
    Ok(())
  })
}

pub fn backup(
  _bus: u8,
  _addr: u8,
  _dest: &Path,
  _progress: &mut dyn FnMut(usize),
) -> Result<()> {
  Err("Backup is not available in the Android build yet.".into())
}

pub fn restore(
  _bus: u8,
  _addr: u8,
  _src: &Path,
  _progress: &mut dyn FnMut(usize),
) -> Result<()> {
  Err("Restore is not available in the Android build yet.".into())
}

pub struct Screenshot {
  pub width: u16,
  pub height: u16,
  pub rgba: Vec<u8>,
}

fn rgb565le_to_rgba(data: &[u8], pixels: usize) -> Vec<u8> {
  let mut out = Vec::with_capacity(pixels * 4);
  for chunk in data.chunks_exact(2).take(pixels) {
    let c = u16::from_le_bytes([chunk[0], chunk[1]]);
    let r5 = (c >> 11) & 0x1f;
    let g6 = (c >> 5) & 0x3f;
    let b5 = c & 0x1f;
    out.extend_from_slice(&[
      ((r5 << 3) | (r5 >> 2)) as u8,
      ((g6 << 2) | (g6 >> 4)) as u8,
      ((b5 << 3) | (b5 >> 2)) as u8,
      255,
    ]);
  }
  out
}

pub fn screenshot(_bus: u8, _addr: u8) -> Result<Screenshot> {
  with_handle(|h| {
    let mut image: *mut nspire_image = ptr::null_mut();
    nsp_err(unsafe { nspire_screenshot(h, &mut image) })?;
    if image.is_null() {
      return Err("Empty screenshot".into());
    }
    let width = unsafe { (*image).width };
    let height = unsafe { (*image).height };
    let bpp = unsafe { (*image).bbp };
    let len = (width as usize * height as usize * bpp as usize) / 8;
    let data = unsafe { (*image).data.as_slice(len) }.to_vec();
    unsafe { libnspire_sys::free(image as _) };
    let pixels = width as usize * height as usize;
    let rgba = match bpp {
      16 => rgb565le_to_rgba(&data, pixels),
      8 => data.iter().flat_map(|&v| [v, v, v, 255]).collect(),
      other => return Err(format!("Unsupported screenshot depth: {other} bpp").into()),
    };
    Ok(Screenshot {
      width,
      height,
      rgba,
    })
  })
}

pub fn screenshot_png(bus: u8, addr: u8, dest: &Path) -> Result<()> {
  let shot = screenshot(bus, addr)?;
  let file = File::create(dest)?;
  let mut encoder = png::Encoder::new(file, shot.width as u32, shot.height as u32);
  encoder.set_color(png::ColorType::Rgba);
  encoder.set_depth(png::BitDepth::Eight);
  let mut writer = encoder
    .write_header()
    .map_err(|e| NlinkError::from(e.to_string()))?;
  writer
    .write_image_data(&shot.rgba)
    .map_err(|e| NlinkError::from(e.to_string()))?;
  Ok(())
}

const EXIT_TEST_MODE_TNS: &[u8] = include_bytes!("exit_test_mode.tns");
const EXIT_TEST_MODE_PATH: &str = "/Press-to-Test/Exit Test Mode.tns";

pub fn exit_exam_mode(_bus: u8, _addr: u8) -> Result<()> {
  let entries = list_dir(0, 0, "/")?;
  if !entries.iter().any(|e| e.is_dir && e.path == "Press-to-Test") {
    return Err("Calculator does not appear to be in exam mode (no Press-to-Test folder).".into());
  }
  let cpath = CString::new(EXIT_TEST_MODE_PATH)?;
  with_handle(|h| {
    let rc = unsafe {
      nspire_file_write(
        h,
        cpath.as_ptr(),
        EXIT_TEST_MODE_TNS.as_ptr() as *mut c_void,
        EXIT_TEST_MODE_TNS.len(),
        Some(report_progress),
        ptr::null_mut(),
      )
    };
    if rc != 0 {
      let _ = nsp_err(rc);
    }
    Ok(())
  })
}

pub fn detect_ndless(_bus: u8, _addr: u8) -> Option<String> {
  None
}


