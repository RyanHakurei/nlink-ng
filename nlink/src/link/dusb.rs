//! DirectLink / DUSB for the TI-84 Plus, TI-84 Plus CE, and the USB TI-83 Plus.
//! Packet layout follows Benjamin Moody's public "TI-84 Plus USB Protocol" notes
//! (31 Mar 2006) and the same raw/virtual headers used by the GPL-3 ticalc-usb project.
//! This file does not include libticalcs source.

use crate::error::{NlinkError, Result, MAX_FILE_SIZE};
use crate::link::paths::Var;
use crate::link::transport::{BulkIo, ByteBuf};

const RAW_BUF_REQ: u8 = 1;
const RAW_BUF_ALLOC: u8 = 2;
const RAW_VIRT: u8 = 3;
const RAW_VIRT_LAST: u8 = 4;
const RAW_ACK: u8 = 5;

const V_PING: u16 = 0x0001;
const V_PARM_REQ: u16 = 0x0007;
const V_PARM_DATA: u16 = 0x0008;
const V_DIR_REQ: u16 = 0x0009;
const V_VAR_HDR: u16 = 0x000a;
const V_RTS: u16 = 0x000b;
const V_VAR_REQ: u16 = 0x000c;
const V_VAR_DATA: u16 = 0x000d;
const V_DEL: u16 = 0x0010;
const V_MODE_ACK: u16 = 0x0012;
const V_DATA_ACK: u16 = 0xaa00;
const V_DELAY: u16 = 0xbb00;
const V_EOT: u16 = 0xdd00;
const V_ERROR: u16 = 0xee00;

const MODE_NORMAL: [u8; 10] = [0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 0xd0];

struct Raw {
  kind: u8,
  data: Vec<u8>,
}

struct Virt {
  kind: u16,
  data: Vec<u8>,
}

pub struct Calc<T: BulkIo> {
  io: T,
  rx: ByteBuf,
  max_raw: usize,
  pub model: String,
  pub product: u32,
  pub free_ram: u64,
  pub free_flash: u64,
  pub total_ram: u64,
  pub total_flash: u64,
  pub clock: Option<String>,
  pub battery: Option<String>,
  pub os_major: u16,
  pub os_minor: u8,
  pub os_patch: u8,
  pub vars: Vec<Var>,
}

impl<T: BulkIo> Calc<T> {
  pub fn handshake(io: T) -> Result<Self> {
    let mut calc = Self {
      io,
      rx: ByteBuf::new(),
      max_raw: 250,
      model: "TI-84 Plus".into(),
      product: 0,
      free_ram: 0,
      free_flash: 0,
      total_ram: 0,
      total_flash: 0,
      clock: None,
      battery: None,
      os_major: 0,
      os_minor: 0,
      os_patch: 0,
      vars: Vec::new(),
    };
    calc.refresh_info()?;
    Ok(calc)
  }

  pub fn refresh_info(&mut self) -> Result<()> {
    self.begin_op()?;
    let data = self.parameters(&[
      0x0001, 0x0002, 0x000b, 0x000c, 0x000d, 0x000e, 0x000f, 0x0010, 0x0011, 0x0024, 0x0025, 0x002d,
    ])?;
    if let Some(bytes) = data.get(&0x0001) {
      self.product = be_uint(bytes) as u32;
    }
    if let Some(bytes) = data.get(&0x0002) {
      let name = std::str::from_utf8(bytes)
        .unwrap_or("")
        .trim_matches(char::from(0))
        .trim();
      if !name.is_empty() {
        self.model = name.to_string();
      }
    }
    if self.model == "TI-84 Plus" {
      self.model = match self.product {
        0x04 => "TI-83 Plus".into(),
        0x0a => "TI-84 Plus".into(),
        0x0b => "TI-82 Advanced".into(),
        0x0f => "TI-84 Plus C Silver Edition".into(),
        0x13 => "TI-84 Plus CE".into(),
        0x15 => "TI-82 Advanced Edition Python".into(),
        _ => self.model.clone(),
      };
    }
    if let Some(bytes) = data.get(&0x000b) {
      if bytes.len() >= 4 {
        self.os_major = u16::from_be_bytes([bytes[0], bytes[1]]);
        self.os_minor = bytes[2];
        self.os_patch = bytes[3];
      }
    }
    if let Some(bytes) = data.get(&0x000d).or_else(|| data.get(&0x000c)) {
      self.total_ram = be_uint(bytes);
    }
    if let Some(bytes) = data.get(&0x0010).or_else(|| data.get(&0x000f)) {
      self.total_flash = be_uint(bytes);
    }
    if let Some(bytes) = data.get(&0x000e) {
      self.free_ram = be_uint(bytes);
    }
    if let Some(bytes) = data.get(&0x0011) {
      self.free_flash = be_uint(bytes);
    }
    let clock_on = data.get(&0x0024).and_then(|b| b.first().copied()).unwrap_or(1) != 0;
    if let Some(bytes) = data.get(&0x0025) {
      self.clock = Some(if clock_on {
        format_ti_clock(be_uint(bytes))
      } else {
        "off".into()
      });
    }
    if let Some(byte) = data.get(&0x002d).and_then(|b| b.first().copied()) {
      self.battery = Some(if byte == 0 { "low" } else { "good" }.into());
    }
    self.max_raw = effective_buffer(self.max_raw, &self.model);
    Ok(())
  }

