mod dbus;
mod dusb;
mod evo;
mod paths;
mod romdump;
mod ti8x;
mod transport;

use std::collections::HashMap;
use std::path::Path;
use std::sync::{LazyLock, Mutex};

use crate::device::FileInfo;
use crate::error::{NlinkError, Result};
use transport::BulkIo;

static SESSIONS: LazyLock<Mutex<HashMap<(u8, u8), Box<dyn Ops + Send>>>> =
  LazyLock::new(|| Mutex::new(HashMap::new()));

trait Ops {
  fn info(&self) -> serde_json::Value;
  fn list(&mut self, path: &str) -> Result<Vec<FileInfo>>;
  fn download(&mut self, remote: &str, dest: &Path) -> Result<()>;
  fn upload(&mut self, dest_dir: &str, src: &Path) -> Result<()>;
  fn remove(&mut self, remote: &str) -> Result<()>;
  fn shot(&mut self) -> Result<(u16, u16, Vec<u8>)>;
  fn backup(&mut self, dest: &Path) -> Result<()>;
}

pub fn connected(bus: u8, addr: u8) -> bool {
  SESSIONS.lock().expect("link session").contains_key(&(bus, addr))
}

pub fn close(bus: u8, addr: u8) {
  SESSIONS.lock().expect("link session").remove(&(bus, addr));
}

pub fn info_value(bus: u8, addr: u8) -> Option<Result<serde_json::Value>> {
  let map = SESSIONS.lock().expect("link session");
  Some(Ok(map.get(&(bus, addr))?.info()))
}

pub fn list_dir(bus: u8, addr: u8, path: &str) -> Option<Result<Vec<FileInfo>>> {
  with_mut(bus, addr, |ops| ops.list(path))
}

pub fn download_file(bus: u8, addr: u8, remote: &str, dest: &Path) -> Option<Result<()>> {
  with_mut(bus, addr, |ops| ops.download(remote, dest))
}

pub fn upload_file(bus: u8, addr: u8, dest_dir: &str, src: &Path) -> Option<Result<()>> {
  with_mut(bus, addr, |ops| ops.upload(dest_dir, src))
}

pub fn remove(bus: u8, addr: u8, remote: &str) -> Option<Result<()>> {
  with_mut(bus, addr, |ops| ops.remove(remote))
}

pub fn screenshot(bus: u8, addr: u8) -> Option<Result<(u16, u16, Vec<u8>)>> {
  with_mut(bus, addr, |ops| ops.shot())
}

pub fn backup(
  bus: u8,
  addr: u8,
  dest: &Path,
  progress: &mut dyn FnMut(usize),
) -> Option<Result<()>> {
  with_mut(bus, addr, |ops| {
    let result = ops.backup(dest);
    progress(0);
    result
  })
}

#[cfg(not(target_os = "android"))]
pub fn rom_dump_device(dev: rusb::Device<rusb::GlobalContext>, dest: &Path) -> Result<()> {
  let io = transport::RusbIo::open(dev, false)?;
  dump_rom(io, dest)
}

#[cfg(target_os = "android")]
pub fn rom_dump_endpoints(ep_in: u8, ep_out: u8, dest: &Path) -> Result<()> {
  dump_rom(transport::AndroidIo::new(ep_in, ep_out), dest)
}

pub fn dump_rom<T: BulkIo + Send + 'static>(io: T, dest: &Path) -> Result<()> {
  let rom = romdump::dump(io)?;
  if let Some(parent) = dest.parent() {
    if !parent.as_os_str().is_empty() {
      std::fs::create_dir_all(parent)?;
    }
  }
  std::fs::write(dest, rom)?;
  Ok(())
}

pub fn unsupported(bus: u8, addr: u8) -> Option<NlinkError> {
  connected(bus, addr).then(|| NlinkError::from("Not supported on this calculator."))
}

fn with_mut<R>(bus: u8, addr: u8, f: impl FnOnce(&mut dyn Ops) -> Result<R>) -> Option<Result<R>> {
  let mut map = SESSIONS.lock().expect("link session");
  let ops = map.get_mut(&(bus, addr))?;
  Some(f(ops.as_mut()))
}

fn store(bus: u8, addr: u8, ops: Box<dyn Ops + Send>) -> Result<serde_json::Value> {
  let info = ops.info();
  SESSIONS.lock().expect("link session").insert((bus, addr), ops);
  Ok(info)
}

