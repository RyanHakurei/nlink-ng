#include "nlink_internal.hpp"

#include <string>

namespace nlink {

std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  static const char *hex = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0x0f]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string file_list_json(const std::vector<FileInfo> &files) {
  std::string out = "[";
  for (size_t i = 0; i < files.size(); ++i) {
    if (i != 0) out.push_back(',');
    const FileInfo &f = files[i];
    out += "{\"path\":\"";
    out += json_escape(f.path);
    out += "\",\"isDir\":";
    out += f.is_dir ? "true" : "false";
    out += ",\"date\":";
    out += std::to_string(f.date);
    out += ",\"size\":";
    out += std::to_string(f.size);
    out += '}';
  }
  out += ']';
  return out;
}

std::string make_calc_info(const std::string &family, const std::string &name, uint64_t free_ram,
                           uint64_t total_ram, uint64_t free_storage, uint64_t total_storage,
                           uint64_t major, uint64_t minor, uint64_t patch, const std::string *clock,
                           const std::string *battery, const char *ram_note) {
  std::string out = "{\"name\":\"";
  out += json_escape(name);
  out += "\",\"family\":\"";
  out += json_escape(family);
  out += "\",\"id\":\"\",\"free_storage\":";
  out += std::to_string(free_storage);
  out += ",\"total_storage\":";
  out += std::to_string(total_storage);
  out += ",\"free_ram\":";
  out += std::to_string(free_ram);
  out += ",\"total_ram\":";
  out += std::to_string(total_ram);
  out += ",\"is_cx_ii\":false,\"version\":{\"major\":";
  out += std::to_string(major);
  out += ",\"minor\":";
  out += std::to_string(minor);
  out += ",\"patch\":";
  out += std::to_string(patch);
  out += ",\"build\":0}";
  if (clock != nullptr) {
    out += ",\"clock\":\"";
    out += json_escape(*clock);
    out += '"';
  }
  if (battery != nullptr) {
    out += ",\"battery\":\"";
    out += json_escape(*battery);
    out += '"';
  }
  if (ram_note != nullptr) {
    out += ",\"ram_note\":\"";
    out += json_escape(ram_note);
    out += '"';
  }
  out += '}';
  return out;
}

}  // namespace nlink
