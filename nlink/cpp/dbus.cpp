// SilverLink packet bytes follow the public TI-83+ link guide.

#include "dbus.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace nlink {

std::string make_calc_info(const std::string &family, const std::string &name, uint64_t free_ram,
                           uint64_t total_ram, uint64_t free_storage, uint64_t total_storage, uint64_t major,
                           uint64_t minor, uint64_t patch, const std::string *clock, const std::string *battery,
                           const char *ram_note);

namespace {

constexpr uint8_t MID_PC = 0x23;
constexpr uint8_t MID_PC_83 = 0x03;
constexpr uint8_t MID_PC_82 = 0x02;

constexpr uint8_t CMD_VAR = 0x06;
constexpr uint8_t CMD_CTS = 0x09;
constexpr uint8_t CMD_DATA = 0x15;
constexpr uint8_t CMD_SKIP = 0x36;
constexpr uint8_t CMD_ACK = 0x56;
constexpr uint8_t CMD_ERR = 0x5a;
constexpr uint8_t CMD_RDY = 0x68;
constexpr uint8_t CMD_SCR = 0x6d;
constexpr uint8_t CMD_DEL = 0x88;
constexpr uint8_t CMD_EOT = 0x92;
constexpr uint8_t CMD_REQ = 0xa2;
constexpr uint8_t CMD_RTS = 0xc9;

std::string hex_u8(uint8_t value) {
  static const char *hex = "0123456789abcdef";
  std::string out = "0x";
  out.push_back(hex[value >> 4]);
  out.push_back(hex[value & 0x0f]);
  return out;
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

void name_bytes(const std::string &name, uint8_t out[8]) {
  std::memset(out, 0, 8);
  size_t n = name.size() < 8 ? name.size() : 8;
  if (n != 0) std::memcpy(out, name.data(), n);
}

bool utf8_ok(const uint8_t *data, size_t n, std::string &out) {
  out.clear();
  size_t i = 0;
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
      return false;
    }
    if (i + need > n) return false;
    for (size_t j = 1; j < need; ++j) {
      if ((data[i + j] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (data[i + j] & 0x3f);
    }
    bool overlong = (need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000);
    if (overlong || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    out.append(reinterpret_cast<const char *>(data + i), need);
    i += need;
  }
  return true;
}

std::string trim_nuls(const std::string &raw) {
  size_t b = 0;
  size_t e = raw.size();
  while (b < e && raw[b] == '\0') ++b;
  while (e > b && raw[e - 1] == '\0') --e;
  return raw.substr(b, e - b);
}

std::vector<Var> parse_header_blob(const uint8_t *data, size_t n) {
  std::vector<Var> out;
  size_t step = (n % 13 == 0) ? 13 : 11;
  for (size_t off = 0; off + step <= n; off += step) {
    if (std::optional<Var> var = dbus_parse_header(data + off, step)) out.push_back(*var);
  }
  return out;
}

std::vector<uint8_t> write_backup_file(const uint8_t signature[8], uint16_t address, const std::vector<uint8_t> &a,
                                       const std::vector<uint8_t> &b, const std::vector<uint8_t> &c) {
  auto append_le16 = [](std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>(value >> 8));
  };
  std::vector<uint8_t> body;
  append_le16(body, 9);
  append_le16(body, static_cast<uint16_t>(a.size()));
  body.push_back(0x13);
  append_le16(body, static_cast<uint16_t>(b.size()));
  append_le16(body, static_cast<uint16_t>(c.size()));
  append_le16(body, address);
  append_le16(body, static_cast<uint16_t>(a.size()));
  body.insert(body.end(), a.begin(), a.end());
  append_le16(body, static_cast<uint16_t>(b.size()));
  body.insert(body.end(), b.begin(), b.end());
  append_le16(body, static_cast<uint16_t>(c.size()));
  body.insert(body.end(), c.begin(), c.end());

  std::vector<uint8_t> out;
  out.insert(out.end(), signature, signature + 8);
  out.push_back(0x1a);
  out.push_back(0x0a);
  out.push_back(0x00);
  uint8_t comment[42];
  std::memset(comment, 0x20, sizeof comment);
  std::memcpy(comment, "nlink-ng", 8);
  out.insert(out.end(), comment, comment + 42);
  append_le16(out, static_cast<uint16_t>(body.size()));
  uint16_t sum = 0;
  for (uint8_t byte : body) sum = static_cast<uint16_t>(sum + byte);
  out.insert(out.end(), body.begin(), body.end());
  append_le16(out, sum);
  return out;
}

}  // namespace

std::vector<uint8_t> dbus_encode(uint8_t mid, uint8_t cmd, const uint8_t *data, size_t n) {
  std::vector<uint8_t> out;
  out.push_back(mid);
  out.push_back(cmd);
  out.push_back(static_cast<uint8_t>(n & 0xff));
  out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
  if (n != 0 && data != nullptr) out.insert(out.end(), data, data + n);
  if (n != 0) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = static_cast<uint16_t>(sum + data[i]);
    out.push_back(static_cast<uint8_t>(sum & 0xff));
    out.push_back(static_cast<uint8_t>(sum >> 8));
  }
  return out;
}

