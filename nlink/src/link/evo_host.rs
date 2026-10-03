use std::collections::HashMap;
use std::path::Path;
use std::sync::{LazyLock, Mutex};

use serde_json::json;

use crate::error::{NlinkError, Result};
use crate::link::evo;
use crate::link::paths::{self, FileInfo, Var};
use crate::link::transport::BulkIo;

static SESSIONS: LazyLock<Mutex<HashMap<(u8, u8), evo::Calc<Box<dyn BulkIo + Send>>>>> =
  LazyLock::new(|| Mutex::new(HashMap::new()));

fn lock() -> std::sync::MutexGuard<'static, HashMap<(u8, u8), evo::Calc<Box<dyn BulkIo + Send>>>> {
  SESSIONS.lock().expect("evo session")
}

pub fn connected(bus: u8, addr: u8) -> bool {
  lock().contains_key(&(bus, addr))
}

pub fn close(bus: u8, addr: u8) {
  lock().remove(&(bus, addr));
}

fn info_json(model: &str, os_version: &str) -> serde_json::Value {
  let mut value = json!({
    "name": model,
    "family": "evo",
    "id": "",
    "free_storage": 0,
    "total_storage": 0,
    "free_ram": 0,
    "total_ram": 0,
    "is_cx_ii": false,
    "version": { "major": 0, "minor": 0, "patch": 0, "build": 0 }
  });
  if !os_version.is_empty() {
    value["os"] = serde_json::Value::String(os_version.to_string());
  }
  value
}

fn store<T: BulkIo + Send + 'static>(bus: u8, addr: u8, calc: evo::Calc<T>) -> Result<serde_json::Value> {
  let json = info_json(&calc.model, &calc.os_version);
  let boxed = calc.map_io(|io| Box::new(io) as Box<dyn BulkIo + Send>);
  lock().insert((bus, addr), boxed);
  Ok(json)
}

#[cfg(not(target_os = "android"))]
pub fn open_usb(bus: u8, addr: u8) -> Result<serde_json::Value> {
  close(bus, addr);
  let dev = find_usb(bus, addr)?;
  let product = dev.device_descriptor()?.product_id();
  if product != 0xe018 {
    return Err(NlinkError::from("not a TI-84 Evo"));
  }
  let io = crate::link::transport::RusbIo::open(dev, true)?;
  let calc = evo::Calc::handshake(io)?;
  store(bus, addr, calc)
}

#[cfg(not(target_os = "android"))]
fn find_usb(bus: u8, addr: u8) -> Result<rusb::Device<rusb::GlobalContext>> {
  rusb::devices()?
    .iter()
    .find(|d| d.bus_number() == bus && d.address() == addr)
    .ok_or_else(|| NlinkError::from("Failed to find device"))
}

#[cfg(target_os = "android")]
pub fn open_android(ep_in: u8, ep_out: u8) -> Result<serde_json::Value> {
  close(0, 0);
  let calc = evo::Calc::handshake(crate::link::transport::AndroidIo::new(ep_in, ep_out))?;
  store(0, 0, calc)
}

pub fn info(bus: u8, addr: u8) -> Result<serde_json::Value> {
  let map = lock();
  let calc = map.get(&(bus, addr)).ok_or_else(|| NlinkError::from("calculator session lost"))?;
  Ok(info_json(&calc.model, &calc.os_version))
}

pub fn list_dir(bus: u8, addr: u8, path: &str) -> Result<Vec<FileInfo>> {
  let mut map = lock();
  let calc = map.get_mut(&(bus, addr)).ok_or_else(|| NlinkError::from("calculator session lost"))?;
  if path.is_empty() || path == "/" || calc.vars.is_empty() {
    calc.reload()?;
  }
  paths::list(&calc.vars, path)
}

pub fn download(bus: u8, addr: u8, remote: &str, dest: &Path) -> Result<()> {
  let mut map = lock();
  let calc = map.get_mut(&(bus, addr)).ok_or_else(|| NlinkError::from("calculator session lost"))?;
  let var = paths::find(&calc.vars, remote)?.clone();
  let data = calc.get_var(&var.name, var.type_id)?;
  write_host(dest, &var, &data)
}

pub fn upload(bus: u8, addr: u8, src: &Path) -> Result<()> {
  let mut map = lock();
  let calc = map.get_mut(&(bus, addr)).ok_or_else(|| NlinkError::from("calculator session lost"))?;
  calc.put_file(src)?;
  let _ = calc.reload();
  Ok(())
}

pub fn remove(bus: u8, addr: u8, remote: &str) -> Result<()> {
  let mut map = lock();
  let calc = map.get_mut(&(bus, addr)).ok_or_else(|| NlinkError::from("calculator session lost"))?;
  let var = paths::find(&calc.vars, remote)?.clone();
  calc.delete_var(&var.name, var.type_id)?;
  let _ = calc.reload();
  Ok(())
}

pub fn screenshot(bus: u8, addr: u8) -> Result<(u16, u16, Vec<u8>)> {
  let mut map = lock();
  let calc = map.get_mut(&(bus, addr)).ok_or_else(|| NlinkError::from("calculator session lost"))?;
  calc.screenshot()
}

fn write_host(dest: &Path, var: &Var, data: &[u8]) -> Result<()> {
  std::fs::create_dir_all(dest)?;
  std::fs::write(dest.join(format!("{}.8xp2", var.name)), data)?;
  Ok(())
}
