use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Write};
use std::path::Path;
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use libnspire::dir::EntryType;
use libnspire::{PID, PID_CX2, VID};
use rusb::GlobalContext;
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

pub enum DeviceState {
  Open(Arc<Mutex<libnspire::Handle<GlobalContext>>>, libnspire::info::Info),
  Closed,
}

pub struct Device {
  pub name: String,
  pub device: Arc<rusb::Device<GlobalContext>>,
  pub state: DeviceState,
  pub needs_drivers: bool,
}

pub static DEVICES: std::sync::LazyLock<RwLock<HashMap<(u8, u8), Device>>> =
  std::sync::LazyLock::new(|| RwLock::new(HashMap::new()));

fn is_nspire(dev: &rusb::Device<GlobalContext>) -> rusb::Result<bool> {
  let descriptor = dev.device_descriptor()?;
  Ok(descriptor.vendor_id() == VID && matches!(descriptor.product_id(), PID | PID_CX2))
}

fn display_name(dev: &rusb::Device<GlobalContext>) -> String {
  match dev.device_descriptor() {
    Ok(d) if d.product_id() == PID_CX2 => "TI-Nspire CX II".to_string(),
    _ => "TI-Nspire".to_string(),
  }
}

fn add_device(dev: Arc<rusb::Device<GlobalContext>>) -> rusb::Result<((u8, u8), Device)> {
  if !is_nspire(&dev)? {
    return Err(rusb::Error::Other);
  }
  Ok((
    (dev.bus_number(), dev.address()),
    Device {
      name: display_name(&dev),
      device: dev,
      state: DeviceState::Closed,
      needs_drivers: false,
    },
  ))
}

pub fn enumerate() -> Result<Vec<ListedDevice>> {
  let usb_devices: Vec<_> = rusb::devices()?.iter().collect();
  let mut map = DEVICES.write().unwrap();
  map.retain(|k, _| {
    usb_devices
      .iter()
      .any(|d| d.bus_number() == k.0 && d.address() == k.1)
  });
  let mut listed = Vec::new();
  for usb in usb_devices {
    let key = (usb.bus_number(), usb.address());
    if !map.contains_key(&key) {
      if let Ok((k, device)) = add_device(Arc::new(usb)) {
        map.insert(k, device);
      }
    }
    if let Some(device) = map.get(&key) {
      listed.push(ListedDevice {
        bus_number: key.0,
        address: key.1,
        name: device.name.clone(),
        is_cx_ii: device
          .device
          .device_descriptor()
          .map(|d| d.product_id() == PID_CX2)
          .unwrap_or(false),
        needs_drivers: device.needs_drivers,
      });
    }
  }
  Ok(listed)
}

fn find_usb(bus: u8, addr: u8) -> Result<rusb::Device<GlobalContext>> {
  let list = rusb::devices()?;
  list
    .iter()
    .find(|d| d.bus_number() == bus && d.address() == addr)
    .ok_or_else(|| NlinkError::from("Failed to find device"))
}

fn open_handle(usb: rusb::Device<GlobalContext>) -> Result<(libnspire::Handle<GlobalContext>, libnspire::info::Info)> {
  let mut last_err: Option<NlinkError> = None;
  for attempt in 0..4 {
    if attempt > 0 {
      std::thread::sleep(Duration::from_millis(200 * attempt as u64));
    }
    let opened = match usb.open() {
      Ok(h) => h,
      Err(e) => {
        last_err = Some(e.into());
        continue;
      }
    };
    match libnspire::Handle::new(opened) {
      Ok(handle) => match handle.info() {
        Ok(info) => return Ok((handle, info)),
        Err(e) => last_err = Some(e.into()),
      },
      Err(e) => last_err = Some(e.into()),
    }
  }
  Err(last_err.unwrap_or_else(|| NlinkError::from("Failed to open calculator")))
}

pub fn open(bus: u8, addr: u8) -> Result<libnspire::info::Info> {
  {
    let map = DEVICES.read().unwrap();
    if let Some(device) = map.get(&(bus, addr)) {
      if let DeviceState::Open(_, info) = &device.state {
        return Ok(info.clone());
      }
    }
  }
  let usb = find_usb(bus, addr)?;
  let (handle, info) = open_handle(usb)?;
  let mut map = DEVICES.write().unwrap();
  let device = map
    .get_mut(&(bus, addr))
    .ok_or_else(|| NlinkError::from("Device lost"))?;
  device.state = DeviceState::Open(Arc::new(Mutex::new(handle)), info.clone());
  Ok(info)
}