  pub fn reload(&mut self) -> Result<()> {
    self.begin_op()?;
    self.send_virt(V_DIR_REQ, &dir_request())?;
    let mut vars = Vec::new();
    loop {
      let pkt = self.recv_virt()?;
      match pkt.kind {
        V_VAR_HDR => vars.push(parse_var_header(&pkt.data)?),
        V_EOT => break,
        V_ERROR => {
          return Err(NlinkError::from(format!(
            "calculator rejected the directory ({})",
            error_code(&pkt.data)
          )))
        }
        other => {
          return Err(NlinkError::from(format!(
            "unexpected directory packet {other:#06x}"
          )))
        }
      }
    }
    self.vars = vars;
    Ok(())
  }

  pub fn get_var(&mut self, name: &str, type_id: u8) -> Result<Vec<u8>> {
    self.begin_op()?;
    self.send_virt(V_VAR_REQ, &var_request(name, type_id))?;
    let hdr = self.recv_virt()?;
    if hdr.kind == V_ERROR {
      return Err(NlinkError::from(format!(
        "calculator rejected the request ({})",
        error_code(&hdr.data)
      )));
    }
    if hdr.kind != V_VAR_HDR {
      return Err(NlinkError::from("calculator did not return a variable header"));
    }
    let body = self.recv_virt()?;
    if body.kind != V_VAR_DATA {
      return Err(NlinkError::from("calculator did not return variable data"));
    }
    if body.data.len() as u64 > MAX_FILE_SIZE {
      return Err(NlinkError::from("variable exceeds the safety limit"));
    }
    Ok(body.data)
  }

  pub fn put_var(&mut self, name: &str, type_id: u8, archived: bool, version: u32, data: &[u8]) -> Result<()> {
    if type_id == 0x23 {
      return Err(NlinkError::from("sending an operating system is not supported"));
    }
    if data.len() as u64 > MAX_FILE_SIZE {
      return Err(NlinkError::from("variable exceeds the safety limit"));
    }
    crate::progress::reset(data.len() as u64);
    self.begin_op()?;
    self.send_virt(V_RTS, &rts(name, data.len() as u32, type_id, archived, version))?;
    self.expect_virt(V_DATA_ACK)?;
    self.send_virt(V_VAR_DATA, data)?;
    crate::progress::finish();
    self.expect_virt(V_DATA_ACK)?;
    self.send_virt(V_EOT, &[])?;
    Ok(())
  }

  pub fn delete_var(&mut self, name: &str, type_id: u8) -> Result<()> {
    self.begin_op()?;
    self.send_virt(V_DEL, &delete_request(name, type_id))?;
    self.expect_virt(V_DATA_ACK)?;
    Ok(())
  }

  pub fn screenshot(&mut self) -> Result<(u16, u16, Vec<u8>)> {
    self.begin_op()?;
    let data = self.parameters(&[0x0022])?;
    let lcd = data
      .get(&0x0022)
      .ok_or_else(|| NlinkError::from("this calculator did not return a screen"))?;
    decode_lcd(lcd)
  }

