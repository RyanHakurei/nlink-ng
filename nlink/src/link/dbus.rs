//! Classic DBUS variable protocol used by a SilverLink (`0451:e001`) to talk to
//! a TI-82, TI-83, or TI-83 Plus link port.
//! Packet bytes follow the public TI-83+ link guide (machine id, command, little-endian
//! length, data, little-endian checksum of the data). The SilverLink bulk endpoints
//! carry those bytes. This has not been run against a cable.

use crate::error::{NlinkError, Result, MAX_FILE_SIZE};
use crate::link::paths::Var;
use crate::link::transport::{BulkIo, ByteBuf};

const MID_PC: u8 = 0x23;
const MID_PC_83: u8 = 0x03;
const MID_PC_82: u8 = 0x02;

const CMD_VAR: u8 = 0x06;
const CMD_CTS: u8 = 0x09;
const CMD_DATA: u8 = 0x15;
const CMD_SKIP: u8 = 0x36;
const CMD_ACK: u8 = 0x56;
const CMD_ERR: u8 = 0x5a;
const CMD_RDY: u8 = 0x68;
const CMD_SCR: u8 = 0x6d;
const CMD_DEL: u8 = 0x88;
const CMD_EOT: u8 = 0x92;
const CMD_REQ: u8 = 0xa2;
const CMD_RTS: u8 = 0xc9;

struct Packet {
  mid: u8,
  cmd: u8,
  data: Vec<u8>,
}

pub struct Calc<T: BulkIo> {
  io: T,
  rx: ByteBuf,
  pub pc_id: u8,
  pub model: String,
  pub vars: Vec<Var>,
}

impl<T: BulkIo> Calc<T> {
  pub fn handshake(io: T) -> Result<Self> {
    let mut calc = Self {
      io,
      rx: ByteBuf::new(),
      pc_id: MID_PC,
      model: "TI-83 Plus".into(),
      vars: Vec::new(),
    };
    if calc.ready(MID_PC).is_ok() {
      calc.model = match calc.pc_id {
        MID_PC_82 => "TI-82".into(),
        MID_PC_83 => "TI-83".into(),
        _ => "TI-83 Plus / TI-84 Plus".into(),
      };
      return Ok(calc);
    }
    calc.rx = ByteBuf::new();
    if calc.ready(MID_PC_83).is_ok() {
      calc.pc_id = MID_PC_83;
      calc.model = "TI-83".into();
      return Ok(calc);
    }
    calc.rx = ByteBuf::new();
    calc.ready(MID_PC_82)?;
    calc.pc_id = MID_PC_82;
    calc.model = "TI-82".into();
    Ok(calc)
  }

  fn ready(&mut self, pc_id: u8) -> Result<u8> {
    self.send(pc_id, CMD_RDY, &[])?;
    let ack = self.recv()?;
    if ack.cmd != CMD_ACK {
      return Err(NlinkError::from("calculator did not acknowledge SilverLink"));
    }
    self.pc_id = match ack.mid {
      0x82 => MID_PC_82,
      0x83 => MID_PC_83,
      _ => MID_PC,
    };
    Ok(ack.mid)
  }

  pub fn reload(&mut self) -> Result<()> {
    let header = var_header(0, 0x19, &[0; 8], 0, 0);
    self.send(self.pc_id, CMD_REQ, &header)?;
    self.expect(CMD_ACK)?;
    let mut vars = Vec::new();
    loop {
      let pkt = self.recv()?;
      match pkt.cmd {
        CMD_VAR => {
          if let Some(var) = parse_header(&pkt.data) {
            vars.push(var);
          }
          self.send(self.pc_id, CMD_ACK, &[])?;
        }
        CMD_DATA => {
          vars.extend(parse_header_blob(&pkt.data));
          self.send(self.pc_id, CMD_ACK, &[])?;
        }
        CMD_EOT => {
          self.send(self.pc_id, CMD_ACK, &[])?;
          break;
        }
        CMD_SKIP | CMD_ERR => {
          return Err(NlinkError::from("calculator rejected the directory request"));
        }
        CMD_ACK => {}
        other => {
          return Err(NlinkError::from(format!(
            "unexpected SilverLink packet {other:#04x}"
          )))
        }
      }
    }
    self.vars = vars;
    Ok(())
  }

