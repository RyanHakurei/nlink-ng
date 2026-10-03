#include "nlink_internal.hpp"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace nlink {
namespace {
uint32_t crc_table[256];
bool crc_ready = false;

void init_crc() {
  if (crc_ready) return;
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  crc_ready = true;
}

uint32_t crc32_bytes(const uint8_t *data, size_t n) {
  init_crc();
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ data[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

void be32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t n) {
  be32(out, static_cast<uint32_t>(n));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data, data + n);
  uint32_t crc = crc32_bytes(out.data() + start, 4 + n);
  be32(out, crc);
}
}

void write_png(const std::string &path, const Image &image) {
  if (image.width == 0 || image.height == 0 ||
      image.rgba.size() < static_cast<size_t>(image.width) * image.height * 4) {
    throw Error("Screenshot data was truncated");
  }
  std::vector<uint8_t> raw;
  raw.reserve((static_cast<size_t>(image.width) * 4 + 1) * image.height);
  for (uint16_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const uint8_t *row = image.rgba.data() + static_cast<size_t>(y) * image.width * 4;
    raw.insert(raw.end(), row, row + static_cast<size_t>(image.width) * 4);
  }
  uLongf bound = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> deflated(bound);
  if (compress2(deflated.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), Z_DEFAULT_COMPRESSION) !=
      Z_OK) {
    throw Error("failed to encode PNG");
  }
  deflated.resize(bound);

  uint8_t ihdr[13];
  auto put = [](uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
  };
  put(ihdr, image.width);
  put(ihdr + 4, image.height);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
  chunk(png, "IHDR", ihdr, sizeof ihdr);
  chunk(png, "IDAT", deflated.data(), deflated.size());
  chunk(png, "IEND", nullptr, 0);

  std::ofstream out(path, std::ios::binary);
  if (!out) throw Error("failed to create " + path);
  out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!out) throw Error("failed to write " + path);
}
}  // namespace nlink
