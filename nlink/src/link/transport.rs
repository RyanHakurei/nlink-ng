use crate::error::{NlinkError, Result};

/// One bulk IN/OUT pipe. Protocol code never talks to libusb or ioctls directly.
pub trait BulkIo {
  fn write_all(&mut self, data: &[u8]) -> Result<()>;
  fn read_some(&mut self, buf: &mut [u8]) -> Result<usize>;
}

/// In-memory pipe for protocol tests. `incoming` is what the calculator would send.
#[cfg(test)]
pub struct Pipe {
  pub written: Vec<u8>,
  pub incoming: Vec<u8>,
  pub read_pos: usize,
}

#[cfg(test)]
impl Pipe {
  pub fn with_incoming(incoming: Vec<u8>) -> Self {
    Self {
      written: Vec::new(),
      incoming,
      read_pos: 0,
    }
  }
}

#[cfg(test)]
impl BulkIo for Pipe {
  fn write_all(&mut self, data: &[u8]) -> Result<()> {
    self.written.extend_from_slice(data);
    Ok(())
  }

  fn read_some(&mut self, buf: &mut [u8]) -> Result<usize> {
    if self.read_pos >= self.incoming.len() {
      return Err(NlinkError::from("calculator closed the USB pipe"));
    }
    let n = (self.incoming.len() - self.read_pos).min(buf.len());
    buf[..n].copy_from_slice(&self.incoming[self.read_pos..self.read_pos + n]);
    self.read_pos += n;
    Ok(n)
  }
}

pub struct ByteBuf {
  data: Vec<u8>,
}

impl ByteBuf {
  pub fn new() -> Self {
    Self { data: Vec::new() }
  }

  pub fn fill_from(&mut self, io: &mut dyn BulkIo, need: usize) -> Result<()> {
    let mut tmp = [0u8; 4096];
    while self.data.len() < need {
      let n = io.read_some(&mut tmp)?;
      if n == 0 {
        return Err(NlinkError::from("empty USB read"));
      }
      self.data.extend_from_slice(&tmp[..n]);
    }
    Ok(())
  }

  pub fn take(&mut self, n: usize) -> Result<Vec<u8>> {
    if self.data.len() < n {
      return Err(NlinkError::from("short USB packet"));
    }
    Ok(self.data.drain(..n).collect())
  }
}

#[cfg(not(target_os = "android"))]
pub struct RusbIo {
  handle: rusb::DeviceHandle<rusb::GlobalContext>,
  iface: u8,
  ep_in: u8,
  ep_out: u8,
}

#[cfg(not(target_os = "android"))]
impl RusbIo {
  pub fn open(dev: rusb::Device<rusb::GlobalContext>, prefer_last: bool) -> Result<Self> {
    let config = dev
      .config_descriptor(0)
      .or_else(|_| dev.active_config_descriptor())?;
    let mut candidates = Vec::new();
    for iface in config.interfaces() {
      for desc in iface.descriptors() {
        let mut ep_in = None;
        let mut ep_out = None;
        for ep in desc.endpoint_descriptors() {
          if ep.transfer_type() != rusb::TransferType::Bulk {
            continue;
          }
          match ep.direction() {
            rusb::Direction::In => ep_in = Some(ep.address()),
            rusb::Direction::Out => ep_out = Some(ep.address()),
          }
        }
        if let (Some(ep_in), Some(ep_out)) = (ep_in, ep_out) {
          candidates.push((desc.interface_number(), ep_in, ep_out));
        }
      }
    }
    let (iface, ep_in, ep_out) = if prefer_last {
      candidates.pop()
    } else {
      candidates.drain(..).next()
    }
    .ok_or_else(|| NlinkError::from("calculator has no bulk endpoints"))?;
    let mut handle = dev.open()?;
    if handle.kernel_driver_active(iface).unwrap_or(false) {
      let _ = handle.detach_kernel_driver(iface);
    }
    handle.claim_interface(iface)?;
    Ok(Self {
      handle,
      iface,
      ep_in,
      ep_out,
    })
  }
}

#[cfg(not(target_os = "android"))]
impl Drop for RusbIo {
  fn drop(&mut self) {
    let _ = self.handle.release_interface(self.iface);
  }
}

#[cfg(not(target_os = "android"))]
impl BulkIo for RusbIo {
  fn write_all(&mut self, data: &[u8]) -> Result<()> {
    let mut off = 0;
    while off < data.len() {
      let n = self
        .handle
        .write_bulk(self.ep_out, &data[off..], std::time::Duration::from_secs(8))?;
      if n == 0 {
        return Err(NlinkError::from("empty USB write"));
      }
      off += n;
    }
    Ok(())
  }

  fn read_some(&mut self, buf: &mut [u8]) -> Result<usize> {
    match self
      .handle
      .read_bulk(self.ep_in, buf, std::time::Duration::from_secs(8))
    {
      Ok(n) => Ok(n),
      Err(rusb::Error::Timeout) => Err(NlinkError::timeout()),
      Err(e) => Err(e.into()),
    }
  }
}

#[cfg(target_os = "android")]
pub struct AndroidIo {
  ep_in: u8,
  ep_out: u8,
}

#[cfg(target_os = "android")]
impl AndroidIo {
  pub fn new(ep_in: u8, ep_out: u8) -> Self {
    Self { ep_in, ep_out }
  }
}

#[cfg(target_os = "android")]
impl BulkIo for AndroidIo {
  fn write_all(&mut self, data: &[u8]) -> Result<()> {
    let mut off = 0;
    while off < data.len() {
      let mut transferred = 0i32;
      let rc = unsafe {
        nlink_android_bulk(
          self.ep_out,
          data[off..].as_ptr() as *mut std::ffi::c_void,
          (data.len() - off) as i32,
          &mut transferred,
          8000,
        )
      };
      if rc != 0 {
        return Err(NlinkError::from(format!("USB write failed ({rc})")));
      }
      if transferred <= 0 {
        return Err(NlinkError::from("empty USB write"));
      }
      off += transferred as usize;
    }
    Ok(())
  }

  fn read_some(&mut self, buf: &mut [u8]) -> Result<usize> {
    let mut transferred = 0i32;
    let rc = unsafe {
      nlink_android_bulk(
        self.ep_in,
        buf.as_mut_ptr() as *mut std::ffi::c_void,
        buf.len() as i32,
        &mut transferred,
        8000,
      )
    };
    if rc != 0 {
      return Err(if rc == -1 {
        NlinkError::timeout()
      } else {
        NlinkError::from(format!("USB read failed ({rc})"))
      });
    }
    Ok(transferred.max(0) as usize)
  }
}

#[cfg(target_os = "android")]
unsafe extern "C" {
  fn nlink_android_bulk(
    ep: u8,
    ptr: *mut std::ffi::c_void,
    len: i32,
    transferred: *mut i32,
    timeout: u32,
  ) -> i32;
}