  fn begin_op(&mut self) -> Result<()> {
    self.write_raw(RAW_BUF_REQ, &1024u32.to_be_bytes())?;
    let alloc = self.read_raw()?;
    if alloc.kind != RAW_BUF_ALLOC || alloc.data.len() < 4 {
      return Err(NlinkError::from(
        "calculator did not accept the USB buffer size",
      ));
    }
    let offered = u32::from_be_bytes([alloc.data[0], alloc.data[1], alloc.data[2], alloc.data[3]]) as usize;
    self.max_raw = effective_buffer(offered, &self.model).max(16);
    self.send_virt(V_PING, &MODE_NORMAL)?;
    let mode = self.recv_virt()?;
    if mode.kind == V_ERROR {
      return Err(NlinkError::from(format!(
        "calculator rejected the connection ({})",
        error_code(&mode.data)
      )));
    }
    if mode.kind != V_MODE_ACK {
      return Err(NlinkError::from("calculator did not enter link mode"));
    }
    Ok(())
  }

  fn parameters(&mut self, ids: &[u16]) -> Result<std::collections::HashMap<u16, Vec<u8>>> {
    let mut data = Vec::new();
    data.extend_from_slice(&(ids.len() as u16).to_be_bytes());
    for id in ids {
      data.extend_from_slice(&id.to_be_bytes());
    }
    self.send_virt(V_PARM_REQ, &data)?;
    let pkt = self.recv_virt_skip_delay()?;
    if pkt.kind != V_PARM_DATA {
      return Err(NlinkError::from("calculator did not return device info"));
    }
    parse_parameters(&pkt.data)
  }

  fn send_virt(&mut self, kind: u16, data: &[u8]) -> Result<()> {
    let mut virt = Vec::with_capacity(6 + data.len());
    virt.extend_from_slice(&(data.len() as u32).to_be_bytes());
    virt.extend_from_slice(&kind.to_be_bytes());
    virt.extend_from_slice(data);
    let mut off = 0;
    while off < virt.len() {
      let n = (virt.len() - off).min(self.max_raw);
      let last = off + n == virt.len();
      self.write_raw(if last { RAW_VIRT_LAST } else { RAW_VIRT }, &virt[off..off + n])?;
      let ack = self.read_raw()?;
      if ack.kind != RAW_ACK {
        return Err(NlinkError::from("calculator did not acknowledge a USB packet"));
      }
      off += n;
    }
    let done = data.len() as u64;
    if done > 0 && kind == V_VAR_DATA {
      crate::progress::set_remaining(0);
    }
    Ok(())
  }

  fn expect_virt(&mut self, kind: u16) -> Result<Virt> {
    let pkt = self.recv_virt_skip_delay()?;
    if pkt.kind == V_ERROR {
      return Err(NlinkError::from(format!(
        "calculator error {}",
        error_code(&pkt.data)
      )));
    }
    if pkt.kind != kind {
      return Err(NlinkError::from(format!(
        "unexpected calculator packet {:#06x}",
        pkt.kind
      )));
    }
    Ok(pkt)
  }

  fn recv_virt_skip_delay(&mut self) -> Result<Virt> {
    loop {
      let pkt = self.recv_virt()?;
      if pkt.kind == V_DELAY {
        continue;
      }
      return Ok(pkt);
    }
  }

  fn recv_virt(&mut self) -> Result<Virt> {
    let mut buf = Vec::new();
    loop {
      let raw = self.read_raw()?;
      match raw.kind {
        RAW_VIRT | RAW_VIRT_LAST => {
          buf.extend_from_slice(&raw.data);
          self.write_raw(RAW_ACK, &0xe000u16.to_be_bytes())?;
          if raw.kind == RAW_VIRT_LAST {
            break;
          }
        }
        RAW_ACK => continue,
        other => {
          return Err(NlinkError::from(format!(
            "unexpected raw USB packet {other}"
          )))
        }
      }
    }
    if buf.len() < 6 {
      return Err(NlinkError::from("truncated calculator packet"));
    }
    let size = u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]) as usize;
    let kind = u16::from_be_bytes([buf[4], buf[5]]);
    if buf.len() < 6 + size {
      return Err(NlinkError::from("truncated calculator packet"));
    }
    Ok(Virt {
      kind,
      data: buf[6..6 + size].to_vec(),
    })
  }

  fn write_raw(&mut self, kind: u8, data: &[u8]) -> Result<()> {
    let mut pkt = Vec::with_capacity(5 + data.len());
    pkt.extend_from_slice(&(data.len() as u32).to_be_bytes());
    pkt.push(kind);
    pkt.extend_from_slice(data);
    self.io.write_all(&pkt)
  }

  fn read_raw(&mut self) -> Result<Raw> {
    self.rx.fill_from(&mut self.io, 5)?;
    let head = self.rx.take(5)?;
    let len = u32::from_be_bytes([head[0], head[1], head[2], head[3]]) as usize;
    if len > 1024 * 1024 {
      return Err(NlinkError::from("calculator packet is too large"));
    }
    self.rx.fill_from(&mut self.io, len)?;
    let data = self.rx.take(len)?;
    Ok(Raw {
      kind: head[4],
      data,
    })
  }
}