  pub fn get_var(&mut self, name: &str, type_id: u8) -> Result<Vec<u8>> {
    let header = var_header(0, type_id, &name_bytes(name), 0, 0);
    self.send(self.pc_id, CMD_REQ, &header)?;
    self.expect(CMD_ACK)?;
    let hdr = self.expect(CMD_VAR)?;
    let var = parse_header(&hdr.data).ok_or_else(|| NlinkError::from("bad variable header"))?;
    self.send(self.pc_id, CMD_ACK, &[])?;
    self.send(self.pc_id, CMD_CTS, &[])?;
    self.expect(CMD_ACK)?;
    let mut data = Vec::new();
    loop {
      let pkt = self.recv()?;
      match pkt.cmd {
        CMD_DATA => {
          data.extend_from_slice(&pkt.data);
          self.send(self.pc_id, CMD_ACK, &[])?;
        }
        CMD_EOT => {
          self.send(self.pc_id, CMD_ACK, &[])?;
          break;
        }
        CMD_SKIP | CMD_ERR => return Err(NlinkError::from("calculator skipped this variable")),
        other => {
          return Err(NlinkError::from(format!(
            "unexpected SilverLink packet {other:#04x}"
          )))
        }
      }
    }
    if data.len() as u64 > MAX_FILE_SIZE || var.size as u64 > MAX_FILE_SIZE {
      return Err(NlinkError::from("variable exceeds the safety limit"));
    }
    Ok(data)
  }

  pub fn put_var(&mut self, name: &str, type_id: u8, archived: bool, data: &[u8]) -> Result<()> {
    if type_id == 0x23 {
      return Err(NlinkError::from("sending an operating system is not supported"));
    }
    let size = u16::try_from(data.len()).map_err(|_| NlinkError::from("variable is too large"))?;
    let header = var_header(size, type_id, &name_bytes(name), 0, if archived { 0x80 } else { 0 });
    crate::progress::reset(data.len() as u64);
    self.send(self.pc_id, CMD_RTS, &header)?;
    self.expect(CMD_ACK)?;
    let next = self.recv()?;
    if next.cmd == CMD_SKIP {
      return Err(NlinkError::from("calculator skipped this variable"));
    }
    if next.cmd != CMD_CTS {
      return Err(NlinkError::from("calculator did not accept the variable"));
    }
    self.send(self.pc_id, CMD_ACK, &[])?;
    self.send(self.pc_id, CMD_DATA, data)?;
    self.expect(CMD_ACK)?;
    self.send(self.pc_id, CMD_EOT, &[])?;
    self.expect(CMD_ACK)?;
    crate::progress::finish();
    Ok(())
  }

  pub fn delete_var(&mut self, name: &str, type_id: u8) -> Result<()> {
    let header = var_header(0, type_id, &name_bytes(name), 0, 0);
    self.send(self.pc_id, CMD_DEL, &header)?;
    self.expect(CMD_ACK)?;
    Ok(())
  }

  pub fn recv_backup(&mut self) -> Result<Vec<u8>> {
    let request = var_header(0, 0x13, &[0; 8], 0, 0);
    self.send(self.pc_id, CMD_REQ, &request)?;
    self.expect(CMD_ACK)?;
    let header = self.expect(CMD_VAR)?;
    if header.data.len() < 9 || header.data[2] != 0x13 {
      return Err(NlinkError::from("calculator did not return a RAM backup header"));
    }
    let size1 = u16::from_le_bytes([header.data[0], header.data[1]]);
    let size2 = u16::from_le_bytes([header.data[3], header.data[4]]);
    let size3 = u16::from_le_bytes([header.data[5], header.data[6]]);
    let address = u16::from_le_bytes([header.data[7], header.data[8]]);
    let total = size1 as u64 + size2 as u64 + size3 as u64;
    crate::progress::reset(total);
    self.send(self.pc_id, CMD_ACK, &[])?;
    self.send(self.pc_id, CMD_CTS, &[])?;
    self.expect(CMD_ACK)?;
    let section1 = self.expect_section(size1)?;
    crate::progress::set_remaining(total - section1.len() as u64);
    let section2 = self.expect_section(size2)?;
    crate::progress::set_remaining(total - section1.len() as u64 - section2.len() as u64);
    let section3 = self.expect_section(size3)?;
    crate::progress::finish();
    let signature = if self.model.starts_with("TI-82") {
      b"**TI82**"
    } else if self.model == "TI-83" {
      b"**TI83**"
    } else {
      b"**TI83F*"
    };
    Ok(write_backup_file(signature, address, &section1, &section2, &section3))
  }

