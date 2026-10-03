#include "paths.hpp"

#include <algorithm>
#include <cstring>

namespace nlink {
namespace {

const char *folder_of(const Var &var) {
  if (var.type_id == 0x24) return "Apps";
  if (var.archived) return "Archive";
  return "RAM";
}

bool is_control_cp(uint32_t cp) { return cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f); }

void append_utf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

// Returns false at end. Invalid UTF-8 is consumed one byte at a time.
bool next_cp(const std::string &s, size_t &i, uint32_t &cp) {
  if (i >= s.size()) return false;
  auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  unsigned char c = byte(i);
  size_t need = 1;
  uint32_t value = c;
  if (c < 0x80) {
    need = 1;
    value = c;
  } else if ((c & 0xe0) == 0xc0) {
    need = 2;
    value = c & 0x1f;
  } else if ((c & 0xf0) == 0xe0) {
    need = 3;
    value = c & 0x0f;
  } else if ((c & 0xf8) == 0xf0) {
    need = 4;
    value = c & 0x07;
  } else {
    cp = c;
    ++i;
    return true;
  }
  if (i + need > s.size()) {
    cp = c;
    ++i;
    return true;
  }
  for (size_t j = 1; j < need; ++j) {
    unsigned char cont = byte(i + j);
    if ((cont & 0xc0) != 0x80) {
      cp = c;
      ++i;
      return true;
    }
    value = (value << 6) | (cont & 0x3f);
  }
  cp = value;
  i += need;
  return true;
}

std::string safe_name(const std::string &name) {
  std::string out;
  size_t i = 0;
  uint32_t cp = 0;
  while (next_cp(name, i, cp)) {
    if (cp == '/' || cp == '\\' || is_control_cp(cp)) {
      out.push_back('_');
    } else {
      append_utf8(out, cp);
    }
  }
  if (out.empty()) out = "VAR";
  return out;
}

std::string other_label(uint8_t type_id) {
  static const char *hex = "0123456789ABCDEF";
  std::string label = "Other-";
  label.push_back(hex[type_id >> 4]);
  label.push_back(hex[type_id & 0x0f]);
  return label;
}

std::string type_label(uint8_t type_id) {
  const char *name = type_name(type_id);
  if (std::strcmp(name, "Other") == 0) return other_label(type_id);
  return name;
}

FileInfo make_dir(const std::string &name) {
  FileInfo info;
  info.path = name;
  info.is_dir = true;
  info.date = 0;
  info.size = 0;
  return info;
}

FileInfo make_file(const std::string &name, uint64_t size) {
  FileInfo info;
  info.path = name;
  info.is_dir = false;
  info.date = 0;
  info.size = size;
  return info;
}

std::string ascii_lower(const std::string &s) {
  std::string out = s;
  for (char &ch : out) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 'A' && c <= 'Z') ch = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

void sort_entries(std::vector<FileInfo> &out) {
  std::stable_sort(out.begin(), out.end(), [](const FileInfo &a, const FileInfo &b) {
    return ascii_lower(a.path) < ascii_lower(b.path);
  });
}

std::vector<std::string> split_path(const std::string &path) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t j = i;
    while (j < path.size() && path[j] != '/') ++j;
    parts.push_back(path.substr(i, j - i));
    i = j;
  }
  return parts;
}

}  // namespace

const char *type_name(uint8_t type_id) {
  switch (type_id) {
    case 0x00:
      return "Real";
    case 0x01:
      return "List";
    case 0x02:
      return "Matrix";
    case 0x03:
      return "Equation";
    case 0x04:
      return "String";
    case 0x05:
    case 0x06:
      return "Program";
    case 0x07:
      return "Picture";
    case 0x08:
      return "GDB";
    case 0x0c:
      return "Complex";
    case 0x0d:
      return "Complex list";
    case 0x15:
      return "AppVar";
    case 0x24:
      return "Application";
    default:
      return "Other";
  }
}

const char *file_ext(uint8_t type_id) {
  switch (type_id) {
    case 0x00:
      return "8xn";
    case 0x01:
    case 0x0d:
      return "8xl";
    case 0x02:
      return "8xm";
    case 0x03:
      return "8xy";
    case 0x04:
      return "8xs";
    case 0x05:
    case 0x06:
      return "8xp";
    case 0x07:
      return "8xi";
    case 0x08:
      return "8xd";
    case 0x15:
      return "8xv";
    case 0x24:
      return "8xk";
    default:
      return "8xg";
  }
}

std::vector<FileInfo> list_vars(const std::vector<Var> &vars, const std::string &path_in) {
  std::string path = path_in.empty() ? std::string("/") : path_in;
  std::vector<std::string> parts = split_path(path);
  std::vector<FileInfo> out;
  if (parts.empty()) {
    const char *names[] = {"RAM", "Archive", "Apps"};
    for (const char *name : names) {
      bool any = false;
      for (const Var &var : vars) {
        if (std::strcmp(folder_of(var), name) == 0) {
          any = true;
          break;
        }
      }
      if (std::strcmp(name, "Apps") == 0 && !any) continue;
      out.push_back(make_dir(name));
    }
    return out;
  }
  if (parts.size() == 1) {
    const std::string &folder = parts[0];
    std::vector<std::string> seen;
    for (const Var &var : vars) {
      if (folder != folder_of(var)) continue;
      if (var.type_id == 0x24) continue;
      std::string label = type_label(var.type_id);
      if (std::find(seen.begin(), seen.end(), label) == seen.end()) {
        seen.push_back(label);
        out.push_back(make_dir(label));
      }
    }
    if (folder == "Apps") {
      for (const Var &var : vars) {
        if (var.type_id == 0x24) out.push_back(make_file(safe_name(var.name), var.size));
      }
    }
    sort_entries(out);
    return out;
  }
  if (parts.size() == 2) {
    const std::string &folder = parts[0];
    const std::string &kind = parts[1];
    for (const Var &var : vars) {
      if (folder != folder_of(var)) continue;
      if (type_label(var.type_id) == kind) out.push_back(make_file(safe_name(var.name), var.size));
    }
    sort_entries(out);
    return out;
  }
  throw Error("that folder does not exist");
}

const Var &find_var(const std::vector<Var> &vars, const std::string &remote) {
  std::vector<std::string> parts = split_path(remote);
  std::string folder;
  std::string name;
  bool has_kind = false;
  std::string kind;
  if (parts.size() == 2 && parts[0] == "Apps") {
    folder = "Apps";
    name = parts[1];
  } else if (parts.size() == 3) {
    folder = parts[0];
    kind = parts[1];
    has_kind = true;
    name = parts[2];
  } else {
    throw Error("choose a variable, not a folder");
  }
  for (const Var &var : vars) {
    if (safe_name(var.name) != name || folder != folder_of(var)) continue;
    if (folder == "Apps") {
      if (var.type_id == 0x24) return var;
      continue;
    }
    if (has_kind && kind == type_label(var.type_id)) return var;
  }
  throw Error("variable not found");
}

bool dest_archived(const std::string &dest_dir) {
  size_t start = 0;
  for (;;) {
    size_t slash = dest_dir.find('/', start);
    size_t end = slash == std::string::npos ? dest_dir.size() : slash;
    if (end - start == 7 && dest_dir.compare(start, 7, "Archive") == 0) return true;
    if (slash == std::string::npos) break;
    start = slash + 1;
    if (start > dest_dir.size()) break;
  }
  return false;
}

}  // namespace nlink
