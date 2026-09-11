use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use clap::{Parser, Subcommand};
use indicatif::{ProgressBar, ProgressStyle};
use libnspire::{dir::EntryType, PID, PID_CX2, VID};

#[derive(Parser, Debug)]
#[command(name = "n-link", author, about, version)]
struct Opt {
  #[command(subcommand)]
  cmd: Option<SubCommand>,
}

#[derive(Subcommand, Debug)]
enum SubCommand {
  Upload(Upload),
  Download(Download),
  UploadOS(UploadOS),
  Copy(Copy),
  Move(Move),
  Mkdir(Mkdir),
  Rmdir(Rmdir),
  Rm(Rm),
  Ls(Ls),
  /// View license information
  License,
}

/// Upload files to the calculator
#[derive(Parser, Debug)]
struct Upload {
  /// Files to upload
  #[arg(required = true)]
  files: Vec<PathBuf>,
  /// Destination path
  dest: String,
}

/// Download files or directories from the calculator.
/// Directories are downloaded recursively, preserving structure.
#[derive(Parser, Debug)]
struct Download {
  /// Files or directories to download
  #[arg(required = true)]
  files: Vec<String>,
  /// Destination directory on this computer
  dest: PathBuf,
}

/// Upload and install a .tcc/.tco/.tcc2/.tco2/.tct2 OS file
#[derive(Parser, Debug)]
struct UploadOS {
  /// Path to the OS file
  file: PathBuf,

  /// Disables the file extension check
  #[arg(long)]
  no_check_os: bool,
}

/// Copy a file to a different location
#[derive(Parser, Debug)]
struct Copy {
  /// Path to file
  from_path: String,

  /// Path to new location
  dist_path: String,
}

/// Move a file or directory to a new location
#[derive(Parser, Debug)]
struct Move {
  /// Path to file
  from_path: String,

  /// Path to new location
  dist_path: String,
}

/// Create a directory
#[derive(Parser, Debug)]
struct Mkdir {
  /// Path to directory
  path: String,
}

/// Delete a directory
#[derive(Parser, Debug)]
struct Rmdir {
  /// Path to directory
  path: String,
}

/// Delete files from the calculator
#[derive(Parser, Debug)]
struct Rm {
  /// Files to delete
  #[arg(required = true)]
  paths: Vec<String>,
}

/// List the contents of a directory
#[derive(Parser, Debug)]
struct Ls {
  /// Path to directory
  path: String,
}

fn get_dev() -> Option<libnspire::Handle<rusb::GlobalContext>> {
  let devices = match rusb::devices() {
    Ok(d) => d,
    Err(error) => {
      eprintln!("Failed to enumerate USB devices: {}", error);
      return None;
    }
  };
  devices.iter().find_map(|dev| {
    let descriptor = match dev.device_descriptor() {
      Ok(d) => d,
      Err(_) => return None,
    };
    if descriptor.vendor_id() == VID && matches!(descriptor.product_id(), PID | PID_CX2) {
      match dev.open() {
        Ok(handle) => match libnspire::Handle::new(handle) {
          Ok(handle) => Some(handle),
          Err(error) => {
            eprintln!("Failed to initialize calculator: {}", error);
            None
          }
        },
        Err(error) => {
          eprintln!("Failed to open calculator: {}", error);
          None
        }
      }
    } else {
      None
    }
  })
}

pub fn cwd() -> PathBuf {
  #[cfg(target_os = "linux")]
  if std::env::var_os("APPIMAGE").is_some() && std::env::var_os("APPDIR").is_some() {
    if let Some(cwd) = std::env::var_os("OWD") {
      return cwd.into();
    }
  };
  std::env::current_dir().expect("Couldn't get current directory")
}

fn bar_style() -> ProgressStyle {
  ProgressStyle::default_bar()
    .template("{spinner:.green} {msg} [{elapsed_precise}] [{bar:40.cyan/blue}] {bytes}/{total_bytes} ({bytes_per_sec}, {eta})")
    .expect("invalid progress template")
}

fn join_nspire_path(parent: &str, name: &str) -> String {
  let parent = parent.trim_end_matches('/');
  if parent.is_empty() {
    format!("/{}", name)
  } else {
    format!("{}/{}", parent, name)
  }
}

fn nspire_basename(path: &str) -> &str {
  path.trim_end_matches('/').rsplit('/').next().unwrap_or("")
}