  fn expect_section(&mut self, expected: u16) -> Result<Vec<u8>> {
    let pkt = self.expect(CMD_DATA)?;
    if pkt.data.len() != expected as usize {
      return Err(NlinkError::from(format!(
        "RAM backup section was {} bytes, expected {expected}",
        pkt.data.len()
      )));
    }
    self.send(self.pc_id, CMD_ACK, &[])?;
    Ok(pkt.data)
  }

  pub fn screenshot(&mut self) -> Result<(u16, u16, Vec<u8>)> {
    self.send(self.pc_id, CMD_SCR, &[])?;
    self.expect(CMD_ACK)?;
    let mut data = Vec::new();
    loop {
      let pkt = self.recv()?;
      match pkt.cmd {
        CMD_DATA => {
          data.extend_from_slice(&pkt.data);
          self.send(self.pc_id, CMD_ACK, &[])?;
        }
        CMD_EOT => {
          self.send(self.pc_id, CMD_ACK, &[])?;
          break;
        }
        _ => return Err(NlinkError::from("calculator did not return a screen")),
      }
    }
    if data.len() != 768 {
      return Err(NlinkError::from(format!(
        "unsupported SilverLink screen ({} bytes)",
        data.len()
      )));
    }
    Ok((96, 64, crate::link::dusb::decode_lcd(&data)?.2))
  }

  fn expect(&mut self, cmd: u8) -> Result<Packet> {
    let pkt = self.recv()?;
    if pkt.cmd == CMD_ERR {
      return Err(NlinkError::from("calculator reported a checksum error"));
    }
    if pkt.cmd != cmd {
      return Err(NlinkError::from(format!(
        "expected packet {cmd:#04x}, got {:#04x}",
        pkt.cmd
      )));
    }
    Ok(pkt)
  }

  fn send(&mut self, mid: u8, cmd: u8, data: &[u8]) -> Result<()> {
    self.io.write_all(&encode(mid, cmd, data))
  }

  fn recv(&mut self) -> Result<Packet> {
    self.rx.fill_from(&mut self.io, 4)?;
    let head = self.rx.take(4)?;
    let len = u16::from_le_bytes([head[2], head[3]]) as usize;
    let rest = if len == 0 { 0 } else { len + 2 };
    self.rx.fill_from(&mut self.io, rest)?;
    let tail = if rest == 0 {
      Vec::new()
    } else {
      self.rx.take(rest)?
    };
    if len > 0 {
      let sum = tail[..len]
        .iter()
        .fold(0u16, |s, b| s.wrapping_add(*b as u16));
      let got = u16::from_le_bytes([tail[len], tail[len + 1]]);
      if sum != got {
        return Err(NlinkError::from("SilverLink checksum mismatch"));
      }
    }
    Ok(Packet {
      mid: head[0],
      cmd: head[1],
      data: if len == 0 { Vec::new() } else { tail[..len].to_vec() },
    })
  }
}

pub fn write_backup_file(signature: &[u8; 8], address: u16, a: &[u8], b: &[u8], c: &[u8]) -> Vec<u8> {
  let mut body = Vec::new();
  body.extend_from_slice(&9u16.to_le_bytes());
  body.extend_from_slice(&(a.len() as u16).to_le_bytes());
  body.push(0x13);
  body.extend_from_slice(&(b.len() as u16).to_le_bytes());
  body.extend_from_slice(&(c.len() as u16).to_le_bytes());
  body.extend_from_slice(&address.to_le_bytes());
  body.extend_from_slice(&(a.len() as u16).to_le_bytes());
  body.extend_from_slice(a);
  body.extend_from_slice(&(b.len() as u16).to_le_bytes());
  body.extend_from_slice(b);
  body.extend_from_slice(&(c.len() as u16).to_le_bytes());
  body.extend_from_slice(c);
  let mut out = Vec::new();
  out.extend_from_slice(signature);
  out.extend_from_slice(&[0x1a, 0x0a, 0x00]);
  let mut comment = [0x20u8; 42];
  comment[..8].copy_from_slice(b"nlink-ng");
  out.extend_from_slice(&comment);
  out.extend_from_slice(&(body.len() as u16).to_le_bytes());
  let sum = body.iter().fold(0u16, |s, byte| s.wrapping_add(*byte as u16));
  out.extend_from_slice(&body);
  out.extend_from_slice(&sum.to_le_bytes());
  out
}

