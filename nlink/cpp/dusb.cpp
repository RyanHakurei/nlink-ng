// DUSB follows Benjamin Moody's public notes and does not include libticalcs source.

#include "dusb.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace nlink {

std::string make_calc_info(const std::string &family, const std::string &name, uint64_t free_ram,
                           uint64_t total_ram, uint64_t free_storage, uint64_t total_storage, uint64_t major,
                           uint64_t minor, uint64_t patch, const std::string *clock, const std::string *battery,
                           const char *ram_note);

namespace {

constexpr uint8_t RAW_BUF_REQ = 1;
constexpr uint8_t RAW_BUF_ALLOC = 2;
constexpr uint8_t RAW_VIRT = 3;
constexpr uint8_t RAW_VIRT_LAST = 4;
constexpr uint8_t RAW_ACK = 5;

constexpr uint16_t V_PING = 0x0001;
constexpr uint16_t V_PARM_REQ = 0x0007;
constexpr uint16_t V_PARM_DATA = 0x0008;
constexpr uint16_t V_DIR_REQ = 0x0009;
constexpr uint16_t V_VAR_HDR = 0x000a;
constexpr uint16_t V_RTS = 0x000b;
constexpr uint16_t V_VAR_REQ = 0x000c;
constexpr uint16_t V_VAR_DATA = 0x000d;
constexpr uint16_t V_DEL = 0x0010;
constexpr uint16_t V_MODE_ACK = 0x0012;
constexpr uint16_t V_DATA_ACK = 0xaa00;
constexpr uint16_t V_DELAY = 0xbb00;
constexpr uint16_t V_EOT = 0xdd00;
constexpr uint16_t V_ERROR = 0xee00;

constexpr uint8_t MODE_NORMAL[10] = {0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 0xd0};

std::string hex_u16(uint16_t value) {
  static const char *hex = "0123456789abcdef";
  std::string out = "0x";
  out.push_back(hex[(value >> 12) & 0x0f]);
  out.push_back(hex[(value >> 8) & 0x0f]);
  out.push_back(hex[(value >> 4) & 0x0f]);
  out.push_back(hex[value & 0x0f]);
  return out;
}

std::string ascii_lower(const std::string &s) {
  std::string out = s;
  for (char &ch : out) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 'A' && c <= 'Z') ch = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

void append_be16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_be32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

uint16_t read_be16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t be_uint(const uint8_t *data, size_t n) {
  uint64_t value = 0;
  size_t m = n < 8 ? n : 8;
  for (size_t i = 0; i < m; ++i) value = (value << 8) | data[i];
  return value;
}

std::string error_code(const std::vector<uint8_t> &data) {
  if (data.size() >= 2) return hex_u16(read_be16(data.data()));
  return "unknown";
}

std::string utf8_lossy(const uint8_t *data, size_t n) {
  std::string out;
  size_t i = 0;
  auto bad = [&] { out.append("\xEF\xBF\xBD"); };
  while (i < n) {
    unsigned char c = data[i];
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    size_t need = 0;
    uint32_t cp = 0;
    if ((c & 0xe0) == 0xc0) {
      need = 2;
      cp = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      need = 3;
      cp = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      need = 4;
      cp = c & 0x07;
    } else {
      bad();
      ++i;
      continue;
    }
    if (i + need > n) {
      bad();
      break;
    }
    bool ok = true;
    for (size_t j = 1; j < need; ++j) {
      if ((data[i + j] & 0xc0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (data[i + j] & 0x3f);
    }
    bool overlong = (need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000);
    if (!ok || overlong || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
      bad();
      ++i;
      continue;
    }
    out.append(reinterpret_cast<const char *>(data + i), need);
    i += need;
  }
  return out;
}

bool is_ascii_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string trim_model(std::string s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && s[b] == '\0') ++b;
  while (e > b && s[e - 1] == '\0') --e;
  while (b < e && is_ascii_ws(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && is_ascii_ws(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string io_message() {
  int err = errno;
  if (err == 0) return "failed to access file";
  return std::string(std::strerror(err)) + " (os error " + std::to_string(err) + ")";
}

std::vector<uint8_t> read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Error(io_message());
  in.seekg(0, std::ios::end);
  std::streamoff sz = in.tellg();
  if (sz < 0) throw Error(io_message());
  in.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (sz > 0) {
    in.read(reinterpret_cast<char *>(buf.data()), sz);
    if (!in) throw Error(io_message());
  }
  return buf;
}

void write_file(const std::string &path, const std::vector<uint8_t> &data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw Error(io_message());
  if (!data.empty()) {
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  }
  if (!out) throw Error(io_message());
}

void ensure_dir(const std::string &path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) throw Error(ec.message() + " (os error " + std::to_string(ec.value()) + ")");
}

void push_out_attrs(std::vector<uint8_t> &data, const std::vector<std::pair<uint16_t, std::vector<uint8_t>>> &attrs) {
  append_be16(data, static_cast<uint16_t>(attrs.size()));
  for (const auto &attr : attrs) {
    append_be16(data, attr.first);
    append_be16(data, static_cast<uint16_t>(attr.second.size()));
    data.insert(data.end(), attr.second.begin(), attr.second.end());
  }
}

std::vector<uint8_t> name_prefix(const std::string &name) {
  std::vector<uint8_t> data;
  append_be16(data, static_cast<uint16_t>(name.size()));
  data.insert(data.end(), name.begin(), name.end());
  data.push_back(0);
  return data;
}

std::vector<uint8_t> be_bytes_u32(uint32_t value) {
  std::vector<uint8_t> out;
  append_be32(out, value);
  return out;
}

std::vector<uint8_t> dir_request() {
  std::vector<uint8_t> data;
  append_be32(data, 3);
  append_be16(data, 0x0001);
  append_be16(data, 0x0002);
  append_be16(data, 0x0003);
  const uint8_t tail[] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01};
  data.insert(data.end(), tail, tail + sizeof tail);
  return data;
}

std::vector<uint8_t> rts(const std::string &name, uint32_t size, uint8_t type_id, bool archived, uint32_t version) {
  std::vector<uint8_t> data = name_prefix(name);
  append_be32(data, size);
  data.push_back(0x01);
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> attrs;
  attrs.push_back({0x0002, be_bytes_u32(0xf0070000u | static_cast<uint32_t>(type_id))});
  attrs.push_back({0x0003, std::vector<uint8_t>{static_cast<uint8_t>(archived ? 1 : 0)}});
  attrs.push_back({0x0008, be_bytes_u32(version)});
  push_out_attrs(data, attrs);
  return data;
}

std::vector<uint8_t> var_request(const std::string &name, uint8_t type_id) {
  std::vector<uint8_t> data = name_prefix(name);
  const uint8_t mid[] = {0x01, 0xff, 0xff, 0xff, 0xff};
  data.insert(data.end(), mid, mid + sizeof mid);
  append_be16(data, 3);
  append_be16(data, 0x0001);
  append_be16(data, 0x0002);
  append_be16(data, 0x0003);
  append_be16(data, 1);
  append_be16(data, 0x0011);
  append_be16(data, 4);
  append_be32(data, 0xf0070000u | static_cast<uint32_t>(type_id));
  data.push_back(0x00);
  data.push_back(0x00);
  return data;
}

std::vector<uint8_t> delete_request(const std::string &name, uint8_t type_id) {
  std::vector<uint8_t> data = name_prefix(name);
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> attrs;
  attrs.push_back({0x0002, be_bytes_u32(0xf0070000u | static_cast<uint32_t>(type_id))});
  push_out_attrs(data, attrs);
  const uint8_t tail[] = {0x01, 0x00, 0x00, 0x00, 0x00};
  data.insert(data.end(), tail, tail + sizeof tail);
  return data;
}

std::map<uint16_t, std::vector<uint8_t>> parse_parameters(const uint8_t *data, size_t n) {
  if (data == nullptr || n < 2) throw Error("parameter block is truncated");
  size_t count = read_be16(data);
  size_t pos = 2;
  std::map<uint16_t, std::vector<uint8_t>> out;
  for (size_t i = 0; i < count; ++i) {
    if (n < pos + 3) break;
    uint16_t id = read_be16(data + pos);
    uint8_t ok = data[pos + 2];
    pos += 3;
    if (ok != 0) continue;
    if (n < pos + 2) break;
    size_t vn = read_be16(data + pos);
    pos += 2;
    if (vn > n - pos) throw Error("parameter value is truncated");
    out[id] = std::vector<uint8_t>(data + pos, data + pos + vn);
    pos += vn;
  }
  return out;
}

std::vector<uint8_t> mono96(const uint8_t *data) {
  std::vector<uint8_t> rgba;
  rgba.reserve(96 * 64 * 4);
  for (int row = 0; row < 64; ++row) {
    for (int col = 0; col < 96; ++col) {
      uint8_t byte = data[row * 12 + col / 8];
      bool on = (byte & static_cast<uint8_t>(0x80 >> (col % 8))) != 0;
      uint8_t value = on ? 0 : 255;
      rgba.push_back(value);
      rgba.push_back(value);
      rgba.push_back(value);
      rgba.push_back(255);
    }
  }
  return rgba;
}

std::vector<uint8_t> rgb565(const uint8_t *data, size_t n) {
  std::vector<uint8_t> rgba;
  rgba.reserve((n / 2) * 4);
  for (size_t i = 0; i + 1 < n; i += 2) {
    uint16_t color = static_cast<uint16_t>(data[i] | (static_cast<uint16_t>(data[i + 1]) << 8));
    uint8_t r = static_cast<uint8_t>((color >> 11) & 0x1f);
    uint8_t g = static_cast<uint8_t>((color >> 5) & 0x3f);
    uint8_t b = static_cast<uint8_t>(color & 0x1f);
    rgba.push_back(static_cast<uint8_t>((r << 3) | (r >> 2)));
    rgba.push_back(static_cast<uint8_t>((g << 2) | (g >> 4)));
    rgba.push_back(static_cast<uint8_t>((b << 3) | (b >> 2)));
    rgba.push_back(255);
  }
  return rgba;
}

void civil_from_unix_days(int64_t unix_days, int32_t &year, uint32_t &month, uint32_t &day) {
  int64_t z = unix_days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint64_t doe = static_cast<uint64_t>(z - era * 146097);
  uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint64_t mp = (5 * doy + 2) / 153;
  day = static_cast<uint32_t>(doy - (153 * mp + 2) / 5 + 1);
  month = mp < 10 ? static_cast<uint32_t>(mp + 3) : static_cast<uint32_t>(mp - 9);
  year = static_cast<int32_t>(y + (month <= 2 ? 1 : 0));
}

const std::vector<uint8_t> *map_get(const std::map<uint16_t, std::vector<uint8_t>> &data, uint16_t id) {
  auto it = data.find(id);
  if (it == data.end()) return nullptr;
  return &it->second;
}

}  // namespace

bool can_rom_dump(const std::string &model) {
  std::string lower = ascii_lower(model);
  return lower.find("84") != std::string::npos && lower.find("ce") == std::string::npos &&
         lower.find("evo") == std::string::npos;
}

bool can_ram_backup(const std::string &model) { return ascii_lower(model).find("ce") == std::string::npos; }

std::string format_ti_clock(uint64_t seconds) {
  // Seconds since 1997-01-01, which is how the 84/CE clock parameter is stored.
  constexpr uint64_t DAYS_1970_TO_1997 = 9862;
  uint64_t unix_days = DAYS_1970_TO_1997 + seconds / 86400;
  uint64_t tod = seconds % 86400;
  int32_t year = 0;
  uint32_t month = 0;
  uint32_t day = 0;
  civil_from_unix_days(static_cast<int64_t>(unix_days), year, month, day);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02llu:%02llu:%02llu", static_cast<int>(year), month, day,
                static_cast<unsigned long long>(tod / 3600), static_cast<unsigned long long>((tod % 3600) / 60),
                static_cast<unsigned long long>(tod % 60));
  return buf;
}

size_t effective_buffer(size_t offered, const std::string &model) {
  size_t n = offered;
  if (n == 0 || n > 1024) n = 1024;
  if (model.find("CE") != std::string::npos || model.find("Premium") != std::string::npos) {
    if (n > 1018) n = 1018;
  }
  return n;
}

Image decode_lcd(const uint8_t *data, size_t n) {
  if (data == nullptr) n = 0;
  Image image;
  if (n == 768) {
    image.width = 96;
    image.height = 64;
    image.rgba = mono96(data);
    return image;
  }
  if (n == 153600) {
    image.width = 320;
    image.height = 240;
    image.rgba = rgb565(data, n);
    return image;
  }
  throw Error("unsupported screen size (" + std::to_string(n) + " bytes)");
}

Var parse_var_header(const uint8_t *data, size_t n) {
  if (data == nullptr) n = 0;
  if (n < 4) throw Error("variable header is truncated");
  size_t name_len = read_be16(data);
  if (name_len > n || n - name_len < 3) throw Error("variable header is truncated");
  std::string name = utf8_lossy(data + 2, name_len);
  size_t pos = 2 + name_len;
  if (pos < n && data[pos] == 0) ++pos;
  if (n < pos + 2) throw Error("variable header is truncated");
  size_t count = read_be16(data + pos);
  pos += 2;
  Var var;
  var.name = std::move(name);
  for (size_t i = 0; i < count; ++i) {
    if (n < pos + 3) break;
    uint16_t id = read_be16(data + pos);
    uint8_t ok = data[pos + 2];
    pos += 3;
    if (ok != 0) continue;
    if (n < pos + 2) break;
    size_t vn = read_be16(data + pos);
    pos += 2;
    if (vn > n - pos) throw Error("variable attribute is truncated");
    const uint8_t *value = data + pos;
    pos += vn;
    if (id == 0x0001) {
      var.size = be_uint(value, vn);
    } else if (id == 0x0002 || id == 0x0011) {
      var.type_id = vn == 0 ? 0 : value[vn - 1];
    } else if (id == 0x0003) {
      var.archived = vn != 0 && value[0] != 0;
    } else if (id == 0x0008) {
      var.version = static_cast<uint32_t>(be_uint(value, vn));
    }
  }
  return var;
}

DusbCalc::DusbCalc(std::unique_ptr<BulkIo> io) : io_(std::move(io)), max_raw_(250), model_("TI-84 Plus") {}

std::unique_ptr<DusbCalc> DusbCalc::handshake(std::unique_ptr<BulkIo> io) {
  std::unique_ptr<DusbCalc> calc(new DusbCalc(std::move(io)));
  calc->refresh_info();
  return calc;
}

BulkIo *DusbCalc::transport() const { return io_.get(); }

size_t DusbCalc::max_raw() const { return max_raw_; }

std::string DusbCalc::model() const { return model_; }

std::string DusbCalc::info_json() const {
  const char *note = nullptr;
  if (ascii_lower(model_).find("ce") != std::string::npos && free_ram_ == 0) {
    note = "CE reports no free RAM while the home screen is up";
  }
  const std::string *clock = clock_ ? &*clock_ : nullptr;
  const std::string *battery = battery_ ? &*battery_ : nullptr;
  return make_calc_info("dusb", model_, free_ram_, total_ram_, free_flash_, total_flash_, os_major_, os_minor_,
                        os_patch_, clock, battery, note);
}

std::vector<FileInfo> DusbCalc::list(const std::string &path) {
  if (path.empty() || path == "/" || vars_.empty()) reload();
  return list_vars(vars_, path);
}

void DusbCalc::download(const std::string &remote, const std::string &dest_dir) {
  Var var = find_var(vars_, remote);
  std::vector<uint8_t> data = get_var(var.name, var.type_id);
  ensure_dir(dest_dir);
  TiEntry entry;
  entry.var = var;
  entry.data = std::move(data);
  std::vector<uint8_t> bytes = write_8xp(entry);
  std::string filename = var.name + "." + file_ext(var.type_id);
  write_file((std::filesystem::path(dest_dir) / filename).string(), bytes);
}

void DusbCalc::upload(const std::string &dest_dir, const std::string &src) {
  std::vector<uint8_t> bytes = read_file(src);
  bool archived_dir = dest_archived(dest_dir);
  std::vector<TiEntry> entries = parse_ti(bytes.data(), bytes.size());
  for (const TiEntry &entry : entries) {
    put_var(entry.var.name, entry.var.type_id, entry.var.archived || archived_dir, entry.var.version, entry.data.data(),
            entry.data.size());
  }
  try {
    reload();
  } catch (const Error &) {
  }
}

void DusbCalc::remove(const std::string &remote) {
  Var var = find_var(vars_, remote);
  delete_var(var.name, var.type_id);
  try {
    reload();
  } catch (const Error &) {
  }
}

Image DusbCalc::screenshot() {
  begin_op();
  const uint16_t ids[] = {0x0022};
  std::map<uint16_t, std::vector<uint8_t>> data = parameters(ids, 1);
  const std::vector<uint8_t> *lcd = map_get(data, 0x0022);
  if (lcd == nullptr) throw Error("this calculator did not return a screen");
  return decode_lcd(lcd->data(), lcd->size());
}

void DusbCalc::backup(const std::string &dest) {
  if (!can_ram_backup(model_)) {
    throw Error("A full RAM backup is not available on the CE. Copy variables from the file list instead.");
  }
  if (vars_.empty()) reload();
  std::vector<Var> ram;
  for (const Var &var : vars_) {
    if (!var.archived && var.type_id != 0x24) ram.push_back(var);
  }
  if (ram.empty()) throw Error("there are no variables in RAM to back up");
  progress_reset(static_cast<uint64_t>(ram.size()));
  std::vector<TiEntry> entries;
  for (size_t index = 0; index < ram.size(); ++index) {
    TiEntry entry;
    entry.var = ram[index];
    entry.data = get_var(ram[index].name, ram[index].type_id);
    entries.push_back(std::move(entry));
    progress_set_remaining(static_cast<uint64_t>(ram.size() - index - 1));
  }
  std::vector<uint8_t> bytes = write_group(entries);
  write_file(dest, bytes);
  progress_finish();
}

void DusbCalc::refresh_info() {
  begin_op();
  const uint16_t ids[] = {0x0001, 0x0002, 0x000b, 0x000c, 0x000d, 0x000e,
                          0x000f, 0x0010, 0x0011, 0x0024, 0x0025, 0x002d};
  std::map<uint16_t, std::vector<uint8_t>> data = parameters(ids, sizeof ids / sizeof ids[0]);
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x0001)) {
    product_ = static_cast<uint32_t>(be_uint(bytes->data(), bytes->size()));
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x0002)) {
    std::string raw;
    bool ok = true;
    size_t i = 0;
    while (i < bytes->size()) {
      unsigned char c = (*bytes)[i];
      if (c < 0x80) {
        raw.push_back(static_cast<char>(c));
        ++i;
        continue;
      }
      size_t need = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
      if (need == 0 || i + need > bytes->size()) {
        ok = false;
        break;
      }
      bool cont = true;
      for (size_t j = 1; j < need; ++j) {
        if (((*bytes)[i + j] & 0xc0) != 0x80) cont = false;
      }
      if (!cont) {
        ok = false;
        break;
      }
      raw.append(reinterpret_cast<const char *>(bytes->data() + i), need);
      i += need;
    }
    std::string name = ok ? trim_model(raw) : std::string();
    if (!name.empty()) model_ = std::move(name);
  }
  if (model_ == "TI-84 Plus") {
    switch (product_) {
      case 0x04:
        model_ = "TI-83 Plus";
        break;
      case 0x0a:
        model_ = "TI-84 Plus";
        break;
      case 0x0b:
        model_ = "TI-82 Advanced";
        break;
      case 0x0f:
        model_ = "TI-84 Plus C Silver Edition";
        break;
      case 0x13:
        model_ = "TI-84 Plus CE";
        break;
      case 0x15:
        model_ = "TI-82 Advanced Edition Python";
        break;
      default:
        break;
    }
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x000b)) {
    if (bytes->size() >= 4) {
      os_major_ = read_be16(bytes->data());
      os_minor_ = (*bytes)[2];
      os_patch_ = (*bytes)[3];
    }
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x000d)) {
    total_ram_ = be_uint(bytes->data(), bytes->size());
  } else if (const std::vector<uint8_t> *bytes = map_get(data, 0x000c)) {
    total_ram_ = be_uint(bytes->data(), bytes->size());
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x0010)) {
    total_flash_ = be_uint(bytes->data(), bytes->size());
  } else if (const std::vector<uint8_t> *bytes = map_get(data, 0x000f)) {
    total_flash_ = be_uint(bytes->data(), bytes->size());
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x000e)) {
    free_ram_ = be_uint(bytes->data(), bytes->size());
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x0011)) {
    free_flash_ = be_uint(bytes->data(), bytes->size());
  }
  bool clock_on = true;
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x0024)) {
    if (!bytes->empty()) clock_on = (*bytes)[0] != 0;
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x0025)) {
    clock_ = clock_on ? format_ti_clock(be_uint(bytes->data(), bytes->size())) : std::string("off");
  }
  if (const std::vector<uint8_t> *bytes = map_get(data, 0x002d)) {
    if (!bytes->empty()) battery_ = (*bytes)[0] == 0 ? std::string("low") : std::string("good");
  }
  max_raw_ = effective_buffer(max_raw_, model_);
}

