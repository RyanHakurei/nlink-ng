use std::ffi::CStr;
use std::os::raw::c_int;
use std::ptr;

use libnspire_sys::{free, nspire_handle_t, nspire_strerror, nspire_view_frame};

use crate::error::{NlinkError, Result};

const HEADER_LEN: usize = 24;
const FORMAT_GRAY4: u16 = 1;
const FORMAT_RGB565: u16 = 2;

struct FrameBuffer(*mut u8);

impl Drop for FrameBuffer {
  fn drop(&mut self) {
    if !self.0.is_null() {
      unsafe { free(self.0.cast()) };
    }
  }
}

pub fn pull_frame(handle: *mut nspire_handle_t) -> Result<(u16, u16, Vec<u8>)> {
  let mut bytes: *mut u8 = ptr::null_mut();
  let mut len: u32 = 0;
  let rc = unsafe { nspire_view_frame(handle, &mut bytes, &mut len) };
  if rc != 0 {
    return Err(view_error(rc));
  }
  let owned = FrameBuffer(bytes);
  if owned.0.is_null() || len < HEADER_LEN as u32 {
    return Err(NlinkError::from(
      "The calculator closed the view stream without a frame. Start nlink-view on the TI-Nspire.",
    ));
  }
  let slice = unsafe { std::slice::from_raw_parts(owned.0, len as usize) };
  decode_frame(slice)
}

fn view_error(rc: c_int) -> NlinkError {
  let detail = unsafe {
    let text = nspire_strerror(rc);
    if text.is_null() {
      format!("libnspire error {rc}")
    } else {
      CStr::from_ptr(text).to_string_lossy().into_owned()
    }
  };
  NlinkError::from(format!(
    "{detail}. Live view needs nlink-view running on the calculator."
  ))
}

pub fn decode_frame(bytes: &[u8]) -> Result<(u16, u16, Vec<u8>)> {
  if bytes.len() < HEADER_LEN || &bytes[..8] != b"NLNKFRM1" {
    return Err(NlinkError::from(
      "The view stream did not start with a frame header.",
    ));
  }
  let width = u16::from_le_bytes([bytes[8], bytes[9]]);
  let height = u16::from_le_bytes([bytes[10], bytes[11]]);
  let format = u16::from_le_bytes([bytes[12], bytes[13]]);
  let length = u32::from_le_bytes([bytes[20], bytes[21], bytes[22], bytes[23]]) as usize;
  if bytes.len() != HEADER_LEN + length {
    return Err(NlinkError::from("The view frame length does not match its header."));
  }
  let pixels = (width as usize)
    .checked_mul(height as usize)
    .filter(|n| *n > 0 && *n <= 640 * 480)
    .ok_or_else(|| NlinkError::from("The view frame size is not usable."))?;
  let payload = &bytes[HEADER_LEN..];
  let rgba = match format {
    FORMAT_GRAY4 => gray4_to_rgba(payload, pixels)?,
    FORMAT_RGB565 => rgb565_to_rgba(payload, pixels)?,
    _ => {
      return Err(NlinkError::from(format!(
        "Unsupported view frame format {format}."
      )))
    }
  };
  Ok((width, height, rgba))
}

fn gray4_to_rgba(data: &[u8], pixels: usize) -> Result<Vec<u8>> {
  let need = pixels.div_ceil(2);
  if data.len() != need {
    return Err(NlinkError::from("The grayscale view frame is the wrong size."));
  }
  let mut out = Vec::with_capacity(pixels * 4);
  for i in 0..pixels {
    let byte = data[i / 2];
    let nibble = if i % 2 == 0 { byte >> 4 } else { byte & 0x0f };
    let value = nibble * 17;
    out.extend_from_slice(&[value, value, value, 255]);
  }
  Ok(out)
}

fn rgb565_to_rgba(data: &[u8], pixels: usize) -> Result<Vec<u8>> {
  if data.len() != pixels * 2 {
    return Err(NlinkError::from("The color view frame is the wrong size."));
  }
  let mut out = Vec::with_capacity(pixels * 4);
  for chunk in data.chunks_exact(2) {
    let color = u16::from_le_bytes([chunk[0], chunk[1]]);
    let red5 = (color >> 11) & 0x1f;
    let green6 = (color >> 5) & 0x3f;
    let blue5 = color & 0x1f;
    out.extend_from_slice(&[
      ((red5 << 3) | (red5 >> 2)) as u8,
      ((green6 << 2) | (green6 >> 4)) as u8,
      ((blue5 << 3) | (blue5 >> 2)) as u8,
      255,
    ]);
  }
  Ok(out)
}

#[cfg(test)]
mod tests {
  use super::*;

  fn header(width: u16, height: u16, format: u16, payload: &[u8]) -> Vec<u8> {
    let mut bytes = vec![0u8; 24 + payload.len()];
    bytes[..8].copy_from_slice(b"NLNKFRM1");
    bytes[8..10].copy_from_slice(&width.to_le_bytes());
    bytes[10..12].copy_from_slice(&height.to_le_bytes());
    bytes[12..14].copy_from_slice(&format.to_le_bytes());
    bytes[16..20].copy_from_slice(&1u32.to_le_bytes());
    bytes[20..24].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    bytes[24..].copy_from_slice(payload);
    bytes
  }

  #[test]
  fn decodes_gray4_high_nibble_first() {
    let (width, height, rgba) = decode_frame(&header(2, 1, FORMAT_GRAY4, &[0xF0])).unwrap();
    assert_eq!((width, height), (2, 1));
    assert_eq!(&rgba[..4], &[255, 255, 255, 255]);
    assert_eq!(&rgba[4..], &[0, 0, 0, 255]);
  }

  #[test]
  fn decodes_rgb565_red() {
    let (width, height, rgba) = decode_frame(&header(1, 1, FORMAT_RGB565, &[0x00, 0xF8])).unwrap();
    assert_eq!((width, height), (1, 1));
    assert_eq!(rgba, vec![255, 0, 0, 255]);
  }

  #[test]
  fn rejects_a_short_header() {
    assert!(decode_frame(b"NLNKFRM").is_err());
  }

  #[test]
  fn rejects_a_length_that_does_not_match() {
    let mut bytes = header(1, 1, FORMAT_RGB565, &[0x00, 0xF8]);
    bytes[20] = 1;
    assert!(decode_frame(&bytes).is_err());
  }
}