#[allow(dead_code)]
pub fn can_rom_dump(model: &str) -> bool {
  let model = model.to_ascii_lowercase();
  model.contains("84") && !model.contains("ce") && !model.contains("evo")
}

pub fn can_ram_backup(model: &str) -> bool {
  !model.to_ascii_lowercase().contains("ce")
}

/// Seconds since 1997-01-01, which is how the 84/CE clock parameter is stored.
pub fn format_ti_clock(seconds: u64) -> String {
  const DAYS_1970_TO_1997: u64 = 9862;
  let unix_days = DAYS_1970_TO_1997 + seconds / 86400;
  let tod = seconds % 86400;
  let (year, month, day) = civil_from_unix_days(unix_days as i64);
  format!(
    "{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02}",
    tod / 3600,
    (tod % 3600) / 60,
    tod % 60
  )
}

fn civil_from_unix_days(unix_days: i64) -> (i32, u32, u32) {
  let z = unix_days + 719468;
  let era = if z >= 0 { z } else { z - 146096 } / 146097;
  let doe = (z - era * 146097) as u64;
  let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  let y = yoe as i64 + era * 400;
  let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  let mp = (5 * doy + 2) / 153;
  let day = doy - (153 * mp + 2) / 5 + 1;
  let month = if mp < 10 { mp + 3 } else { mp - 9 };
  let year = y + if month <= 2 { 1 } else { 0 };
  (year as i32, month as u32, day as u32)
}

pub fn effective_buffer(offered: usize, model: &str) -> usize {
  let mut n = offered;
  if n == 0 || n > 1024 {
    n = 1024;
  }
  // CE firmware accepts a 1024-byte allocation and then fails past 1018.
  if model.contains("CE") || model.contains("Premium") {
    n = n.min(1018);
  }
  n
}

fn dir_request() -> Vec<u8> {
  let mut data = Vec::new();
  data.extend_from_slice(&3u32.to_be_bytes());
  for id in [0x0001u16, 0x0002, 0x0003] {
    data.extend_from_slice(&id.to_be_bytes());
  }
  data.extend_from_slice(&[0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01]);
  data
}

fn rts(name: &str, size: u32, type_id: u8, archived: bool, version: u32) -> Vec<u8> {
  let mut data = name_prefix(name);
  data.extend_from_slice(&size.to_be_bytes());
  data.push(0x01);
  push_out_attrs(
    &mut data,
    &[
      (0x0002, &(0xf007_0000u32 | u32::from(type_id)).to_be_bytes()),
      (0x0003, &[u8::from(archived)]),
      (0x0008, &version.to_be_bytes()),
    ],
  );
  data
}

fn var_request(name: &str, type_id: u8) -> Vec<u8> {
  let mut data = name_prefix(name);
  data.extend_from_slice(&[0x01, 0xff, 0xff, 0xff, 0xff]);
  data.extend_from_slice(&3u16.to_be_bytes());
  for id in [0x0001u16, 0x0002, 0x0003] {
    data.extend_from_slice(&id.to_be_bytes());
  }
  data.extend_from_slice(&1u16.to_be_bytes());
  data.extend_from_slice(&0x0011u16.to_be_bytes());
  data.extend_from_slice(&4u16.to_be_bytes());
  data.extend_from_slice(&(0xf007_0000u32 | u32::from(type_id)).to_be_bytes());
  data.extend_from_slice(&[0x00, 0x00]);
  data
}

fn delete_request(name: &str, type_id: u8) -> Vec<u8> {
  let mut data = name_prefix(name);
  push_out_attrs(
    &mut data,
    &[(0x0002, &(0xf007_0000u32 | u32::from(type_id)).to_be_bytes())],
  );
  data.extend_from_slice(&[0x01, 0x00, 0x00, 0x00, 0x00]);
  data
}