std::vector<uint8_t> dbus_var_header(uint16_t size, uint8_t type_id, const uint8_t name[8], uint8_t version,
                                     uint8_t flag) {
  std::vector<uint8_t> data;
  data.push_back(static_cast<uint8_t>(size & 0xff));
  data.push_back(static_cast<uint8_t>(size >> 8));
  data.push_back(type_id);
  data.insert(data.end(), name, name + 8);
  data.push_back(version);
  data.push_back(flag);
  return data;
}

std::optional<Var> dbus_parse_header(const uint8_t *data, size_t n) {
  if (data == nullptr || n < 11) return std::nullopt;
  uint64_t size = static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
  uint8_t type_id = data[2];
  if (type_id == 0x19) return std::nullopt;
  std::string raw;
  if (!utf8_ok(data + 3, 8, raw)) return std::nullopt;
  std::string name = trim_nuls(raw);
  if (name.empty()) return std::nullopt;
  uint32_t version = 0;
  bool archived = false;
  if (n >= 13) {
    version = data[11];
    archived = (data[12] & 0x80) != 0;
  }
  Var var;
  var.name = std::move(name);
  var.type_id = type_id;
  var.size = size;
  var.archived = archived;
  var.version = version;
  return var;
}

DbusCalc::DbusCalc(std::unique_ptr<BulkIo> io) : io_(std::move(io)), pc_id_(MID_PC), model_("TI-83 Plus") {}

std::unique_ptr<DbusCalc> DbusCalc::handshake(std::unique_ptr<BulkIo> io) {
  std::unique_ptr<DbusCalc> calc(new DbusCalc(std::move(io)));
  auto try_ready = [&](uint8_t id) -> bool {
    try {
      calc->ready(id);
      return true;
    } catch (const Error &) {
      return false;
    }
  };
  if (try_ready(MID_PC)) {
    if (calc->pc_id_ == MID_PC_82) calc->model_ = "TI-82";
    else if (calc->pc_id_ == MID_PC_83) calc->model_ = "TI-83";
    else calc->model_ = "TI-83 Plus / TI-84 Plus";
    return calc;
  }
  calc->rx_.clear();
  if (try_ready(MID_PC_83)) {
    calc->pc_id_ = MID_PC_83;
    calc->model_ = "TI-83";
    return calc;
  }
  calc->rx_.clear();
  calc->ready(MID_PC_82);
  calc->pc_id_ = MID_PC_82;
  calc->model_ = "TI-82";
  return calc;
}

BulkIo *DbusCalc::transport() const { return io_.get(); }

std::string DbusCalc::model() const { return model_; }

std::string DbusCalc::info_json() const {
  return make_calc_info("silverlink", model_, 0, 0, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr);
}

std::vector<FileInfo> DbusCalc::list(const std::string &path) {
  if (path.empty() || path == "/" || vars_.empty()) reload();
  return list_vars(vars_, path);
}