pub fn close(bus: u8, addr: u8) -> Result<()> {
  let mut map = DEVICES.write().unwrap();
  let device = map
    .get_mut(&(bus, addr))
    .ok_or_else(|| NlinkError::from("Device lost"))?;
  device.state = DeviceState::Closed;
  Ok(())
}

fn with_handle<T>(
  bus: u8,
  addr: u8,
  f: impl FnOnce(&libnspire::Handle<GlobalContext>) -> Result<T>,
) -> Result<T> {
  let handle = {
    let map = DEVICES.read().unwrap();
    let device = map
      .get(&(bus, addr))
      .ok_or_else(|| NlinkError::from("Failed to find device"))?;
    match &device.state {
      DeviceState::Open(handle, _) => handle.clone(),
      DeviceState::Closed => return Err("Device closed".into()),
    }
  };
  let guard = handle
    .lock()
    .map_err(|_| NlinkError::from("Device lock poisoned"))?;
  f(&guard)
}

pub fn info(bus: u8, addr: u8) -> Result<libnspire::info::Info> {
  with_handle(bus, addr, |h| Ok(h.info()?))
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
    let r = ((r5 << 3) | (r5 >> 2)) as u8;
    let g = ((g6 << 2) | (g6 >> 4)) as u8;
    let b = ((b5 << 3) | (b5 >> 2)) as u8;
    out.extend_from_slice(&[r, g, b, 255]);
  }
  out
}

fn gray8_to_rgba(data: &[u8], pixels: usize) -> Vec<u8> {
  let mut out = Vec::with_capacity(pixels * 4);
  for &v in data.iter().take(pixels) {
    out.extend_from_slice(&[v, v, v, 255]);
  }
  out
}