void DusbCalc::reload() {
  begin_op();
  std::vector<uint8_t> request = dir_request();
  send_virt(V_DIR_REQ, request.data(), request.size());
  std::vector<Var> vars;
  for (;;) {
    Virt pkt = recv_virt();
    if (pkt.kind == V_VAR_HDR) {
      vars.push_back(parse_var_header(pkt.data.data(), pkt.data.size()));
    } else if (pkt.kind == V_EOT) {
      break;
    } else if (pkt.kind == V_ERROR) {
      throw Error("calculator rejected the directory (" + error_code(pkt.data) + ")");
    } else {
      throw Error("unexpected directory packet " + hex_u16(pkt.kind));
    }
  }
  vars_ = std::move(vars);
}

std::vector<uint8_t> DusbCalc::get_var(const std::string &name, uint8_t type_id) {
  begin_op();
  std::vector<uint8_t> request = var_request(name, type_id);
  send_virt(V_VAR_REQ, request.data(), request.size());
  Virt hdr = recv_virt();
  if (hdr.kind == V_ERROR) throw Error("calculator rejected the request (" + error_code(hdr.data) + ")");
  if (hdr.kind != V_VAR_HDR) throw Error("calculator did not return a variable header");
  Virt body = recv_virt();
  if (body.kind != V_VAR_DATA) throw Error("calculator did not return variable data");
  if (static_cast<uint64_t>(body.data.size()) > MAX_FILE_SIZE) throw Error("variable exceeds the safety limit");
  return body.data;
}