void DbusCalc::download(const std::string &remote, const std::string &dest_dir) {
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

void DbusCalc::upload(const std::string &dest_dir, const std::string &src) {
  std::vector<uint8_t> bytes = read_file(src);
  bool archived_dir = dest_archived(dest_dir);
  std::vector<TiEntry> entries = parse_ti(bytes.data(), bytes.size());
  for (const TiEntry &entry : entries) {
    put_var(entry.var.name, entry.var.type_id, entry.var.archived || archived_dir, entry.data.data(),
            entry.data.size());
  }
  try {
    reload();
  } catch (const Error &) {
  }
}

void DbusCalc::remove(const std::string &remote) {
  Var var = find_var(vars_, remote);
  delete_var(var.name, var.type_id);
  try {
    reload();
  } catch (const Error &) {
  }
}

Image DbusCalc::screenshot() {
  send(pc_id_, CMD_SCR, nullptr, 0);
  expect(CMD_ACK);
  std::vector<uint8_t> data;
  for (;;) {
    Packet pkt = recv();
    if (pkt.cmd == CMD_DATA) {
      data.insert(data.end(), pkt.data.begin(), pkt.data.end());
      send(pc_id_, CMD_ACK, nullptr, 0);
    } else if (pkt.cmd == CMD_EOT) {
      send(pc_id_, CMD_ACK, nullptr, 0);
      break;
    } else {
      throw Error("calculator did not return a screen");
    }
  }
  if (data.size() != 768) {
    throw Error("unsupported SilverLink screen (" + std::to_string(data.size()) + " bytes)");
  }
  return decode_lcd(data.data(), data.size());
}

void DbusCalc::backup(const std::string &dest) {
  std::vector<uint8_t> bytes = recv_backup();
  write_file(dest, bytes);
}

std::vector<uint8_t> DbusCalc::recv_backup() {
  uint8_t zeros[8] = {};
  std::vector<uint8_t> request = dbus_var_header(0, 0x13, zeros, 0, 0);
  send(pc_id_, CMD_REQ, request.data(), request.size());
  expect(CMD_ACK);
  Packet header = expect(CMD_VAR);
  if (header.data.size() < 9 || header.data[2] != 0x13) {
    throw Error("calculator did not return a RAM backup header");
  }
  uint16_t size1 = static_cast<uint16_t>(header.data[0] | (header.data[1] << 8));
  uint16_t size2 = static_cast<uint16_t>(header.data[3] | (header.data[4] << 8));
  uint16_t size3 = static_cast<uint16_t>(header.data[5] | (header.data[6] << 8));
  uint16_t address = static_cast<uint16_t>(header.data[7] | (header.data[8] << 8));
  uint64_t total = static_cast<uint64_t>(size1) + size2 + size3;
  progress_reset(total);
  send(pc_id_, CMD_ACK, nullptr, 0);
  send(pc_id_, CMD_CTS, nullptr, 0);
  expect(CMD_ACK);
  std::vector<uint8_t> section1 = expect_section(size1);
  progress_set_remaining(total - section1.size());
  std::vector<uint8_t> section2 = expect_section(size2);
  progress_set_remaining(total - section1.size() - section2.size());
  std::vector<uint8_t> section3 = expect_section(size3);
  progress_finish();
  const uint8_t sig82[8] = {'*', '*', 'T', 'I', '8', '2', '*', '*'};
  const uint8_t sig83[8] = {'*', '*', 'T', 'I', '8', '3', '*', '*'};
  const uint8_t sig8x[8] = {'*', '*', 'T', 'I', '8', '3', 'F', '*'};
  const uint8_t *signature = sig8x;
  if (model_.compare(0, 5, "TI-82") == 0) signature = sig82;
  else if (model_ == "TI-83") signature = sig83;
  return write_backup_file(signature, address, section1, section2, section3);
}

uint8_t DbusCalc::ready(uint8_t pc_id) {
  send(pc_id, CMD_RDY, nullptr, 0);
  Packet ack = recv();
  if (ack.cmd != CMD_ACK) throw Error("calculator did not acknowledge SilverLink");
  if (ack.mid == 0x82) pc_id_ = MID_PC_82;
  else if (ack.mid == 0x83) pc_id_ = MID_PC_83;
  else pc_id_ = MID_PC;
  return ack.mid;
}

void DbusCalc::reload() {
  uint8_t zeros[8] = {};
  std::vector<uint8_t> header = dbus_var_header(0, 0x19, zeros, 0, 0);
  send(pc_id_, CMD_REQ, header.data(), header.size());
  expect(CMD_ACK);
  std::vector<Var> vars;
  for (;;) {
    Packet pkt = recv();
    if (pkt.cmd == CMD_VAR) {
      if (std::optional<Var> var = dbus_parse_header(pkt.data.data(), pkt.data.size())) vars.push_back(*var);
      send(pc_id_, CMD_ACK, nullptr, 0);
    } else if (pkt.cmd == CMD_DATA) {
      std::vector<Var> more = parse_header_blob(pkt.data.data(), pkt.data.size());
      vars.insert(vars.end(), more.begin(), more.end());
      send(pc_id_, CMD_ACK, nullptr, 0);
    } else if (pkt.cmd == CMD_EOT) {
      send(pc_id_, CMD_ACK, nullptr, 0);
      break;
    } else if (pkt.cmd == CMD_SKIP || pkt.cmd == CMD_ERR) {
      throw Error("calculator rejected the directory request");
    } else if (pkt.cmd == CMD_ACK) {
    } else {
      throw Error("unexpected SilverLink packet " + hex_u8(pkt.cmd));
    }
  }
  vars_ = std::move(vars);
}

std::vector<uint8_t> DbusCalc::get_var(const std::string &name, uint8_t type_id) {
  uint8_t raw[8];
  name_bytes(name, raw);
  std::vector<uint8_t> header = dbus_var_header(0, type_id, raw, 0, 0);
  send(pc_id_, CMD_REQ, header.data(), header.size());
  expect(CMD_ACK);
  Packet hdr = expect(CMD_VAR);
  std::optional<Var> var = dbus_parse_header(hdr.data.data(), hdr.data.size());
  if (!var) throw Error("bad variable header");
  send(pc_id_, CMD_ACK, nullptr, 0);
  send(pc_id_, CMD_CTS, nullptr, 0);
  expect(CMD_ACK);
  std::vector<uint8_t> data;
  for (;;) {
    Packet pkt = recv();
    if (pkt.cmd == CMD_DATA) {
      data.insert(data.end(), pkt.data.begin(), pkt.data.end());
      send(pc_id_, CMD_ACK, nullptr, 0);
    } else if (pkt.cmd == CMD_EOT) {
      send(pc_id_, CMD_ACK, nullptr, 0);
      break;
    } else if (pkt.cmd == CMD_SKIP || pkt.cmd == CMD_ERR) {
      throw Error("calculator skipped this variable");
    } else {
      throw Error("unexpected SilverLink packet " + hex_u8(pkt.cmd));
    }
  }
  if (static_cast<uint64_t>(data.size()) > MAX_FILE_SIZE || var->size > MAX_FILE_SIZE) {
    throw Error("variable exceeds the safety limit");
  }
  return data;
}

void DbusCalc::put_var(const std::string &name, uint8_t type_id, bool archived, const uint8_t *data, size_t n) {
  if (type_id == 0x23) throw Error("sending an operating system is not supported");
  if (n > 0xffff) throw Error("variable is too large");
  uint8_t raw[8];
  name_bytes(name, raw);
  std::vector<uint8_t> header = dbus_var_header(static_cast<uint16_t>(n), type_id, raw, 0, archived ? 0x80 : 0);
  progress_reset(static_cast<uint64_t>(n));
  send(pc_id_, CMD_RTS, header.data(), header.size());
  expect(CMD_ACK);
  Packet next = recv();
  if (next.cmd == CMD_SKIP) throw Error("calculator skipped this variable");
  if (next.cmd != CMD_CTS) throw Error("calculator did not accept the variable");
  send(pc_id_, CMD_ACK, nullptr, 0);
  send(pc_id_, CMD_DATA, data, n);
  expect(CMD_ACK);
  send(pc_id_, CMD_EOT, nullptr, 0);
  expect(CMD_ACK);
  progress_finish();
}

void DbusCalc::delete_var(const std::string &name, uint8_t type_id) {
  uint8_t raw[8];
  name_bytes(name, raw);
  std::vector<uint8_t> header = dbus_var_header(0, type_id, raw, 0, 0);
  send(pc_id_, CMD_DEL, header.data(), header.size());
  expect(CMD_ACK);
}

std::vector<uint8_t> DbusCalc::expect_section(uint16_t expected) {
  Packet pkt = expect(CMD_DATA);
  if (pkt.data.size() != expected) {
    throw Error("RAM backup section was " + std::to_string(pkt.data.size()) + " bytes, expected " +
                std::to_string(expected));
  }
  send(pc_id_, CMD_ACK, nullptr, 0);
  return pkt.data;
}

DbusCalc::Packet DbusCalc::expect(uint8_t cmd) {
  Packet pkt = recv();
  if (pkt.cmd == CMD_ERR) throw Error("calculator reported a checksum error");
  if (pkt.cmd != cmd) {
    throw Error("expected packet " + hex_u8(cmd) + ", got " + hex_u8(pkt.cmd));
  }
  return pkt;
}

void DbusCalc::send(uint8_t mid, uint8_t cmd, const uint8_t *data, size_t n) {
  std::vector<uint8_t> bytes = dbus_encode(mid, cmd, data, n);
  io_->write_all(bytes.data(), bytes.size());
}

DbusCalc::Packet DbusCalc::recv() {
  rx_.fill_from(*io_, 4);
  std::vector<uint8_t> head = rx_.take(4);
  size_t len = static_cast<size_t>(head[2]) | (static_cast<size_t>(head[3]) << 8);
  Packet pkt;
  pkt.mid = head[0];
  pkt.cmd = head[1];
  if (len == 0) return pkt;
  rx_.fill_from(*io_, len + 2);
  std::vector<uint8_t> tail = rx_.take(len + 2);
  uint16_t sum = 0;
  for (size_t i = 0; i < len; ++i) sum = static_cast<uint16_t>(sum + tail[i]);
  uint16_t got = static_cast<uint16_t>(tail[len] | (static_cast<uint16_t>(tail[len + 1]) << 8));
  if (sum != got) throw Error("SilverLink checksum mismatch");
  pkt.data.assign(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(len));
  return pkt;
}

std::unique_ptr<LinkCalc> open_dbus(std::unique_ptr<BulkIo> io) {
  std::unique_ptr<DbusCalc> calc = DbusCalc::handshake(std::move(io));
  return calc;
}

}  // namespace nlink
