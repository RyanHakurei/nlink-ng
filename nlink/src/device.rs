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