fn download_file(
  handle: &libnspire::Handle<rusb::GlobalContext>,
  remote: &str,
  dest_file: &Path,
  size: u64,
) -> bool {
  if size > crate::error::MAX_FILE_SIZE {
    eprintln!(
      "Refusing to download {}: size {} exceeds safety limit {}",
      remote,
      size,
      crate::error::MAX_FILE_SIZE
    );
    return false;
  }
  if let Some(parent) = dest_file.parent() {
    if let Err(error) = fs::create_dir_all(parent) {
      eprintln!(
        "Failed to create {}: {}",
        parent.display(),
        error
      );
      return false;
    }
  }
  let mut dest = match File::create(dest_file) {
    Ok(file) => file,
    Err(error) => {
      eprintln!("Failed to open destination file {}: {}", dest_file.display(), error);
      return false;
    }
  };
  let mut buf = vec![0u8; size as usize];
  let bar = ProgressBar::new(buf.len() as u64);
  bar.set_style(bar_style());
  bar.set_message(format!("Download {}", remote));
  bar.enable_steady_tick(std::time::Duration::from_millis(100));
  let len = buf.len();
  match handle.read_file(remote, &mut buf, &mut |remaining| {
    bar.set_position((len - remaining) as u64);
  }) {
    Ok(_) => match dest.write_all(&buf) {
      Ok(_) => {
        bar.finish_with_message(format!("Download {}: Ok", remote));
        true
      }
      Err(error) => {
        bar.abandon_with_message(format!("Failed to write {}: {}", dest_file.display(), error));
        false
      }
    },
    Err(error) => {
      bar.abandon_with_message(format!("Failed to transfer {}: {}", remote, error));
      false
    }
  }
}

fn download_dir(
  handle: &libnspire::Handle<rusb::GlobalContext>,
  remote: &str,
  dest_dir: &Path,
) -> (u32, u32) {
  if let Err(error) = fs::create_dir_all(dest_dir) {
    eprintln!("Failed to create {}: {}", dest_dir.display(), error);
    return (0, 1);
  }
  match handle.list_dir(remote) {
    Ok(list) => {
      let mut ok = 0;
      let mut fail = 0;
      for item in list.iter() {
        let name = item.name().to_string_lossy();
        if name.is_empty() || name == "." || name == ".." {
          continue;
        }
        let child_remote = join_nspire_path(remote, name.as_ref());
        let child_local = dest_dir.join(name.as_ref());
        if item.entry_type() == EntryType::Directory {
          let (child_ok, child_fail) = download_dir(handle, &child_remote, &child_local);
          ok += child_ok;
          fail += child_fail;
        } else if download_file(handle, &child_remote, &child_local, item.size()) {
          ok += 1;
        } else {
          fail += 1;
        }
      }
      (ok, fail)
    }
    Err(error) => {
      eprintln!("Failed to list directory {}: {}", remote, error);
      (0, 1)
    }
  }
}

fn is_directory(
  handle: &libnspire::Handle<rusb::GlobalContext>,
  path: &str,
) -> Result<bool, libnspire::Error> {
  match handle.file_attr(path) {
    Ok(attr) => Ok(attr.entry_type() == EntryType::Directory),
    Err(attr_error) => match handle.list_dir(path) {
      Ok(_) => Ok(true),
      Err(_) => Err(attr_error),
    },
  }
}