fn name_prefix(name: &str) -> Vec<u8> {
  let bytes = name.as_bytes();
  let mut data = Vec::new();
  data.extend_from_slice(&(bytes.len() as u16).to_be_bytes());
  data.extend_from_slice(bytes);
  data.push(0);
  data
}

fn push_out_attrs(data: &mut Vec<u8>, attrs: &[(u16, &[u8])]) {
  data.extend_from_slice(&(attrs.len() as u16).to_be_bytes());
  for (id, value) in attrs {
    data.extend_from_slice(&id.to_be_bytes());
    data.extend_from_slice(&(value.len() as u16).to_be_bytes());
    data.extend_from_slice(value);
  }
}

pub fn parse_var_header(data: &[u8]) -> Result<Var> {
  if data.len() < 4 {
    return Err(NlinkError::from("variable header is truncated"));
  }
  let name_len = u16::from_be_bytes([data[0], data[1]]) as usize;
  if data.len() < 2 + name_len + 1 {
    return Err(NlinkError::from("variable header is truncated"));
  }
  let name = String::from_utf8_lossy(&data[2..2 + name_len]).into_owned();
  let mut pos = 2 + name_len;
  if data.get(pos) == Some(&0) {
    pos += 1;
  }
  if data.len() < pos + 2 {
    return Err(NlinkError::from("variable header is truncated"));
  }
  let count = u16::from_be_bytes([data[pos], data[pos + 1]]) as usize;
  pos += 2;
  let mut type_id = 0u8;
  let mut size = 0u64;
  let mut archived = false;
  let mut version = 0u32;
  for _ in 0..count {
    if data.len() < pos + 3 {
      break;
    }
    let id = u16::from_be_bytes([data[pos], data[pos + 1]]);
    let ok = data[pos + 2];
    pos += 3;
    if ok != 0 {
      continue;
    }
    if data.len() < pos + 2 {
      break;
    }
    let n = u16::from_be_bytes([data[pos], data[pos + 1]]) as usize;
    pos += 2;
    if data.len() < pos + n {
      return Err(NlinkError::from("variable attribute is truncated"));
    }
    let value = &data[pos..pos + n];
    pos += n;
    match id {
      0x0001 => size = be_uint(value),
      0x0002 | 0x0011 => type_id = value.last().copied().unwrap_or(0),
      0x0003 => archived = value.first().copied().unwrap_or(0) != 0,
      0x0008 => version = be_uint(value) as u32,
      _ => {}
    }
  }
  Ok(Var {
    name,
    type_id,
    size,
    archived,
    version,
  })
}

fn parse_parameters(data: &[u8]) -> Result<std::collections::HashMap<u16, Vec<u8>>> {
  if data.len() < 2 {
    return Err(NlinkError::from("parameter block is truncated"));
  }
  let count = u16::from_be_bytes([data[0], data[1]]) as usize;
  let mut pos = 2;
  let mut out = std::collections::HashMap::new();
  for _ in 0..count {
    if data.len() < pos + 3 {
      break;
    }
    let id = u16::from_be_bytes([data[pos], data[pos + 1]]);
    let ok = data[pos + 2];
    pos += 3;
    if ok != 0 {
      continue;
    }
    if data.len() < pos + 2 {
      break;
    }
    let n = u16::from_be_bytes([data[pos], data[pos + 1]]) as usize;
    pos += 2;
    if data.len() < pos + n {
      return Err(NlinkError::from("parameter value is truncated"));
    }
    out.insert(id, data[pos..pos + n].to_vec());
    pos += n;
  }
  Ok(out)
}

fn be_uint(bytes: &[u8]) -> u64 {
  let mut n = 0u64;
  for b in bytes.iter().take(8) {
    n = (n << 8) | u64::from(*b);
  }
  n
}

fn error_code(data: &[u8]) -> String {
  if data.len() >= 2 {
    format!("{:#06x}", u16::from_be_bytes([data[0], data[1]]))
  } else {
    "unknown".into()
  }
}

pub fn decode_lcd(data: &[u8]) -> Result<(u16, u16, Vec<u8>)> {
  match data.len() {
    768 => Ok((96, 64, mono96(data))),
    153_600 => Ok((320, 240, rgb565(data))),
    n => Err(NlinkError::from(format!(
      "unsupported screen size ({n} bytes)"
    ))),
  }
}

