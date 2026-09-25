use crate::device::FileInfo;
use crate::error::{NlinkError, Result};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Var {
  pub name: String,
  pub type_id: u8,
  pub size: u64,
  pub archived: bool,
  pub version: u32,
}

pub fn type_name(type_id: u8) -> &'static str {
  match type_id {
    0x00 => "Real",
    0x01 => "List",
    0x02 => "Matrix",
    0x03 => "Equation",
    0x04 => "String",
    0x05 | 0x06 => "Program",
    0x07 => "Picture",
    0x08 => "GDB",
    0x0c => "Complex",
    0x0d => "Complex list",
    0x15 => "AppVar",
    0x24 => "Application",
    _ => "Other",
  }
}

pub fn file_ext(type_id: u8) -> &'static str {
  match type_id {
    0x00 => "8xn",
    0x01 | 0x0d => "8xl",
    0x02 => "8xm",
    0x03 => "8xy",
    0x04 => "8xs",
    0x05 | 0x06 => "8xp",
    0x07 => "8xi",
    0x08 => "8xd",
    0x15 => "8xv",
    0x24 => "8xk",
    _ => "8xg",
  }
}

fn safe_name(name: &str) -> String {
  let mut out = String::new();
  for ch in name.chars() {
    if ch == '/' || ch == '\\' || ch.is_control() {
      out.push('_');
    } else {
      out.push(ch);
    }
  }
  if out.is_empty() {
    out.push_str("VAR");
  }
  out
}

fn folder_of(var: &Var) -> &'static str {
  if var.type_id == 0x24 {
    "Apps"
  } else if var.archived {
    "Archive"
  } else {
    "RAM"
  }
}

pub fn list(vars: &[Var], path: &str) -> Result<Vec<FileInfo>> {
  let path = if path.is_empty() { "/" } else { path };
  let parts: Vec<&str> = path.split('/').filter(|s| !s.is_empty()).collect();
  let mut out = Vec::new();
  match parts.as_slice() {
    [] => {
      for name in ["RAM", "Archive", "Apps"] {
        let any = vars.iter().any(|v| folder_of(v) == name);
        if name == "Apps" && !any {
          continue;
        }
        out.push(dir(name));
      }
    }
    [folder] => {
      let mut seen = Vec::new();
      for var in vars.iter().filter(|v| folder_of(v) == *folder) {
        let label = if var.type_id == 0x24 {
          continue;
        } else if type_name(var.type_id) == "Other" {
          format!("Other-{:02X}", var.type_id)
        } else {
          type_name(var.type_id).to_string()
        };
        if !seen.contains(&label) {
          seen.push(label.clone());
          out.push(dir(&label));
        }
      }
      if *folder == "Apps" {
        for var in vars.iter().filter(|v| v.type_id == 0x24) {
          out.push(file(&safe_name(&var.name), var.size));
        }
      }
      out.sort_by(|a, b| a.path.to_lowercase().cmp(&b.path.to_lowercase()));
    }
    [folder, kind] => {
      for var in vars {
        if folder_of(var) != *folder {
          continue;
        }
        let label = if type_name(var.type_id) == "Other" {
          format!("Other-{:02X}", var.type_id)
        } else {
          type_name(var.type_id).to_string()
        };
        if label == *kind {
          out.push(file(&safe_name(&var.name), var.size));
        }
      }
      out.sort_by(|a, b| a.path.to_lowercase().cmp(&b.path.to_lowercase()));
    }
    _ => return Err(NlinkError::from("that folder does not exist")),
  }
  Ok(out)
}

fn dir(name: &str) -> FileInfo {
  FileInfo {
    path: name.to_string(),
    is_dir: true,
    date: 0,
    size: 0,
  }
}

fn file(name: &str, size: u64) -> FileInfo {
  FileInfo {
    path: name.to_string(),
    is_dir: false,
    date: 0,
    size,
  }
}

pub fn find<'a>(vars: &'a [Var], remote: &str) -> Result<&'a Var> {
  let parts: Vec<&str> = remote.split('/').filter(|s| !s.is_empty()).collect();
  let (folder, kind, name) = match parts.as_slice() {
    ["Apps", name] => ("Apps", None, *name),
    [folder, kind, name] => (*folder, Some(*kind), *name),
    _ => return Err(NlinkError::from("choose a variable, not a folder")),
  };
  vars
    .iter()
    .find(|var| {
      if safe_name(&var.name) != name || folder_of(var) != folder {
        return false;
      }
      if folder == "Apps" {
        return var.type_id == 0x24;
      }
      let label = if type_name(var.type_id) == "Other" {
        format!("Other-{:02X}", var.type_id)
      } else {
        type_name(var.type_id).to_string()
      };
      kind == Some(label.as_str())
    })
    .ok_or_else(|| NlinkError::from("variable not found"))
}

pub fn dest_archived(dest_dir: &str) -> bool {
  dest_dir.split('/').any(|p| p == "Archive")
}

#[cfg(test)]
mod tests {
  use super::*;

  fn sample() -> Vec<Var> {
    vec![
      Var {
        name: "HELLO".into(),
        type_id: 0x05,
        size: 12,
        archived: false,
        version: 0,
      },
      Var {
        name: "DATA".into(),
        type_id: 0x15,
        size: 4,
        archived: true,
        version: 0,
      },
    ]
  }

  #[test]
  fn virtual_tree_splits_ram_and_archive() {
    let vars = sample();
    let root = list(&vars, "/").unwrap();
    assert!(root.iter().any(|f| f.path == "RAM" && f.is_dir));
    assert!(root.iter().any(|f| f.path == "Archive" && f.is_dir));
    let ram = list(&vars, "/RAM").unwrap();
    assert!(ram.iter().any(|f| f.path == "Program"));
    let progs = list(&vars, "/RAM/Program").unwrap();
    assert_eq!(progs.len(), 1);
    assert_eq!(progs[0].path, "HELLO");
    assert_eq!(progs[0].size, 12);
    let found = find(&vars, "/Archive/AppVar/DATA").unwrap();
    assert!(found.archived);
  }
}