#[cfg(not(target_os = "android"))]
pub fn open_usb(
  bus: u8,
  addr: u8,
  dev: rusb::Device<rusb::GlobalContext>,
  product: u16,
) -> Result<serde_json::Value> {
  close(bus, addr);
  let io = transport::RusbIo::open(dev, product == 0xe018)?;
  open_product(bus, addr, product, io)
}

#[cfg(target_os = "android")]
pub fn open_fd(ep_in: u8, ep_out: u8, product: u16) -> Result<serde_json::Value> {
  close(0, 0);
  open_product(0, 0, product, transport::AndroidIo::new(ep_in, ep_out))
}

fn open_product<T: BulkIo + Send + 'static>(
  bus: u8,
  addr: u8,
  product: u16,
  io: T,
) -> Result<serde_json::Value> {
  let ops: Box<dyn Ops + Send> = match product {
    0xe001 => Box::new(dbus::Calc::handshake(io)?),
    0xe018 => Box::new(evo::Calc::handshake(io)?),
    _ => Box::new(dusb::Calc::handshake(io)?),
  };
  store(bus, addr, ops)
}

fn info_value_of(
  family: &str,
  model: &str,
  free_ram: u64,
  total_ram: u64,
  free_flash: u64,
  total_flash: u64,
  major: u64,
  minor: u64,
  patch: u64,
) -> serde_json::Value {
  serde_json::json!({
    "name": model,
    "family": family,
    "id": "",
    "free_storage": free_flash,
    "total_storage": total_flash,
    "free_ram": free_ram,
    "total_ram": total_ram,
    "is_cx_ii": false,
    "version": { "major": major, "minor": minor, "patch": patch, "build": 0 }
  })
}

impl<T: BulkIo> Ops for dusb::Calc<T> {
  fn info(&self) -> serde_json::Value {
    let mut value = info_value_of(
      "dusb",
      &self.model,
      self.free_ram,
      self.total_ram,
      self.free_flash,
      self.total_flash,
      self.os_major as u64,
      self.os_minor as u64,
      self.os_patch as u64,
    );
    if let Some(clock) = &self.clock {
      value["clock"] = serde_json::Value::String(clock.clone());
    }
    if let Some(battery) = &self.battery {
      value["battery"] = serde_json::Value::String(battery.clone());
    }
    if self.model.to_ascii_lowercase().contains("ce") && self.free_ram == 0 {
      value["ram_note"] = serde_json::json!("CE reports no free RAM while the home screen is up");
    }
    value
  }

  fn list(&mut self, path: &str) -> Result<Vec<FileInfo>> {
    if path.is_empty() || path == "/" || self.vars.is_empty() {
      self.reload()?;
    }
    paths::list(&self.vars, path)
  }

  fn download(&mut self, remote: &str, dest: &Path) -> Result<()> {
    let var = paths::find(&self.vars, remote)?.clone();
    let data = self.get_var(&var.name, var.type_id)?;
    write_host(dest, &var, &data, false)
  }

  fn upload(&mut self, dest_dir: &str, src: &Path) -> Result<()> {
    let bytes = std::fs::read(src)?;
    let archived_dir = paths::dest_archived(dest_dir);
    for entry in ti8x::parse(&bytes)? {
      self.put_var(
        &entry.var.name,
        entry.var.type_id,
        entry.var.archived || archived_dir,
        entry.var.version,
        &entry.data,
      )?;
    }
    let _ = self.reload();
    Ok(())
  }

  fn remove(&mut self, remote: &str) -> Result<()> {
    let var = paths::find(&self.vars, remote)?.clone();
    self.delete_var(&var.name, var.type_id)?;
    let _ = self.reload();
    Ok(())
  }

  fn shot(&mut self) -> Result<(u16, u16, Vec<u8>)> {
    self.screenshot()
  }

