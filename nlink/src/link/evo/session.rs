use super::{directory, kermit, screen, ti_file};
use crate::error::{NlinkError, Result};
use crate::link::paths::Var;
use crate::link::transport::BulkIo;

pub struct Calc<T: BulkIo> {
  io: T,
  buffered: Vec<u8>,
  pub model: String,
  pub os_version: String,
  pub vars: Vec<Var>,
}

impl<T: BulkIo> Calc<T> {
  pub fn handshake(io: T) -> Result<Self> {
    let mut calc = Self {
      io,
      buffered: Vec::new(),
      model: "TI-84 Evo".into(),
      os_version: String::new(),
      vars: Vec::new(),
    };
    let payload = calc.request_uri("hh01/sys/attributes")?;
    apply_attributes(&mut calc.model, &mut calc.os_version, &payload);
    Ok(calc)
  }

  pub fn reload(&mut self) -> Result<()> {
    let payload = self.request_uri("hh01/inf/res?name=directory")?;
    let listing = directory::listing(&payload).map_err(NlinkError::from)?;
    let value: serde_json::Value =
      serde_json::from_str(&listing.files_json).map_err(|e| NlinkError::from(e.to_string()))?;
    let mut vars = Vec::new();
    if let Some(items) = value.as_array() {
      for item in items {
        let name = item.get("name").and_then(|v| v.as_str()).unwrap_or("");
        if name.is_empty() {
          continue;
        }
        let type_id = item.get("type").and_then(|v| v.as_i64()).unwrap_or(0) as u8;
        let size = item.get("size").and_then(|v| v.as_i64()).unwrap_or(0).max(0) as u64;
        // The Evo "mem" flag is true when the variable is in RAM.
        let archived = !item.get("memory").and_then(|v| v.as_bool()).unwrap_or(true);
        vars.push(Var {
          name: name.to_string(),
          type_id,
          size,
          archived,
          version: item.get("version").and_then(|v| v.as_i64()).unwrap_or(0).max(0) as u32,
        });
      }
    }
    self.vars = vars;
    Ok(())
  }

  pub fn get_var(&mut self, name: &str, type_id: u8) -> Result<Vec<u8>> {
    let wire = ti_file::evo_wire_name(name).map_err(NlinkError::from)?;
    self.request_uri(&format!("hh01/xfr/var?name={wire}&type={type_id}"))
  }

  pub fn put_file(&mut self, path: &std::path::Path) -> Result<()> {
    let variable = ti_file::read(path).map_err(NlinkError::from)?;
    let memory_target = u8::from(variable.archived);
    let destination = format!(
      "hh01/xfr/var?name={}&type={}&memtarget={memory_target}&policy=0",
      variable.wire_name, variable.kind
    );
    crate::progress::reset(variable.contents.len() as u64);
    self.send_file(&destination, &variable.contents)?;
    crate::progress::finish();
    Ok(())
  }

  pub fn delete_var(&mut self, name: &str, type_id: u8) -> Result<()> {
    let wire = ti_file::evo_wire_name(name).map_err(NlinkError::from)?;
    let destination = format!("hh01/del/var?name={wire}&type={type_id}");
    self.send_file(&destination, &[0])
  }

  pub fn screenshot(&mut self) -> Result<(u16, u16, Vec<u8>)> {
    let payload = match self.request_uri("hh01/sys/screen") {
      Ok(payload) => payload,
      Err(_) => self.request_uri("hh01/inf/res?name=screencapture")?,
    };
    screen::decode(&payload).map_err(NlinkError::from)
  }

  fn request_uri(&mut self, uri: &str) -> Result<Vec<u8>> {
    let command = format!("hh01/get/{uri}");
    let first = self.send_command(&command, true, true)?;
    self.receive(first)
  }

  fn send_command(&mut self, command: &str, send_break: bool, send_attributes: bool) -> Result<Option<kermit::Packet>> {
    self.write(&kermit::sender_init(0))?;
    self.expect_ack(0)?;
    self.write(&kermit::encode_packet(1, b'F', command.as_bytes()))?;
    self.expect_ack(1)?;
    let mut sequence = 2u8;
    if send_attributes {
      let attributes = kermit::file_attributes(1);
      self.write(&kermit::encode_packet(sequence, b'A', &attributes))?;
      self.expect_ack(sequence)?;
      sequence = sequence.wrapping_add(1) & 0x3f;
    }
    let body = kermit::encode_data_chunks(b"h", 90)
      .into_iter()
      .next()
      .ok_or_else(|| NlinkError::from("empty Evo request"))?;
    self.write(&kermit::encode_packet(sequence, b'D', &body))?;
    self.expect_ack(sequence)?;
    sequence = sequence.wrapping_add(1) & 0x3f;
    self.write(&kermit::encode_packet(sequence, b'Z', &[]))?;
    self.expect_ack(sequence)?;
    sequence = sequence.wrapping_add(1) & 0x3f;
    if send_break {
      self.write(&kermit::encode_packet(sequence, b'B', &[]))?;
      loop {
        let packet = self.read_packet()?;
        match packet.kind {
          b'Y' if packet.sequence == sequence => break,
          b'S' => return Ok(Some(packet)),
          b'N' => return Err(NlinkError::from("calculator rejected the Evo request")),
          b'E' => return Err(remote(&packet)),
          _ => {}
        }
      }
    }
    Ok(None)
  }