void DusbCalc::put_var(const std::string &name, uint8_t type_id, bool archived, uint32_t version, const uint8_t *data,
                       size_t n) {
  if (type_id == 0x23) throw Error("sending an operating system is not supported");
  if (static_cast<uint64_t>(n) > MAX_FILE_SIZE) throw Error("variable exceeds the safety limit");
  progress_reset(static_cast<uint64_t>(n));
  begin_op();
  std::vector<uint8_t> header = rts(name, static_cast<uint32_t>(n), type_id, archived, version);
  send_virt(V_RTS, header.data(), header.size());
  expect_virt(V_DATA_ACK);
  send_virt(V_VAR_DATA, data, n);
  progress_finish();
  expect_virt(V_DATA_ACK);
  send_virt(V_EOT, nullptr, 0);
}

void DusbCalc::delete_var(const std::string &name, uint8_t type_id) {
  begin_op();
  std::vector<uint8_t> request = delete_request(name, type_id);
  send_virt(V_DEL, request.data(), request.size());
  expect_virt(V_DATA_ACK);
}

void DusbCalc::begin_op() {
  const uint8_t size[4] = {0x00, 0x00, 0x04, 0x00};
  write_raw(RAW_BUF_REQ, size, 4);
  Raw alloc = read_raw();
  if (alloc.kind != RAW_BUF_ALLOC || alloc.data.size() < 4) {
    throw Error("calculator did not accept the USB buffer size");
  }
  size_t offered = read_be32(alloc.data.data());
  size_t buf = effective_buffer(offered, model_);
  if (buf < 16) buf = 16;
  max_raw_ = buf;
  send_virt(V_PING, MODE_NORMAL, sizeof MODE_NORMAL);
  Virt mode = recv_virt();
  if (mode.kind == V_ERROR) throw Error("calculator rejected the connection (" + error_code(mode.data) + ")");
  if (mode.kind != V_MODE_ACK) throw Error("calculator did not enter link mode");
}