fn mono96(data: &[u8]) -> Vec<u8> {
  let mut rgba = Vec::with_capacity(96 * 64 * 4);
  for row in 0..64 {
    for col in 0..96 {
      let byte = data[row * 12 + col / 8];
      let on = byte & (0x80 >> (col % 8)) != 0;
      let v = if on { 0 } else { 255 };
      rgba.extend_from_slice(&[v, v, v, 255]);
    }
  }
  rgba
}

fn rgb565(data: &[u8]) -> Vec<u8> {
  let mut rgba = Vec::with_capacity(320 * 240 * 4);
  for chunk in data.chunks_exact(2) {
    let c = u16::from_le_bytes([chunk[0], chunk[1]]);
    let r = (c >> 11) & 0x1f;
    let g = (c >> 5) & 0x3f;
    let b = c & 0x1f;
    rgba.extend_from_slice(&[
      ((r << 3) | (r >> 2)) as u8,
      ((g << 2) | (g >> 4)) as u8,
      ((b << 3) | (b >> 2)) as u8,
      255,
    ]);
  }
  rgba
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::link::transport::Pipe;

  #[test]
  fn parses_moodys_variable_header() {
    let data = [
      0x00, 0x01, b'A', 0x00, 0x00, 0x06, 0x00, 0x02, 0x00, 0x00, 0x04, 0xf0, 0x07, 0x00, 0x00, 0x00,
      0x03, 0x00, 0x00, 0x01, 0x00, 0x00, 0x05, 0x01, 0x00, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
      0x09, 0x00, 0x41, 0x01, 0x00, 0x42, 0x01,
    ];
    let var = parse_var_header(&data).unwrap();
    assert_eq!(var.name, "A");
    assert_eq!(var.type_id, 0);
    assert_eq!(var.size, 9);
    assert!(!var.archived);
  }

  #[test]
  fn clock_epoch_is_new_year_1997() {
    assert_eq!(format_ti_clock(0), "1997-01-01 00:00:00");
    assert_eq!(format_ti_clock(3661), "1997-01-01 01:01:01");
  }

  #[test]
  fn ce_buffer_is_clamped() {
    assert_eq!(effective_buffer(1024, "TI-84 Plus CE"), 1018);
    assert_eq!(effective_buffer(250, "TI-84 Plus"), 250);
    assert_eq!(effective_buffer(4096, "TI-84 Plus"), 1024);
  }

  #[test]
  fn handshake_ping_matches_published_bytes() {
    let alloc = [0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0xfa];
    let raw_ack = [0x00, 0x00, 0x00, 0x02, 0x05, 0xe0, 0x00];
    let mode = [
      0x00, 0x00, 0x00, 0x0a, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x12, 0x00, 0x00, 0x07, 0xd0,
    ];
    let mut incoming = Vec::new();
    incoming.extend_from_slice(&alloc);
    incoming.extend_from_slice(&raw_ack);
    incoming.extend_from_slice(&mode);
    incoming.extend_from_slice(&raw_ack);
    let mut payload = Vec::new();
    payload.extend_from_slice(&1u16.to_be_bytes());
    payload.extend_from_slice(&0x0002u16.to_be_bytes());
    payload.push(0);
    let name = b"TI-84 Plus CE";
    payload.extend_from_slice(&(name.len() as u16).to_be_bytes());
    payload.extend_from_slice(name);
    let mut virt = Vec::new();
    virt.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    virt.extend_from_slice(&V_PARM_DATA.to_be_bytes());
    virt.extend_from_slice(&payload);
    let mut parm = Vec::new();
    parm.extend_from_slice(&(virt.len() as u32).to_be_bytes());
    parm.push(RAW_VIRT_LAST);
    parm.extend_from_slice(&virt);
    incoming.extend_from_slice(&parm);
    let pipe = Pipe::with_incoming(incoming);
    let calc = Calc::handshake(pipe).unwrap();
    assert_eq!(calc.model, "TI-84 Plus CE");
    assert_eq!(calc.max_raw, 250.min(1018));
    let written = &calc.io.written;
    assert_eq!(&written[..9], &[0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x04, 0x00]);
    assert!(written.windows(10).any(|w| w == MODE_NORMAL));
  }

  #[test]
  fn mono_screen_is_96_by_64() {
    let (w, h, rgba) = decode_lcd(&[0u8; 768]).unwrap();
    assert_eq!((w, h, rgba.len()), (96, 64, 96 * 64 * 4));
  }
}