  fn send_file(&mut self, destination: &str, contents: &[u8]) -> Result<()> {
    self.write(&kermit::sender_init(0))?;
    self.expect_ack(0)?;
    self.write(&kermit::encode_packet(1, b'F', destination.as_bytes()))?;
    self.expect_ack(1)?;
    let attributes = kermit::file_attributes(contents.len());
    self.write(&kermit::encode_packet(2, b'A', &attributes))?;
    self.expect_ack(2)?;
    let mut sequence = 3u8;
    let chunks = kermit::encode_data_chunks(contents, 90);
    let total = chunks.len().max(1);
    for (index, chunk) in chunks.into_iter().enumerate() {
      self.write(&kermit::encode_packet(sequence, b'D', &chunk))?;
      self.expect_ack(sequence)?;
      let left = ((total - index - 1) as u64) * (contents.len() as u64 / total as u64);
      crate::progress::set_remaining(left);
      sequence = sequence.wrapping_add(1) & 0x3f;
    }
    self.write(&kermit::encode_packet(sequence, b'Z', &[]))?;
    self.expect_ack(sequence)?;
    sequence = sequence.wrapping_add(1) & 0x3f;
    self.write(&kermit::encode_packet(sequence, b'B', &[]))?;
    self.expect_ack(sequence)?;
    Ok(())
  }

  fn receive(&mut self, first: Option<kermit::Packet>) -> Result<Vec<u8>> {
    let mut contents = Vec::new();
    let mut saw_file = false;
    if let Some(packet) = first {
      self.handle(&packet, &mut contents, &mut saw_file)?;
      if packet.kind == b'B' {
        return Ok(contents);
      }
    }
    for _ in 0..10_000 {
      let packet = self.read_packet()?;
      self.handle(&packet, &mut contents, &mut saw_file)?;
      if packet.kind == b'B' {
        return Ok(contents);
      }
    }
    Err(NlinkError::from("Evo transfer did not finish"))
  }

  fn handle(&mut self, packet: &kermit::Packet, contents: &mut Vec<u8>, saw_file: &mut bool) -> Result<()> {
    match packet.kind {
      b'S' => self.ack(packet.sequence, true)?,
      b'F' => {
        *saw_file = true;
        self.ack(packet.sequence, false)?;
      }
      b'A' => self.ack(packet.sequence, false)?,
      b'D' => {
        contents.extend(kermit::decode_data(&packet.data).map_err(|e| NlinkError::from(e.to_string()))?);
        self.ack(packet.sequence, false)?;
      }
      b'Z' | b'B' => self.ack(packet.sequence, false)?,
      b'E' => return Err(remote(packet)),
      _ => self.ack(packet.sequence, false)?,
    }
    Ok(())
  }

  fn expect_ack(&mut self, sequence: u8) -> Result<()> {
    loop {
      let packet = self.read_packet()?;
      match packet.kind {
        b'Y' if packet.sequence == sequence => return Ok(()),
        b'N' => return Err(NlinkError::from("calculator rejected an Evo packet")),
        b'E' => return Err(remote(&packet)),
        _ => {}
      }
    }
  }

  fn ack(&mut self, sequence: u8, init: bool) -> Result<()> {
    self.write(&kermit::acknowledgement(sequence, init))
  }

  fn write(&mut self, packet: &[u8]) -> Result<()> {
    self.io.write_all(packet)
  }

  fn read_packet(&mut self) -> Result<kermit::Packet> {
    let mut tmp = [0u8; 4096];
    for _ in 0..64 {
      match kermit::parse_packet(&self.buffered) {
        Ok((packet, used)) => {
          self.buffered.drain(..used);
          return Ok(packet);
        }
        Err(kermit::Error::Incomplete) => {}
        Err(e) => return Err(NlinkError::from(e.to_string())),
      }
      let n = self.io.read_some(&mut tmp)?;
      if n == 0 {
        return Err(NlinkError::from("empty Evo USB read"));
      }
      self.buffered.extend_from_slice(&tmp[..n]);
    }
    Err(NlinkError::from("timed out waiting for an Evo packet"))
  }
}

fn remote(packet: &kermit::Packet) -> NlinkError {
  let message = kermit::decode_data(&packet.data).unwrap_or_else(|_| packet.data.clone());
  NlinkError::from(format!(
    "calculator reported: {}",
    String::from_utf8_lossy(&message)
  ))
}

fn apply_attributes(model: &mut String, os: &mut String, payload: &[u8]) {
  let Ok(root) = directory::decode(payload) else {
    return;
  };
  if let Some(text) = text_field(&root, "pkg-version") {
    *os = text;
  }
  if let Some(product) = text_field(&root, "product") {
    *model = match product.as_str() {
      "23" => "TI-84 Evo".into(),
      "25" => "TI-84 Evo-T".into(),
      "27" => "TI-83 Evo".into(),
      other => format!("TI-84 Evo ({other})"),
    };
  }
}

fn text_field(root: &directory::Value, key: &str) -> Option<String> {
  match directory::map_get(root, key)? {
    directory::Value::Text(text) => Some(text.clone()),
    directory::Value::Integer(n) => Some(n.to_string()),
    directory::Value::Bytes(bytes) => Some(String::from_utf8_lossy(bytes).into_owned()),
    _ => None,
  }
}