std::map<uint16_t, std::vector<uint8_t>> DusbCalc::parameters(const uint16_t *ids, size_t count) {
  std::vector<uint8_t> data;
  append_be16(data, static_cast<uint16_t>(count));
  for (size_t i = 0; i < count; ++i) append_be16(data, ids[i]);
  send_virt(V_PARM_REQ, data.data(), data.size());
  Virt pkt = recv_virt_skip_delay();
  if (pkt.kind != V_PARM_DATA) throw Error("calculator did not return device info");
  return parse_parameters(pkt.data.data(), pkt.data.size());
}

void DusbCalc::send_virt(uint16_t kind, const uint8_t *data, size_t n) {
  std::vector<uint8_t> virt;
  append_be32(virt, static_cast<uint32_t>(n));
  append_be16(virt, kind);
  if (n != 0 && data != nullptr) virt.insert(virt.end(), data, data + n);
  size_t off = 0;
  while (off < virt.size()) {
    size_t chunk = virt.size() - off;
    if (chunk > max_raw_) chunk = max_raw_;
    bool last = off + chunk == virt.size();
    write_raw(last ? RAW_VIRT_LAST : RAW_VIRT, virt.data() + off, chunk);
    Raw ack = read_raw();
    if (ack.kind != RAW_ACK) throw Error("calculator did not acknowledge a USB packet");
    off += chunk;
  }
  if (n > 0 && kind == V_VAR_DATA) progress_set_remaining(0);
}