pub fn screenshot(bus: u8, addr: u8) -> Result<Screenshot> {
  with_handle(bus, addr, |h| {
    let img = h.screenshot()?;
    let pixels = img.width as usize * img.height as usize;
    let rgba = match img.bpp {
      16 => rgb565le_to_rgba(&img.data, pixels),
      8 => gray8_to_rgba(&img.data, pixels),
      other => {
        return Err(NlinkError::from(format!(
          "Unsupported screenshot depth: {other} bpp"
        )))
      }
    };
    if rgba.len() != pixels * 4 {
      return Err(NlinkError::from("Screenshot data was truncated"));
    }
    Ok(Screenshot {
      width: img.width,
      height: img.height,
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

const ZEHN_SIGNATURE: u32 = 0x6e68_655a; // "Zehn"
const ZEHN_FLAG_EXECUTABLE_VERSION: u8 = 10;

fn format_ndless_version(v: u32) -> String {
  if (2000..=2099).contains(&v) {
    format!("r{v}")
  } else if (20..100).contains(&v) {
    format!("{}.{}", v / 10, v % 10)
  } else {
    v.to_string()
  }
}

fn zehn_executable_version(data: &[u8]) -> Option<u32> {
  let limit = data.len().min(20 * 1024).saturating_sub(32);
  let mut i = 0;
  while i <= limit {
    let sig = u32::from_le_bytes(data[i..i + 4].try_into().ok()?);
    let ver = u32::from_le_bytes(data[i + 4..i + 8].try_into().ok()?);
    if sig == ZEHN_SIGNATURE && ver == 1 {
      let reloc_count = u32::from_le_bytes(data[i + 12..i + 16].try_into().ok()?) as usize;
      let flag_count = u32::from_le_bytes(data[i + 16..i + 20].try_into().ok()?) as usize;
      let flags_off = i.checked_add(32)?.checked_add(reloc_count.checked_mul(4)?)?;
      let flags_end = flags_off.checked_add(flag_count.checked_mul(4)?)?;
      if flags_end > data.len() {
        return None;
      }
      for f in 0..flag_count {
        let off = flags_off + f * 4;
        let raw = u32::from_le_bytes(data[off..off + 4].try_into().ok()?);
        let ftype = (raw & 0xff) as u8;
        let fdata = raw >> 8;
        if ftype == ZEHN_FLAG_EXECUTABLE_VERSION {
          return Some(fdata);
        }
      }
      return None;
    }
    i += 4;
  }
  None
}

/// Returns a display version such as `r2022` or `4.5` if Ndless resources are
/// on the calculator. `None` if Ndless does not appear to be installed.
pub fn detect_ndless(bus: u8, addr: u8) -> Option<String> {
  const CANDIDATE_DIRS: &[&str] = &["/ndless", "ndless", "/documents/ndless"];
  for dir in CANDIDATE_DIRS {
    let entries = match list_dir(bus, addr, dir) {
      Ok(e) => e,
      Err(_) => continue,
    };
    let resources = match entries
      .iter()
      .find(|e| !e.is_dir && e.path.eq_ignore_ascii_case("ndless_resources.tns"))
    {
      Some(e) => e,
      None => continue,
    };
    let remote = if dir.ends_with('/') {
      format!("{dir}{}", resources.path)
    } else {
      format!("{dir}/{}", resources.path)
    };
    let size = resources.size;
    if size == 0 || size > 4 * 1024 * 1024 {
      return Some(String::new());
    }
    let bytes = with_handle(bus, addr, |h| {
      let len = usize::try_from(size).map_err(|_| NlinkError::from("too large"))?;
      let mut buf = vec![0; len];
      h.read_file(&remote, &mut buf, &mut |_| {})?;
      Ok(buf)
    });
    return match bytes {
      Ok(buf) => Some(
        zehn_executable_version(&buf)
          .map(format_ndless_version)
          .unwrap_or_default(),
      ),
      Err(_) => Some(String::new()),
    };
  }
  None
}

pub fn list_dir(bus: u8, addr: u8, path: &str) -> Result<Vec<FileInfo>> {
  let path = if path.is_empty() { "/" } else { path };
  with_handle(bus, addr, |h| {
    let dir = h.list_dir(path)?;
    Ok(
      dir
        .iter()
        .map(|file| FileInfo {
          path: file.name().to_string_lossy().to_string(),
          is_dir: file.entry_type() == EntryType::Directory,
          date: file.date(),
          size: file.size(),
        })
        .collect(),
    )
  })
}

pub fn download_file(
  bus: u8,
  addr: u8,
  remote: &str,
  size: u64,
  dest_dir: &Path,
  progress: &mut dyn FnMut(usize),
) -> Result<()> {
  if size > MAX_FILE_SIZE {
    return Err(format!(
      "File is {} bytes, which exceeds the {} byte safety limit. This can indicate a corrupted directory entry.",
      size, MAX_FILE_SIZE
    )
    .into());
  }
  let remote = remote.to_string();
  let dest_dir = dest_dir.to_path_buf();
  with_handle(bus, addr, move |h| {
    let len = usize::try_from(size).map_err(|_| NlinkError::from("File is too large to download on this platform"))?;
    let mut buf = vec![0; len];
    h.read_file(&remote, &mut buf, progress)?;
    std::fs::create_dir_all(&dest_dir)?;
    if let Some(name) = remote.split('/').last() {
      File::create(dest_dir.join(name))?.write_all(&buf)?;
    }
    Ok(())
  })
}

fn join_nspire_path(parent: &str, name: &str) -> String {
  let parent = parent.trim_end_matches('/');
  if parent.is_empty() {
    format!("/{}", name)
  } else {
    format!("{}/{}", parent, name)
  }
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
    let child_remote = join_nspire_path(remote, &entry.path);
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
  bus: u8,
  addr: u8,
  dest_dir: &str,
  src: &Path,
  progress: &mut dyn FnMut(usize),
) -> Result<()> {
  let mut buf = vec![];
  File::open(src)?.read_to_end(&mut buf)?;
  if buf.len() as u64 > MAX_FILE_SIZE {
    return Err(format!(
      "File is {} bytes, which exceeds the {} byte safety limit.",
      buf.len(),
      MAX_FILE_SIZE
    )
    .into());
  }
  let name = src
    .file_name()
    .ok_or_else(|| NlinkError::from("Failed to get file name"))?
    .to_string_lossy()
    .to_string();
  let remote = format!("{}/{}", dest_dir.trim_end_matches('/'), name);
  with_handle(bus, addr, move |h| {
    h.write_file(&remote, &buf, progress)?;
    Ok(())
  })
}

const EXIT_TEST_MODE_TNS: &[u8] = include_bytes!("exit_test_mode.tns");
const EXIT_TEST_MODE_PATH: &str = "/Press-to-Test/Exit Test Mode.tns";

/// Upload TI's "Exit Test Mode.tns" into Press-to-Test. The handheld reboots
/// out of exam/Press-to-Test if that folder is present.
pub fn exit_exam_mode(bus: u8, addr: u8) -> Result<()> {
  with_handle(bus, addr, |h| {
    let dir = h.list_dir("/")?;
    let in_exam = dir.iter().any(|file| {
      file.entry_type() == EntryType::Directory
        && file.name().to_string_lossy() == "Press-to-Test"
    });
    if !in_exam {
      return Err(NlinkError::from(
        "Calculator does not appear to be in exam mode (no Press-to-Test folder).",
      ));
    }
    match h.write_file(EXIT_TEST_MODE_PATH, EXIT_TEST_MODE_TNS, &mut |_| {}) {
      Ok(()) => Ok(()),
      Err(libnspire::Error::NoDevice) => Ok(()),
      Err(e) => Err(e.into()),
    }
  })
}

pub fn mkdir(bus: u8, addr: u8, path: &str) -> Result<()> {
  with_handle(bus, addr, |h| {
    h.create_dir(path)?;
    Ok(())
  })
}

pub fn rm(bus: u8, addr: u8, path: &str) -> Result<()> {
  with_handle(bus, addr, |h| {
    h.delete_file(path)?;
    Ok(())
  })
}

pub fn rmdir(bus: u8, addr: u8, path: &str) -> Result<()> {
  with_handle(bus, addr, |h| {
    h.delete_dir(path)?;
    Ok(())
  })
}

pub fn move_file(bus: u8, addr: u8, src: &str, dest: &str) -> Result<()> {
  with_handle(bus, addr, |h| {
    h.move_file(src, dest)?;
    Ok(())
  })
}

pub fn copy_file(bus: u8, addr: u8, src: &str, dest: &str) -> Result<()> {
  with_handle(bus, addr, |h| {
    h.copy_file(src, dest)?;
    Ok(())
  })
}

pub fn upload_os(bus: u8, addr: u8, src: &Path, progress: &mut dyn FnMut(usize)) -> Result<()> {
  let mut buf = vec![];
  File::open(src)?.read_to_end(&mut buf)?;
  with_handle(bus, addr, move |h| {
    h.send_os(&buf, progress)?;
    Ok(())
  })
}

fn skip_backup_name(name: &str) -> bool {
  name.is_empty()
    || name == "."
    || name == ".."
    || name.eq_ignore_ascii_case("NspireLogs.zip")
}

struct TreeEntry {
  remote: String,
  is_dir: bool,
  size: u64,
}

fn collect_tree(bus: u8, addr: u8, remote: &str, out: &mut Vec<TreeEntry>) -> Result<()> {
  let entries = list_dir(bus, addr, remote)?;
  for entry in entries {
    if skip_backup_name(&entry.path) {
      continue;
    }
    let child = join_nspire_path(remote, &entry.path);
    if entry.is_dir {
      out.push(TreeEntry {
        remote: child.clone(),
        is_dir: true,
        size: 0,
      });
      collect_tree(bus, addr, &child, out)?;
    } else {
      out.push(TreeEntry {
        remote: child,
        is_dir: false,
        size: entry.size,
      });
    }
  }
  Ok(())
}

fn tar_path(remote: &str) -> Result<String> {
  let p = remote.trim_start_matches('/').replace('\\', "/");
  if p.is_empty() || p.split('/').any(|s| s == ".." || s == ".") {
    return Err(NlinkError::from("Invalid backup path"));
  }
  Ok(p)
}

fn calc_path_from_tar(name: &str) -> Result<String> {
  let normalized = name.replace('\\', "/");
  let parts: Vec<&str> = normalized
    .split('/')
    .filter(|s| !s.is_empty() && *s != ".")
    .collect();
  if parts.is_empty() || parts.iter().any(|s| *s == "..") {
    return Err(NlinkError::from("Refusing to restore an unsafe path"));
  }
  Ok(format!("/{}", parts.join("/")))
}

fn mkdir_exists_ok(bus: u8, addr: u8, path: &str) -> Result<()> {
  if path.is_empty() || path == "/" {
    return Ok(());
  }
  match mkdir(bus, addr, path) {
    Ok(()) => Ok(()),
    Err(e) => {
      if list_dir(bus, addr, path).is_ok() {
        Ok(())
      } else {
        let msg = e.to_string().to_lowercase();
        if msg.contains("exist") {
          Ok(())
        } else {
          Err(e)
        }
      }
    }
  }
}

fn ensure_dir(bus: u8, addr: u8, path: &str) -> Result<()> {
  let trimmed = path.trim_matches('/');
  if trimmed.is_empty() {
    return Ok(());
  }
  let mut cur = String::new();
  for part in trimmed.split('/') {
    if part.is_empty() {
      continue;
    }
    cur.push('/');
    cur.push_str(part);
    mkdir_exists_ok(bus, addr, &cur)?;
  }
  Ok(())
}

/// Backup the calculator filesystem to a `.tar.gz`. Skips `NspireLogs.zip`.
pub fn backup(bus: u8, addr: u8, dest: &Path, progress: &mut dyn FnMut(usize)) -> Result<()> {
  let mut tree = Vec::new();
  collect_tree(bus, addr, "/", &mut tree)?;
  let file = File::create(dest)?;
  let encoder = flate2::write::GzEncoder::new(file, flate2::Compression::default());
  let mut archive = tar::Builder::new(encoder);
  let mut wrote = 0u32;
  for entry in &tree {
    let tar_name = match tar_path(&entry.remote) {
      Ok(n) => n,
      Err(_) => continue,
    };
    if entry.is_dir {
      let mut header = tar::Header::new_gnu();
      header.set_path(&tar_name)?;
      header.set_entry_type(tar::EntryType::Directory);
      header.set_mode(0o755);
      header.set_size(0);
      header.set_cksum();
      archive.append(&header, std::io::empty())?;
      wrote += 1;
      continue;
    }
    if entry.size > MAX_FILE_SIZE {
      continue;
    }
    let buf = match with_handle(bus, addr, |h| {
      let len = usize::try_from(entry.size).map_err(|_| NlinkError::from("too large"))?;
      let mut buf = vec![0; len];
      h.read_file(&entry.remote, &mut buf, progress)?;
      Ok(buf)
    }) {
      Ok(b) => b,
      Err(_) => continue,
    };
    let mut header = tar::Header::new_gnu();
    header.set_path(&tar_name)?;
    header.set_mode(0o644);
    header.set_size(buf.len() as u64);
    header.set_cksum();
    archive.append(&header, buf.as_slice())?;
    wrote += 1;
  }
  let encoder = archive.into_inner()?;
  encoder.finish()?;
  if wrote == 0 {
    return Err(NlinkError::from("Backup contained no files"));
  }
  Ok(())
}

/// Restore files from a `.tar.gz` created by [`backup`]. Existing files may be overwritten.
pub fn restore(bus: u8, addr: u8, src: &Path, progress: &mut dyn FnMut(usize)) -> Result<()> {
  let file = File::open(src)?;
  let decoder = flate2::read::GzDecoder::new(file);
  let mut archive = tar::Archive::new(decoder);
  let mut restored = 0u32;
  for entry in archive.entries()? {
    let mut entry = entry?;
    let name = entry.path()?.to_string_lossy().into_owned();
    let remote = match calc_path_from_tar(&name) {
      Ok(p) => p,
      Err(_) => continue,
    };
    if entry.header().entry_type().is_dir() {
      ensure_dir(bus, addr, &remote)?;
      restored += 1;
      continue;
    }
    if let Some(parent) = Path::new(&remote).parent() {
      ensure_dir(bus, addr, &parent.to_string_lossy())?;
    }
    let mut buf = Vec::new();
    entry.read_to_end(&mut buf)?;
    if buf.len() as u64 > MAX_FILE_SIZE {
      continue;
    }
    match with_handle(bus, addr, |h| {
      h.write_file(&remote, &buf, progress)?;
      Ok(())
    }) {
      Ok(()) => restored += 1,
      Err(_) => continue,
    }
  }
  if restored == 0 {
    return Err(NlinkError::from("Restore wrote no files"));
  }
  Ok(())
}
