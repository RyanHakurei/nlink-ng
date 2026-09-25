use crate::error::{NlinkError, Result};
use crate::link::paths::Var;

const SIG_83: &[u8] = b"**TI83**";
const SIG_8X: &[u8] = b"**TI83F*";

#[derive(Clone, Debug)]
pub struct Entry {
  pub var: Var,
  pub data: Vec<u8>,
}

pub fn parse(bytes: &[u8]) -> Result<Vec<Entry>> {
  if bytes.len() < 55 {
    return Err(NlinkError::from("not a TI-83/84 variable file"));
  }
  let sig = &bytes[..8];
  let plus = if sig == SIG_8X {
    true
  } else if sig == SIG_83 || sig == b"**TI82**" || sig == b"**TI73**" {
    false
  } else {
    return Err(NlinkError::from(
      "unsupported file. TI-84 family files use .8xp, .8xv, and other .8x names",
    ));
  };
  let data_len = u16::from_le_bytes([bytes[53], bytes[54]]) as usize;
  let body_end = 55 + data_len;
  if bytes.len() < body_end + 2 {
    return Err(NlinkError::from("TI variable file is truncated"));
  }
  let body = &bytes[55..body_end];
  let sum = u16::from_le_bytes([bytes[body_end], bytes[body_end + 1]]);
  let expect = checksum(body);
  if sum != expect {
    return Err(NlinkError::from("TI variable file checksum does not match"));
  }
  let mut entries = Vec::new();
  let mut off = 0;
  while off + 4 < body.len() {
    let marker = u16::from_le_bytes([body[off], body[off + 1]]);
    let ti83p = plus || marker == 0x0d;
    // 83+: 2 + size + type + name + version + flag + size. Older 8x omits version/flag.
    let header_len = if ti83p { 17 } else { 15 };
    if off + header_len > body.len() {
      break;
    }
    let size = u16::from_le_bytes([body[off + 2], body[off + 3]]) as usize;
    let type_id = body[off + 4];
    let name_raw = &body[off + 5..off + 13];
    let (version, archived) = if ti83p {
      (u32::from(body[off + 13]), body[off + 14] & 0x80 != 0)
    } else {
      (0, false)
    };
    let data_at = off + header_len;
    if data_at + size > body.len() {
      return Err(NlinkError::from("TI variable entry is truncated"));
    }
    let name = name_of(name_raw);
    entries.push(Entry {
      var: Var {
        name,
        type_id,
        size: size as u64,
        archived,
        version,
      },
      data: body[data_at..data_at + size].to_vec(),
    });
    off = data_at + size;
  }
  if entries.is_empty() {
    return Err(NlinkError::from("TI variable file has no entries"));
  }
  Ok(entries)
}

pub fn write_8xp(entry: &Entry) -> Result<Vec<u8>> {
  write_group(std::slice::from_ref(entry))
}

pub fn write_group(entries: &[Entry]) -> Result<Vec<u8>> {
  if entries.is_empty() {
    return Err(NlinkError::from("RAM backup is empty"));
  }
  let mut body = Vec::new();
  for entry in entries {
    if entry.var.type_id == 0x23 {
      return Err(NlinkError::from("sending an operating system is not supported"));
    }
    let size = u16::try_from(entry.data.len()).map_err(|_| NlinkError::from("variable is too large"))?;
    body.extend_from_slice(&0x0du16.to_le_bytes());
    body.extend_from_slice(&size.to_le_bytes());
    body.push(entry.var.type_id);
    let mut name = [0u8; 8];
    let raw = entry.var.name.as_bytes();
    let n = raw.len().min(8);
    name[..n].copy_from_slice(&raw[..n]);
    body.extend_from_slice(&name);
    body.push(entry.var.version as u8);
    body.push(if entry.var.archived { 0x80 } else { 0 });
    body.extend_from_slice(&size.to_le_bytes());
    body.extend_from_slice(&entry.data);
  }
  let mut out = Vec::new();
  out.extend_from_slice(SIG_8X);
  out.extend_from_slice(&[0x1a, 0x0a, 0x00]);
  let mut comment = [0u8; 42];
  let note = b"nlink-ng";
  comment[..note.len()].copy_from_slice(note);
  out.extend_from_slice(&comment);
  let len = u16::try_from(body.len()).map_err(|_| NlinkError::from("variable is too large"))?;
  out.extend_from_slice(&len.to_le_bytes());
  out.extend_from_slice(&body);
  out.extend_from_slice(&checksum(&body).to_le_bytes());
  Ok(out)
}

fn name_of(raw: &[u8]) -> String {
  let end = raw.iter().position(|b| *b == 0).unwrap_or(raw.len());
  String::from_utf8_lossy(&raw[..end]).into_owned()
}

fn checksum(data: &[u8]) -> u16 {
  data.iter().fold(0u16, |sum, b| sum.wrapping_add(*b as u16))
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn round_trip_program() {
    let entry = Entry {
      var: Var {
        name: "HELLO".into(),
        type_id: 0x05,
        size: 3,
        archived: true,
        version: 1,
      },
      data: b"abc".to_vec(),
    };
    let bytes = write_8xp(&entry).unwrap();
    let parsed = parse(&bytes).unwrap();
    assert_eq!(parsed.len(), 1);
    assert_eq!(parsed[0].var.name, "HELLO");
    assert_eq!(parsed[0].var.type_id, 0x05);
    assert!(parsed[0].var.archived);
    assert_eq!(parsed[0].var.version, 1);
    assert_eq!(parsed[0].data, b"abc");
  }

  #[test]
  fn rejects_bad_checksum() {
    let entry = Entry {
      var: Var {
        name: "A".into(),
        type_id: 0x00,
        size: 1,
        archived: false,
        version: 0,
      },
      data: vec![1],
    };
    let mut bytes = write_8xp(&entry).unwrap();
    let last = bytes.len() - 1;
    bytes[last] ^= 0xff;
    assert!(parse(&bytes).is_err());
  }
}