DusbCalc::Virt DusbCalc::expect_virt(uint16_t kind) {
  Virt pkt = recv_virt_skip_delay();
  if (pkt.kind == V_ERROR) throw Error("calculator error " + error_code(pkt.data));
  if (pkt.kind != kind) throw Error("unexpected calculator packet " + hex_u16(pkt.kind));
  return pkt;
}

DusbCalc::Virt DusbCalc::recv_virt_skip_delay() {
  for (;;) {
    Virt pkt = recv_virt();
    if (pkt.kind == V_DELAY) continue;
    return pkt;
  }
}

DusbCalc::Virt DusbCalc::recv_virt() {
  std::vector<uint8_t> buf;
  for (;;) {
    Raw raw = read_raw();
    if (raw.kind == RAW_VIRT || raw.kind == RAW_VIRT_LAST) {
      buf.insert(buf.end(), raw.data.begin(), raw.data.end());
      const uint8_t ack[2] = {0xe0, 0x00};
      write_raw(RAW_ACK, ack, 2);
      if (raw.kind == RAW_VIRT_LAST) break;
    } else if (raw.kind == RAW_ACK) {
      continue;
    } else {
      throw Error("unexpected raw USB packet " + std::to_string(raw.kind));
    }
  }
  if (buf.size() < 6) throw Error("truncated calculator packet");
  size_t size = read_be32(buf.data());
  uint16_t kind = read_be16(buf.data() + 4);
  if (size > buf.size() - 6) throw Error("truncated calculator packet");
  Virt virt;
  virt.kind = kind;
  virt.data.assign(buf.begin() + 6, buf.begin() + 6 + static_cast<std::ptrdiff_t>(size));
  return virt;
}

void DusbCalc::write_raw(uint8_t kind, const uint8_t *data, size_t n) {
  std::vector<uint8_t> pkt;
  append_be32(pkt, static_cast<uint32_t>(n));
  pkt.push_back(kind);
  if (n != 0 && data != nullptr) pkt.insert(pkt.end(), data, data + n);
  io_->write_all(pkt.data(), pkt.size());
}

DusbCalc::Raw DusbCalc::read_raw() {
  rx_.fill_from(*io_, 5);
  std::vector<uint8_t> head = rx_.take(5);
  size_t len = read_be32(head.data());
  if (len > 1024u * 1024u) throw Error("calculator packet is too large");
  rx_.fill_from(*io_, len);
  Raw raw;
  raw.kind = head[4];
  raw.data = rx_.take(len);
  return raw;
}

std::unique_ptr<LinkCalc> open_dusb(std::unique_ptr<BulkIo> io) {
  std::unique_ptr<DusbCalc> calc = DusbCalc::handshake(std::move(io));
  return calc;
}

}  // namespace nlink
