// Adapted from TI-84 Evo Tools. Copyright (c) 2026 Nomadtax. MIT License.

const WIDTH: usize = 320;
const HEIGHT: usize = 240;

pub fn decode(payload: &[u8]) -> Result<(u16, u16, Vec<u8>), String> {
  let data = extract_data_field(payload).unwrap_or(payload);
  if data.starts_with(b"\x89PNG\r\n\x1a\n") {
    return decode_png(data);
  }
  match data.len() {
    153_600 => Ok((WIDTH as u16, HEIGHT as u16, rgb565(data))),
    76_800 => Ok((WIDTH as u16, HEIGHT as u16, gray(data))),
    length => Err(format!("unsupported Evo screen ({length} bytes)")),
  }
}

fn rgb565(data: &[u8]) -> Vec<u8> {
  let mut out = Vec::with_capacity(WIDTH * HEIGHT * 4);
  for chunk in data.chunks_exact(2) {
    let value = u16::from_le_bytes([chunk[0], chunk[1]]);
    let r = (value >> 11) & 0x1f;
    let g = (value >> 5) & 0x3f;
    let b = value & 0x1f;
    out.extend_from_slice(&[
      ((r * 255) / 31) as u8,
      ((g * 255) / 63) as u8,
      ((b * 255) / 31) as u8,
      255,
    ]);
  }
  out
}

fn gray(data: &[u8]) -> Vec<u8> {
  let mut out = Vec::with_capacity(data.len() * 4);
  for value in data {
    out.extend_from_slice(&[*value, *value, *value, 255]);
  }
  out
}

fn decode_png(data: &[u8]) -> Result<(u16, u16, Vec<u8>), String> {
  let decoder = png::Decoder::new(std::io::Cursor::new(data));
  let mut reader = decoder.read_info().map_err(|e| e.to_string())?;
  let mut buf = vec![0; reader.output_buffer_size()];
  let info = reader.next_frame(&mut buf).map_err(|e| e.to_string())?;
  let bytes = &buf[..info.buffer_size()];
  let rgba = match info.color_type {
    png::ColorType::Rgba => bytes.to_vec(),
    png::ColorType::Rgb => bytes
      .chunks_exact(3)
      .flat_map(|p| [p[0], p[1], p[2], 255])
      .collect(),
    png::ColorType::Grayscale => bytes.iter().flat_map(|v| [*v, *v, *v, 255]).collect(),
    other => return Err(format!("unsupported Evo PNG color {other:?}")),
  };
  Ok((info.width as u16, info.height as u16, rgba))
}

fn extract_data_field(payload: &[u8]) -> Option<&[u8]> {
  for index in 0..payload.len().saturating_sub(5) {
    if payload.get(index) == Some(&0x64) && payload.get(index + 1..index + 5) == Some(b"data") {
      let value = index + 5;
      let (length, header) = cbor_byte_string_len(payload.get(value..)?)?;
      return payload.get(value + header..value + header + length);
    }
  }
  None
}

fn cbor_byte_string_len(input: &[u8]) -> Option<(usize, usize)> {
  let first = *input.first()?;
  if first >> 5 != 2 {
    return None;
  }
  match first & 0x1f {
    length @ 0..=23 => Some((length as usize, 1)),
    24 => Some((*input.get(1)? as usize, 2)),
    25 => Some((
      u16::from_be_bytes([*input.get(1)?, *input.get(2)?]) as usize,
      3,
    )),
    26 => Some((
      u32::from_be_bytes([
        *input.get(1)?,
        *input.get(2)?,
        *input.get(3)?,
        *input.get(4)?,
      ]) as usize,
      5,
    )),
    _ => None,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn extracts_cbor_data() {
    let payload = [0xa1, 0x64, b'd', b'a', b't', b'a', 0x43, 1, 2, 3];
    assert_eq!(extract_data_field(&payload), Some(&[1, 2, 3][..]));
  }
}
