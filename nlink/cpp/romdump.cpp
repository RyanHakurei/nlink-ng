#include "romdump.hpp"

namespace nlink {
namespace {

constexpr uint16_t READY = 0xaa55;
constexpr uint16_t OK = 0x0001;
constexpr uint16_t EXIT = 0x0002;
constexpr uint16_t SIZE = 0x0003;
constexpr uint16_t GETDATA = 0x0005;
constexpr uint16_t DATA = 0x0006;
constexpr uint16_t REPEAT = 0x0007;
constexpr size_t BLOCK = 1024;

uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string hex_u16(uint16_t value) {
  static const char *hex = "0123456789abcdef";
  std::string out = "0x";
  out.push_back(hex[(value >> 12) & 0x0f]);
  out.push_back(hex[(value >> 8) & 0x0f]);
  out.push_back(hex[(value >> 4) & 0x0f]);
  out.push_back(hex[value & 0x0f]);
  return out;
}

void send(BulkIo &io, uint16_t cmd, const uint8_t *data, size_t n) {
  std::vector<uint8_t> pkt = romdump_packet(cmd, data, n);
  io.write_all(pkt.data(), pkt.size());
}

std::pair<uint16_t, std::vector<uint8_t>> recv(BulkIo &io, ByteBuf &rx) {
  rx.fill_from(io, 4);
  std::vector<uint8_t> head = rx.take(4);
  uint16_t cmd = read_le16(head.data());
  size_t len = read_le16(head.data() + 2);
  if (len > 64u * 1024u) throw Error("ROM dumper packet is too large");
  rx.fill_from(io, len + 2);
  std::vector<uint8_t> tail = rx.take(len + 2);
  uint16_t sum = 0;
  for (uint8_t byte : head) sum = static_cast<uint16_t>(sum + byte);
  for (size_t i = 0; i < len; ++i) sum = static_cast<uint16_t>(sum + tail[i]);
  uint16_t got = read_le16(tail.data() + len);
  if (sum != got) throw Error("ROM dumper checksum mismatch");
  std::vector<uint8_t> data(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(len));
  return {cmd, std::move(data)};
}

}  // namespace

std::vector<uint8_t> romdump_packet(uint16_t cmd, const uint8_t *data, size_t n) {
  std::vector<uint8_t> out;
  out.reserve(6 + n);
  out.push_back(static_cast<uint8_t>(cmd & 0xff));
  out.push_back(static_cast<uint8_t>(cmd >> 8));
  out.push_back(static_cast<uint8_t>(n & 0xff));
  out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
  if (n != 0 && data != nullptr) out.insert(out.end(), data, data + n);
  uint16_t sum = 0;
  for (uint8_t byte : out) sum = static_cast<uint16_t>(sum + byte);
  out.push_back(static_cast<uint8_t>(sum & 0xff));
  out.push_back(static_cast<uint8_t>(sum >> 8));
  return out;
}

std::vector<uint8_t> dump_rom(BulkIo &io) {
  ByteBuf rx;
  send(io, READY, nullptr, 0);
  auto ready = recv(io, rx);
  if (ready.first != OK) {
    throw Error(
        "the calculator did not answer as a ROM dumper. On a TI-84 Plus or Silver Edition, run the USB ROM "
        "dumper first. The CE and Evo cannot dump a ROM this way.");
  }
  send(io, SIZE, nullptr, 0);
  auto sized = recv(io, rx);
  if (sized.first != SIZE || sized.second.size() < 4) throw Error("ROM dumper did not report a size");
  size_t total = read_le32(sized.second.data());
  if (total == 0 || total > 8u * 1024u * 1024u) {
    throw Error("ROM dumper reported an unexpected size (" + std::to_string(total) + " bytes)");
  }
  progress_reset(static_cast<uint64_t>(total));
  std::vector<uint8_t> rom(total, 0);
  size_t address = 0;
  while (address < total) {
    uint8_t addr[4] = {
        static_cast<uint8_t>(address & 0xff),
        static_cast<uint8_t>((address >> 8) & 0xff),
        static_cast<uint8_t>((address >> 16) & 0xff),
        static_cast<uint8_t>((address >> 24) & 0xff),
    };
    send(io, GETDATA, addr, 4);
    auto pkt = recv(io, rx);
    std::vector<uint8_t> chunk;
    if (pkt.first == DATA) {
      chunk = std::move(pkt.second);
    } else if (pkt.first == REPEAT) {
      if (pkt.second.size() < 3) throw Error("ROM dumper sent a short repeated block");
      size_t count = read_le16(pkt.second.data());
      if (count < 1) count = 1;
      chunk.assign(count, pkt.second[2]);
    } else {
      throw Error("ROM dumper sent unexpected command " + hex_u16(pkt.first));
    }
    size_t n = chunk.size();
    if (n > total - address) n = total - address;
    if (n > BLOCK) n = BLOCK;
    if (n != 0) std::memcpy(rom.data() + address, chunk.data(), n);
    address += BLOCK;
    size_t left = total - (address < total ? address : total);
    progress_set_remaining(static_cast<uint64_t>(left));
  }
  progress_finish();
  send(io, EXIT, nullptr, 0);
  try {
    recv(io, rx);
  } catch (const Error &) {
  }
  return rom;
}

}  // namespace nlink
