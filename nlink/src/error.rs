use serde::Serialize;

pub const MAX_FILE_SIZE: u64 = 256 * 1024 * 1024;

pub const TIMEOUT_MESSAGE: &str = "Timed out waiting for the calculator. This can happen with a corrupted filesystem, a stuck USB transfer, or a very large file. Disconnect and reconnect the calculator if this persists.";

#[derive(Debug, Serialize)]
pub struct NlinkError(pub String);

impl NlinkError {
  pub fn timeout() -> Self {
    NlinkError(TIMEOUT_MESSAGE.to_string())
  }

  pub fn busy() -> Self {
    NlinkError(
      "The calculator is busy with another operation. Wait for it to finish, or disconnect and reconnect if it is stuck.".to_string(),
    )
  }
}

impl std::fmt::Display for NlinkError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    write!(f, "{}", self.0)
  }
}

impl std::error::Error for NlinkError {}

impl From<&str> for NlinkError {
  fn from(value: &str) -> Self {
    NlinkError(value.to_string())
  }
}

impl From<String> for NlinkError {
  fn from(value: String) -> Self {
    NlinkError(value)
  }
}

impl From<anyhow::Error> for NlinkError {
  fn from(value: anyhow::Error) -> Self {
    NlinkError(value.to_string())
  }
}

impl From<std::ffi::NulError> for NlinkError {
  fn from(_: std::ffi::NulError) -> Self {
    NlinkError("invalid path".to_string())
  }
}

impl From<std::io::Error> for NlinkError {
  fn from(value: std::io::Error) -> Self {
    NlinkError(value.to_string())
  }
}

#[cfg(not(target_os = "android"))]
impl From<rusb::Error> for NlinkError {
  fn from(value: rusb::Error) -> Self {
    NlinkError(value.to_string())
  }
}

#[cfg(not(target_os = "android"))]
impl From<libnspire::Error> for NlinkError {
  fn from(error: libnspire::Error) -> Self {
    let message = match error {
      libnspire::Error::Timeout => TIMEOUT_MESSAGE.to_string(),
      libnspire::Error::Busy => {
        "The calculator is busy. Close any open documents on the device and try again.".to_string()
      }
      libnspire::Error::NoDevice => "The calculator was disconnected.".to_string(),
      libnspire::Error::Access => {
        "Permission denied while accessing the calculator. On Linux, install the udev rules."
          .to_string()
      }
      libnspire::Error::InvalidPacket | libnspire::Error::Nack => {
        "The calculator sent an invalid response. The filesystem may be corrupted.".to_string()
      }
      libnspire::Error::DoesNotExist => "The path does not exist on the calculator.".to_string(),
      libnspire::Error::Exists => "A file or directory with that name already exists.".to_string(),
      libnspire::Error::OutOfMemory => {
        "The calculator ran out of memory while handling this request.".to_string()
      }
      other => other.to_string(),
    };
    NlinkError(message)
  }
}

pub type Result<T> = std::result::Result<T, NlinkError>;
