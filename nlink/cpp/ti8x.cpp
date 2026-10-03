#include "ti8x.hpp"

#include <cstring>

namespace nlink {
namespace {

const uint8_t SIG_83[8] = {'*', '*', 'T', 'I', '8', '3', '*', '*'};
const uint8_t SIG_8X[8] = {'*', '*', 'T', 'I', '8', '3', 'F', '*'};
const uint8_t SIG_82[8] = {'*', '*', 'T', 'I', '8', '2', '*', '*'};
const uint8_t SIG_73[8] = {'*', '*', 'T', 'I', '7', '3', '*', '*'};

uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

void append_le16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xff));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

uint16_t checksum(const uint8_t *data, size_t n) {
  uint16_t sum = 0;
  for (size_t i = 0; i < n; ++i) sum = static_cast<uint16_t>(sum + data[i]);
  return sum;
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

std::string name_of(const uint8_t *raw, size_t n) {
  size_t end = 0;
  while (end < n && raw[end] != 0) ++end;
  return utf8_lossy(raw, end);
}

}  // namespace

std::vector<TiEntry> parse_ti(const uint8_t *bytes, size_t n) {
  if (bytes == nullptr) n = 0;
  if (n < 55) throw Error("not a TI-83/84 variable file");
  bool plus = false;
  if (std::memcmp(bytes, SIG_8X, 8) == 0) {
    plus = true;
  } else if (std::memcmp(bytes, SIG_83, 8) == 0 || std::memcmp(bytes, SIG_82, 8) == 0 ||
             std::memcmp(bytes, SIG_73, 8) == 0) {
    plus = false;
  } else {
    throw Error("unsupported file. TI-84 family files use .8xp, .8xv, and other .8x names");
  }
  size_t data_len = read_le16(bytes + 53);
  size_t body_end = 55 + data_len;
  if (n < body_end + 2) throw Error("TI variable file is truncated");
  const uint8_t *body = bytes + 55;
  uint16_t sum = read_le16(bytes + body_end);
  if (sum != checksum(body, data_len)) throw Error("TI variable file checksum does not match");

  std::vector<TiEntry> entries;
  size_t off = 0;
  while (off + 4 < data_len) {
    uint16_t marker = read_le16(body + off);
    bool ti83p = plus || marker == 0x0d;
    size_t header_len = ti83p ? 17 : 15;
    if (off + header_len > data_len) break;
    size_t size = read_le16(body + off + 2);
    uint8_t type_id = body[off + 4];
    uint32_t version = 0;
    bool archived = false;
    if (ti83p) {
      version = body[off + 13];
      archived = (body[off + 14] & 0x80) != 0;
    }
    size_t data_at = off + header_len;
    if (data_at > data_len || size > data_len - data_at) throw Error("TI variable entry is truncated");
    TiEntry entry;
    entry.var.name = name_of(body + off + 5, 8);
    entry.var.type_id = type_id;
    entry.var.size = size;
    entry.var.archived = archived;
    entry.var.version = version;
    entry.data.assign(body + data_at, body + data_at + size);
    entries.push_back(std::move(entry));
    off = data_at + size;
  }
  if (entries.empty()) throw Error("TI variable file has no entries");
  return entries;
}

std::vector<uint8_t> write_group(const std::vector<TiEntry> &entries) {
  if (entries.empty()) throw Error("RAM backup is empty");
  std::vector<uint8_t> body;
  for (const TiEntry &entry : entries) {
    if (entry.var.type_id == 0x23) throw Error("sending an operating system is not supported");
    if (entry.data.size() > 0xffff) throw Error("variable is too large");
    uint16_t size = static_cast<uint16_t>(entry.data.size());
    append_le16(body, 0x000d);
    append_le16(body, size);
    body.push_back(entry.var.type_id);
    uint8_t name[8] = {};
    size_t name_n = entry.var.name.size() < 8 ? entry.var.name.size() : 8;
    if (name_n != 0) std::memcpy(name, entry.var.name.data(), name_n);
    body.insert(body.end(), name, name + 8);
    body.push_back(static_cast<uint8_t>(entry.var.version));
    body.push_back(entry.var.archived ? 0x80 : 0);
    append_le16(body, size);
    body.insert(body.end(), entry.data.begin(), entry.data.end());
  }
  if (body.size() > 0xffff) throw Error("variable is too large");

  std::vector<uint8_t> out;
  out.insert(out.end(), SIG_8X, SIG_8X + 8);
  out.push_back(0x1a);
  out.push_back(0x0a);
  out.push_back(0x00);
  uint8_t comment[42] = {};
  std::memcpy(comment, "nlink-ng", 8);
  out.insert(out.end(), comment, comment + 42);
  append_le16(out, static_cast<uint16_t>(body.size()));
  uint16_t sum = checksum(body.data(), body.size());
  out.insert(out.end(), body.begin(), body.end());
  append_le16(out, sum);
  return out;
}

std::vector<uint8_t> write_8xp(const TiEntry &entry) { return write_group(std::vector<TiEntry>{entry}); }

}  // namespace nlink
