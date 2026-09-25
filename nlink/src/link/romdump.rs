//! Host side of the TI-84 Plus USB ROM dumper command set
//! (ready / size / get-block / repeated-byte / exit).
//! The calculator must already be running that dumper. The CE and Evo have no equivalent.

use crate::error::{NlinkError, Result};
use crate::link::transport::{BulkIo, ByteBuf};

const READY: u16 = 0xaa55;
const OK: u16 = 0x0001;
const EXIT: u16 = 0x0002;
const SIZE: u16 = 0x0003;
const GETDATA: u16 = 0x0005;
const DATA: u16 = 0x0006;
const REPEAT: u16 = 0x0007;
const BLOCK: usize = 1024;

pub fn dump<T: BulkIo>(mut io: T) -> Result<Vec<u8>> {
  let mut rx = ByteBuf::new();
  send(&mut io, READY, &[])?;
  let (cmd, _) = recv(&mut io, &mut rx)?;
  if cmd != OK {
    return Err(NlinkError::from(
      "the calculator did not answer as a ROM dumper. On a TI-84 Plus or Silver Edition, run the USB ROM dumper first. The CE and Evo cannot dump a ROM this way.",
    ));
  }
  send(&mut io, SIZE, &[])?;
  let (cmd, size_bytes) = recv(&mut io, &mut rx)?;
  if cmd != SIZE || size_bytes.len() < 4 {
    return Err(NlinkError::from("ROM dumper did not report a size"));
  }
  let total = u32::from_le_bytes([size_bytes[0], size_bytes[1], size_bytes[2], size_bytes[3]]) as usize;
  if total == 0 || total > 8 * 1024 * 1024 {
    return Err(NlinkError::from(format!("ROM dumper reported an unexpected size ({total} bytes)")));
  }
  crate::progress::reset(total as u64);
  let mut rom = vec![0u8; total];
  let mut address = 0usize;
  while address < total {
    send(&mut io, GETDATA, &(address as u32).to_le_bytes())?;
    let (cmd, data) = recv(&mut io, &mut rx)?;
    let chunk = match cmd {
      DATA => data,
      REPEAT => {
        if data.len() < 3 {
          return Err(NlinkError::from("ROM dumper sent a short repeated block"));
        }
        let count = u16::from_le_bytes([data[0], data[1]]) as usize;
        vec![data[2]; count.max(1)]
      }
      _ => {
        return Err(NlinkError::from(format!(
          "ROM dumper sent unexpected command {cmd:#06x}"
        )))
      }
    };
    let n = chunk.len().min(total - address).min(BLOCK);
    rom[address..address + n].copy_from_slice(&chunk[..n]);
    address += BLOCK;
    crate::progress::set_remaining((total - address.min(total)) as u64);
  }
  crate::progress::finish();
  send(&mut io, EXIT, &[])?;
  let _ = recv(&mut io, &mut rx);
  Ok(rom)
}

fn send<T: BulkIo>(io: &mut T, cmd: u16, data: &[u8]) -> Result<()> {
  io.write_all(&packet(cmd, data))
}

fn recv<T: BulkIo>(io: &mut T, rx: &mut ByteBuf) -> Result<(u16, Vec<u8>)> {
  rx.fill_from(io, 4)?;
  let head = rx.take(4)?;
  let cmd = u16::from_le_bytes([head[0], head[1]]);
  let len = u16::from_le_bytes([head[2], head[3]]) as usize;
  if len > 64 * 1024 {
    return Err(NlinkError::from("ROM dumper packet is too large"));
  }
  rx.fill_from(io, len + 2)?;
  let tail = rx.take(len + 2)?;
  let mut sum = head.iter().fold(0u16, |s, b| s.wrapping_add(*b as u16));
  sum = tail[..len].iter().fold(sum, |s, b| s.wrapping_add(*b as u16));
  let got = u16::from_le_bytes([tail[len], tail[len + 1]]);
  if sum != got {
    return Err(NlinkError::from("ROM dumper checksum mismatch"));
  }
  Ok((cmd, tail[..len].to_vec()))
}

pub fn packet(cmd: u16, data: &[u8]) -> Vec<u8> {
  let mut out = Vec::with_capacity(6 + data.len());
  out.extend_from_slice(&cmd.to_le_bytes());
  out.extend_from_slice(&(data.len() as u16).to_le_bytes());
  out.extend_from_slice(data);
  let sum = out.iter().fold(0u16, |s, b| s.wrapping_add(*b as u16));
  out.extend_from_slice(&sum.to_le_bytes());
  out
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::link::transport::Pipe;

  #[test]
  fn scripted_dump_expands_repeated_blocks() {
    let mut incoming = packet(OK, &[]);
    incoming.extend(packet(SIZE, &2048u32.to_le_bytes()));
    incoming.extend(packet(DATA, &[0xab; 1024]));
    incoming.extend(packet(REPEAT, &{
      let mut body = 1024u16.to_le_bytes().to_vec();
      body.extend_from_slice(&[0x3c, 0x3c]);
      body
    }));
    incoming.extend(packet(EXIT, &[]));
    let rom = dump(Pipe::with_incoming(incoming)).unwrap();
    assert_eq!(rom.len(), 2048);
    assert!(rom[..1024].iter().all(|b| *b == 0xab));
    assert!(rom[1024..].iter().all(|b| *b == 0x3c));
  }
}