  fn backup(&mut self, dest: &Path) -> Result<()> {
    if !dusb::can_ram_backup(&self.model) {
      return Err(NlinkError::from(
        "A full RAM backup is not available on the CE. Copy variables from the file list instead.",
      ));
    }
    if self.vars.is_empty() {
      self.reload()?;
    }
    let ram: Vec<_> = self
      .vars
      .iter()
      .filter(|var| !var.archived && var.type_id != 0x24)
      .cloned()
      .collect();
    if ram.is_empty() {
      return Err(NlinkError::from("there are no variables in RAM to back up"));
    }
    crate::progress::reset(ram.len() as u64);
    let mut entries = Vec::new();
    for (index, var) in ram.iter().enumerate() {
      let data = self.get_var(&var.name, var.type_id)?;
      entries.push(ti8x::Entry {
        var: var.clone(),
        data,
      });
      crate::progress::set_remaining((ram.len() - index - 1) as u64);
    }
    std::fs::write(dest, ti8x::write_group(&entries)?)?;
    crate::progress::finish();
    Ok(())
  }
}

impl<T: BulkIo> Ops for dbus::Calc<T> {
  fn info(&self) -> serde_json::Value {
    info_value_of("silverlink", &self.model, 0, 0, 0, 0, 0, 0, 0)
  }

  fn list(&mut self, path: &str) -> Result<Vec<FileInfo>> {
    if path.is_empty() || path == "/" || self.vars.is_empty() {
      self.reload()?;
    }
    paths::list(&self.vars, path)
  }

  fn download(&mut self, remote: &str, dest: &Path) -> Result<()> {
    let var = paths::find(&self.vars, remote)?.clone();
    let data = self.get_var(&var.name, var.type_id)?;
    write_host(dest, &var, &data, false)
  }

  fn upload(&mut self, dest_dir: &str, src: &Path) -> Result<()> {
    let bytes = std::fs::read(src)?;
    let archived_dir = paths::dest_archived(dest_dir);
    for entry in ti8x::parse(&bytes)? {
      self.put_var(
        &entry.var.name,
        entry.var.type_id,
        entry.var.archived || archived_dir,
        &entry.data,
      )?;
    }
    let _ = self.reload();
    Ok(())
  }

  fn remove(&mut self, remote: &str) -> Result<()> {
    let var = paths::find(&self.vars, remote)?.clone();
    self.delete_var(&var.name, var.type_id)?;
    let _ = self.reload();
    Ok(())
  }

  fn shot(&mut self) -> Result<(u16, u16, Vec<u8>)> {
    self.screenshot()
  }

  fn backup(&mut self, dest: &Path) -> Result<()> {
    let bytes = self.recv_backup()?;
    std::fs::write(dest, bytes)?;
    Ok(())
  }
}

impl<T: BulkIo> Ops for evo::Calc<T> {
  fn info(&self) -> serde_json::Value {
    let mut value = info_value_of("evo", &self.model, 0, 0, 0, 0, 0, 0, 0);
    if !self.os_version.is_empty() {
      value["os"] = serde_json::Value::String(self.os_version.clone());
    }
    value
  }

  fn list(&mut self, path: &str) -> Result<Vec<FileInfo>> {
    if path.is_empty() || path == "/" || self.vars.is_empty() {
      self.reload()?;
    }
    paths::list(&self.vars, path)
  }

  fn download(&mut self, remote: &str, dest: &Path) -> Result<()> {
    let var = paths::find(&self.vars, remote)?.clone();
    let data = self.get_var(&var.name, var.type_id)?;
    write_host(dest, &var, &data, true)
  }

  fn upload(&mut self, _dest_dir: &str, src: &Path) -> Result<()> {
    self.put_file(src)?;
    let _ = self.reload();
    Ok(())
  }

  fn remove(&mut self, remote: &str) -> Result<()> {
    let var = paths::find(&self.vars, remote)?.clone();
    self.delete_var(&var.name, var.type_id)?;
    let _ = self.reload();
    Ok(())
  }

  fn shot(&mut self) -> Result<(u16, u16, Vec<u8>)> {
    self.screenshot()
  }

  fn backup(&mut self, _dest: &Path) -> Result<()> {
    Err(NlinkError::from("Not supported on this calculator."))
  }
}

fn write_host(dest: &Path, var: &paths::Var, data: &[u8], evo: bool) -> Result<()> {
  std::fs::create_dir_all(dest)?;
  let filename = if evo {
    format!("{}.8xp2", var.name)
  } else {
    format!("{}.{}", var.name, paths::file_ext(var.type_id))
  };
  if evo {
    std::fs::write(dest.join(filename), data)?;
    return Ok(());
  }
  let bytes = ti8x::write_8xp(&ti8x::Entry {
    var: var.clone(),
    data: data.to_vec(),
  })?;
  std::fs::write(dest.join(filename), bytes)?;
  Ok(())
}