pub fn encode(mid: u8, cmd: u8, data: &[u8]) -> Vec<u8> {
  let mut out = vec![mid, cmd, (data.len() & 0xff) as u8, (data.len() >> 8) as u8];
  out.extend_from_slice(data);
  if !data.is_empty() {
    let sum = data.iter().fold(0u16, |s, b| s.wrapping_add(*b as u16));
    out.extend_from_slice(&sum.to_le_bytes());
  }
  out
}

fn var_header(size: u16, type_id: u8, name: &[u8; 8], version: u8, flag: u8) -> Vec<u8> {
  let mut data = Vec::with_capacity(13);
  data.extend_from_slice(&size.to_le_bytes());
  data.push(type_id);
  data.extend_from_slice(name);
  data.push(version);
  data.push(flag);
  data
}

fn name_bytes(name: &str) -> [u8; 8] {
  let mut out = [0u8; 8];
  let raw = name.as_bytes();
  let n = raw.len().min(8);
  out[..n].copy_from_slice(&raw[..n]);
  out
}

fn parse_header(data: &[u8]) -> Option<Var> {
  if data.len() < 11 {
    return None;
  }
  let size = u16::from_le_bytes([data[0], data[1]]) as u64;
  let type_id = data[2];
  if type_id == 0x19 {
    return None;
  }
  let name = std::str::from_utf8(&data[3..11]).ok()?.trim_matches('\0').to_string();
  let (version, archived) = if data.len() >= 13 {
    (u32::from(data[11]), data[12] & 0x80 != 0)
  } else {
    (0, false)
  };
  if name.is_empty() {
    return None;
  }
  Some(Var {
    name,
    type_id,
    size,
    archived,
    version,
  })
}

fn parse_header_blob(data: &[u8]) -> Vec<Var> {
  let mut out = Vec::new();
  let step = if data.len() % 13 == 0 { 13 } else { 11 };
  let mut off = 0;
  while off + step <= data.len() {
    if let Some(var) = parse_header(&data[off..off + step]) {
      out.push(var);
    }
    off += step;
  }
  out
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::link::transport::Pipe;

  #[test]
  fn ack_has_no_checksum_and_data_packet_does() {
    assert_eq!(encode(0x23, CMD_RDY, &[]), vec![0x23, 0x68, 0x00, 0x00]);
    let pkt = encode(0x73, CMD_DATA, &[0x01, 0x02]);
    assert_eq!(pkt, vec![0x73, 0x15, 0x02, 0x00, 0x01, 0x02, 0x03, 0x00]);
  }

  #[test]
  fn scripted_ready_names_the_83_plus() {
    let ack = encode(0x73, CMD_ACK, &[]);
    let calc = Calc::handshake(Pipe::with_incoming(ack)).unwrap();
    assert_eq!(calc.model, "TI-83 Plus / TI-84 Plus");
    assert_eq!(calc.io.written, encode(0x23, CMD_RDY, &[]));
  }

  #[test]
  fn scripted_backup_writes_three_sections() {
    let mut incoming = encode(0x73, CMD_ACK, &[]);
    let var = vec![0x02, 0x00, 0x13, 0x02, 0x00, 0x02, 0x00, 0x95, 0x9d];
    incoming.extend(encode(0x73, CMD_VAR, &var));
    incoming.extend(encode(0x73, CMD_ACK, &[]));
    incoming.extend(encode(0x73, CMD_DATA, &[0x11, 0x22]));
    incoming.extend(encode(0x73, CMD_DATA, &[0x33, 0x44]));
    incoming.extend(encode(0x73, CMD_DATA, &[0x55, 0x66]));
    let mut ready = encode(0x73, CMD_ACK, &[]);
    ready.extend(incoming);
    let mut calc = Calc::handshake(Pipe::with_incoming(ready)).unwrap();
    let file = calc.recv_backup().unwrap();
    assert_eq!(&file[..8], b"**TI83F*");
    assert!(file.windows(2).any(|w| w == [0x11, 0x22]));
    assert!(file.windows(2).any(|w| w == [0x55, 0x66]));
    let _ = var;
  }

  #[test]
  fn directory_blob_lists_a_program() {
    let mut header = var_header(4, 0x05, &name_bytes("HELLO"), 0, 0);
    assert!(parse_header(&header).unwrap().name == "HELLO");
    header[12] = 0x80;
    let var = parse_header(&header).unwrap();
    assert!(var.archived);
    assert_eq!(var.type_id, 0x05);
  }
}
