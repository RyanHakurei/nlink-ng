#include "viewframe.hpp"

#include <cstring>

namespace nlink {
namespace {

constexpr size_t HEADER_LEN = 24;
constexpr uint16_t FORMAT_GRAY4 = 1;
constexpr uint16_t FORMAT_RGB565 = 2;

uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> gray4_to_rgba(const uint8_t *data, size_t n, size_t pixels) {
  size_t need = (pixels + 1) / 2;
  if (n != need) throw Error("The grayscale view frame is the wrong size.");
  std::vector<uint8_t> out;
  out.reserve(pixels * 4);
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t byte = data[i / 2];
    uint8_t nibble = (i % 2 == 0) ? static_cast<uint8_t>(byte >> 4) : static_cast<uint8_t>(byte & 0x0f);
    uint8_t value = static_cast<uint8_t>(nibble * 17);
    out.push_back(value);
    out.push_back(value);
    out.push_back(value);
    out.push_back(255);
  }
  return out;
}

std::vector<uint8_t> rgb565_to_rgba(const uint8_t *data, size_t n, size_t pixels) {
  if (n != pixels * 2) throw Error("The color view frame is the wrong size.");
  std::vector<uint8_t> out;
  out.reserve(pixels * 4);
  for (size_t i = 0; i < pixels; ++i) {
    uint16_t color = read_le16(data + i * 2);
    uint8_t red5 = static_cast<uint8_t>((color >> 11) & 0x1f);
    uint8_t green6 = static_cast<uint8_t>((color >> 5) & 0x3f);
    uint8_t blue5 = static_cast<uint8_t>(color & 0x1f);
    out.push_back(static_cast<uint8_t>((red5 << 3) | (red5 >> 2)));
    out.push_back(static_cast<uint8_t>((green6 << 2) | (green6 >> 4)));
    out.push_back(static_cast<uint8_t>((blue5 << 3) | (blue5 >> 2)));
    out.push_back(255);
  }
  return out;
}

}  // namespace

Image decode_view_frame(const uint8_t *bytes, size_t n) {
  if (bytes == nullptr) n = 0;
  if (n < HEADER_LEN || std::memcmp(bytes, "NLNKFRM1", 8) != 0) {
    throw Error("The view stream did not start with a frame header.");
  }
  uint16_t width = read_le16(bytes + 8);
  uint16_t height = read_le16(bytes + 10);
  uint16_t format = read_le16(bytes + 12);
  uint32_t length = read_le32(bytes + 20);
  if (n - HEADER_LEN != length) throw Error("The view frame length does not match its header.");
  size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (pixels == 0 || pixels > 640u * 480u) throw Error("The view frame size is not usable.");
  const uint8_t *payload = bytes + HEADER_LEN;
  Image image;
  image.width = width;
  image.height = height;
  if (format == FORMAT_GRAY4) {
    image.rgba = gray4_to_rgba(payload, length, pixels);
  } else if (format == FORMAT_RGB565) {
    image.rgba = rgb565_to_rgba(payload, length, pixels);
  } else {
    throw Error("Unsupported view frame format " + std::to_string(format) + ".");
  }
  return image;
}

}  // namespace nlink