pub fn run() -> bool {
  let opt = Opt::parse();
  if let Some(cmd) = opt.cmd {
    match cmd {
      SubCommand::Upload(Upload { files, mut dest }) => {
        if let Some(handle) = get_dev() {
          for file in files {
            let mut buf = vec![];
            if let Err(error) = File::open(cwd().join(&file)).and_then(|mut f| f.read_to_end(&mut buf))
            {
              eprintln!("Failed to read {}: {}", file.display(), error);
              continue;
            }
            let name = file
              .file_name()
              .expect("Failed to get file name")
              .to_string_lossy()
              .to_string();
            let bar = ProgressBar::new(buf.len() as u64);
            bar.set_style(bar_style());
            bar.set_message(format!("Upload {}", name));
            bar.enable_steady_tick(std::time::Duration::from_millis(100));
            if dest.ends_with('/') {
              dest.remove(dest.len() - 1);
            }
            let res = handle.write_file(&format!("{}/{}", dest, name), &buf, &mut |remaining| {
              bar.set_position((buf.len() - remaining) as u64)
            });

            match res {
              Ok(_) => {
                bar.finish_with_message(format!("Upload {}: Ok", dest));
              }
              Err(error) => {
                bar.abandon_with_message(format!("Failed: {}", error));
              }
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Download(Download { dest, files }) => {
        if let Some(handle) = get_dev() {
          let dest_root = cwd().join(&dest);
          if let Err(error) = fs::create_dir_all(&dest_root) {
            eprintln!("Failed to create {}: {}", dest_root.display(), error);
            return true;
          }
          for file in files {
            match is_directory(&handle, &file) {
              Ok(true) => {
                let local = if nspire_basename(&file).is_empty() {
                  dest_root.clone()
                } else {
                  dest_root.join(nspire_basename(&file))
                };
                let (ok, fail) = download_dir(&handle, &file, &local);
                println!(
                  "Downloaded folder {} => {}: {} file(s) ok, {} failed",
                  file,
                  local.display(),
                  ok,
                  fail
                );
              }
              Ok(false) => {
                let name = nspire_basename(&file);
                let dest_path = if name.is_empty() {
                  dest_root.join("download")
                } else {
                  dest_root.join(name)
                };
                let size = match handle.file_attr(&file) {
                  Ok(attr) => attr.size(),
                  Err(error) => {
                    eprintln!("Failed to read file info for {}: {}", file, error);
                    continue;
                  }
                };
                download_file(&handle, &file, &dest_path, size);
              }
              Err(error) => {
                eprintln!("Failed to read file info for {}: {}", file, error);
              }
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::UploadOS(UploadOS { file, no_check_os }) => {
        if let Some(handle) = get_dev() {
          let calc_info = match handle.info() {
            Ok(info) => info,
            Err(error) => {
              eprintln!("Failed to obtain device info: {}", error);
              return true;
            }
          };

          let file_ext = file
            .extension()
            .unwrap_or(OsStr::new(""))
            .to_string_lossy()
            .to_string();

          let mut buf = vec![];
          let mut f = match File::open(cwd().join(&file)) {
            Ok(f) => f,
            Err(err) => {
              eprintln!("Failed to open file: {}", err);
              std::process::exit(1);
            }
          };

          if format!(".{}", file_ext) != calc_info.os_extension {
            if no_check_os {
              eprintln!(
                "Warning: {} expects file of type {}",
                calc_info.name, calc_info.os_extension
              );
            } else {
              eprintln!(
                "Error: {} expects file of type {}",
                calc_info.name, calc_info.os_extension
              );
              eprintln!("Provide --no-check-os to bypass this check.");
              std::process::exit(1);
            }
          }

          if let Err(error) = f.read_to_end(&mut buf) {
            eprintln!("Failed to read OS file: {}", error);
            std::process::exit(1);
          }

          let name = file
            .file_name()
            .expect("Failed to get file name")
            .to_string_lossy()
            .to_string();

          let bar = ProgressBar::new(buf.len() as u64);
          bar.set_style(bar_style());
          bar.set_message(format!("Upload OS {}", name));
          bar.enable_steady_tick(std::time::Duration::from_millis(100));

          let res = handle.send_os(&buf, &mut |remaining| {
            bar.set_position((buf.len() - remaining) as u64);
          });

          match res {
            Ok(_) => {
              bar.finish();
            }
            Err(error) => {
              bar.abandon_with_message(format!("OS Upload failed: {}", error));
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Copy(Copy {
        from_path,
        dist_path,
      }) => {
        if let Some(handle) = get_dev() {
          match handle.copy_file(&from_path, &dist_path) {
            Ok(_) => {
              println!("Copy {} => {}: Ok", from_path, dist_path);
            }
            Err(error) => {
              eprintln!("Failed to copy file or directory: {}", error);
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Move(Move {
        from_path,
        dist_path,
      }) => {
        if let Some(handle) = get_dev() {
          match handle.move_file(&from_path, &dist_path) {
            Ok(_) => {
              println!("Move {} => {}: Ok", from_path, dist_path);
            }
            Err(error) => {
              eprintln!("Failed to move file or directory: {}", error);
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Mkdir(Mkdir { path }) => {
        if let Some(handle) = get_dev() {
          match handle.create_dir(&path) {
            Ok(_) => {
              println!("Create {}: Ok", path);
            }
            Err(error) => {
              eprintln!("Failed to create directory: {}", error);
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Rmdir(Rmdir { path }) => {
        if let Some(handle) = get_dev() {
          match handle.delete_dir(&path) {
            Ok(_) => {
              println!("Remove {}: Ok", path);
            }
            Err(error) => {
              eprintln!("Failed to delete directory: {}", error);
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Rm(Rm { paths }) => {
        if let Some(handle) = get_dev() {
          for path in paths {
            match handle.delete_file(&path) {
              Ok(_) => match handle.file_attr(&path) {
                Ok(attr) => {
                  eprintln!(
                    "Delete {} reported success, but it is still on the calculator (type={:?}, size={}). The OS may be protecting or recreating this file.",
                    path,
                    attr.entry_type(),
                    attr.size()
                  );
                }
                Err(_) => {
                  println!("Delete {}: Ok", path);
                }
              },
              Err(error) => {
                eprintln!("Failed to delete {}: {}", path, error);
              }
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::Ls(Ls { path }) => {
        if let Some(handle) = get_dev() {
          match handle.list_dir(&path) {
            Ok(dir_list) => {
              for item in dir_list.iter() {
                println!(
                  "{:>10} {}{}",
                  item.size(),
                  item.name().to_string_lossy(),
                  if item.entry_type() == EntryType::Directory {
                    "/"
                  } else {
                    ""
                  }
                );
              }
            }
            Err(error) => {
              eprintln!("Failed to list directory: {}", error);
              eprintln!("{}", crate::error::TIMEOUT_MESSAGE);
            }
          }
        } else {
          eprintln!("Couldn't find any device");
        }
      }
      SubCommand::License => {
        println!("{}", include_str!("../../LICENSE"));
        println!(include_str!("NOTICE.txt"), env!("CARGO_PKG_REPOSITORY"));
      }
    }
    true
  } else {
    false
  }
}
