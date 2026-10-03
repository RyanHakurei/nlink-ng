#include "device.hpp"

extern "C" {
#include "devinfo.h"
#include "dir.h"
#include "error.h"
#include "file.h"
#include "handle.h"
#include "os.h"
#include "screenshot.h"
#include "usb.h"
#include "view.h"
}

#include <zlib.h>

#ifndef __ANDROID__
#include <libusb.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#include <string.h>
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

extern "C" {
int nlink_evo_open(uint8_t bus, uint8_t addr, char **out_json, char **out_err);
int nlink_evo_open_android(uint8_t ep_in, uint8_t ep_out, char **out_json, char **out_err);
void nlink_evo_close(uint8_t bus, uint8_t addr);
int nlink_evo_connected(uint8_t bus, uint8_t addr);
int nlink_evo_info(uint8_t bus, uint8_t addr, char **out_json, char **out_err);
int nlink_evo_list(uint8_t bus, uint8_t addr, const char *path, char **out_json, char **out_err);
int nlink_evo_download(uint8_t bus, uint8_t addr, const char *remote, const char *dest, char **out_err);
int nlink_evo_upload(uint8_t bus, uint8_t addr, const char *dest_dir, const char *src, char **out_err);
int nlink_evo_remove(uint8_t bus, uint8_t addr, const char *remote, char **out_err);
int nlink_evo_screenshot(uint8_t bus, uint8_t addr, uint8_t **rgba, int *w, int *h, char **out_err);
void nlink_evo_string_free(char *p);
void nlink_evo_buf_free(uint8_t *p, size_t len);
}

namespace nlink {
namespace {
std::recursive_mutex g_mu;

struct Session {
  std::string name;
  std::string family = "nspire";
  bool is_cx_ii = false;
  std::unique_ptr<LinkCalc> link;
  nspire_handle_t *nsp = nullptr;
#ifndef __ANDROID__
  libusb_device_handle *usb = nullptr;
#endif
  std::string cached_info;

  void reset_nspire() {
    if (nsp) {
      nspire_free(nsp);
      nsp = nullptr;
    }
#ifndef __ANDROID__
    if (usb) {
      libusb_close(usb);
      usb = nullptr;
    }
#endif
    cached_info.clear();
  }

  ~Session() { reset_nspire(); }

  Session() = default;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
};

using Key = std::pair<uint8_t, uint8_t>;
std::map<Key, std::unique_ptr<Session>> g_sessions;

struct Kind {
  const char *family;
  const char *name;
};

Kind kind_of(uint16_t pid) {
  switch (pid) {
    case 0xe022:
      return {"nspire", "TI-Nspire CX II"};
    case 0xe012:
      return {"nspire", "TI-Nspire"};
    case 0xe001:
      return {"silverlink", "SilverLink"};
    case 0xe003:
      return {"dusb", "TI-84 Plus"};
    case 0xe008:
      return {"dusb", "TI-84 Plus / CE"};
    case 0xe018:
      return {"evo", "TI-84 Evo"};
    default:
      return {nullptr, nullptr};
  }
}

std::string owned_evo(char *p) {
  std::string out = p ? p : "";
  nlink_evo_string_free(p);
  return out;
}

std::string evo_call(int rc, char *json, char *err) {
  if (rc != 0) {
    std::string message = owned_evo(err);
    nlink_evo_string_free(json);
    throw Error(message.empty() ? "Evo link failed" : message);
  }
  nlink_evo_string_free(err);
  return owned_evo(json);
}

Session &slot(uint8_t bus, uint8_t addr) {
  auto &ptr = g_sessions[{bus, addr}];
  if (!ptr) ptr = std::make_unique<Session>();
  return *ptr;
}

bool link_connected(uint8_t bus, uint8_t addr) {
  if (nlink_evo_connected(bus, addr)) return true;
  auto it = g_sessions.find({bus, addr});
  return it != g_sessions.end() && it->second && it->second->link;
}

void require_nspire_only(uint8_t bus, uint8_t addr) {
  if (link_connected(bus, addr)) throw Error("Not supported on this calculator.");
}

std::string c_field(const char *p, size_t n) { return std::string(p, ::strnlen(p, n)); }

std::string version_json(int major, int minor, int patch, int build) {
  return "{\"major\":" + std::to_string(major) + ",\"minor\":" + std::to_string(minor) +
         ",\"patch\":" + std::to_string(patch) + ",\"build\":" + std::to_string(build) + "}";
}

std::string hw_json(unsigned type) {
  switch (type) {
    case NSPIRE_CAS:
      return "\"Cas\"";
    case NSPIRE_NONCAS:
      return "\"NonCas\"";
    case NSPIRE_CASCX:
      return "\"CasCx\"";
    case NSPIRE_NONCASCX:
      return "\"NonCasCx\"";
    default:
      return "{\"Unknown\":" + std::to_string(type) + "}";
  }
}

std::string battery_json(unsigned status) {
  switch (status) {
    case NSPIRE_BATT_POWERED:
      return "\"Powered\"";
    case NSPIRE_BATT_LOW:
      return "\"Low\"";
    case NSPIRE_BATT_OK:
      return "\"Ok\"";
    default:
      return "{\"Unknown\":" + std::to_string(status) + "}";
  }
}

std::string run_json(unsigned level) {
  switch (level) {
    case NSPIRE_RUNLEVEL_RECOVERY:
      return "\"Recovery\"";
    case NSPIRE_RUNLEVEL_OS:
      return "\"Os\"";
    default:
      return "{\"Unknown\":" + std::to_string(level) + "}";
  }
}

std::string join_path(const std::string &parent, const std::string &name) {
  std::string base = parent;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.empty()) return "/" + name;
  return base + "/" + name;
}

std::string basename_of(const std::string &path) {
  std::string trimmed = path;
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  auto slash = trimmed.find_last_of('/');
  if (slash == std::string::npos) return trimmed;
  return trimmed.substr(slash + 1);
}

void create_dir(const std::string &path) {
  if (path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) throw Error(ec.message());
}

std::vector<uint8_t> read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Error("failed to open " + path);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file_bytes(const std::string &path, const uint8_t *data, size_t n) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw Error("failed to create " + path);
  out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(n));
  if (!out) throw Error("failed to write " + path);
}

std::vector<uint8_t> rgb565_to_rgba(const uint8_t *data, size_t pixels) {
  std::vector<uint8_t> out;
  out.reserve(pixels * 4);
  for (size_t i = 0; i < pixels && (i + 1) * 2 <= pixels * 2; ++i) {
    uint16_t c = static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
    uint8_t r5 = (c >> 11) & 0x1f;
    uint8_t g6 = (c >> 5) & 0x3f;
    uint8_t b5 = c & 0x1f;
    out.push_back(static_cast<uint8_t>((r5 << 3) | (r5 >> 2)));
    out.push_back(static_cast<uint8_t>((g6 << 2) | (g6 >> 4)));
    out.push_back(static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
    out.push_back(255);
  }
  return out;
}

Image image_from_nspire(nspire_image *image) {
  if (!image) throw Error("Empty screenshot");
  uint16_t width = image->width;
  uint16_t height = image->height;
  uint8_t bpp = image->bbp;
  size_t pixels = static_cast<size_t>(width) * height;
  size_t len = pixels * bpp / 8;
  const uint8_t *data = image->data;
  Image out;
  out.width = width;
  out.height = height;
  if (bpp == 16) out.rgba = rgb565_to_rgba(data, pixels);
  else if (bpp == 8) {
    out.rgba.reserve(pixels * 4);
    for (size_t i = 0; i < pixels && i < len; ++i) {
      uint8_t v = data[i];
      out.rgba.insert(out.rgba.end(), {v, v, v, 255});
    }
  } else {
    std::free(image);
    throw Error("Unsupported screenshot depth: " + std::to_string(bpp) + " bpp");
  }
  std::free(image);
  if (out.rgba.size() != pixels * 4) throw Error("Screenshot data was truncated");
  return out;
}

struct NspireCb {
  ProgressFn *fn = nullptr;
  static void thunk(size_t remaining, void *user) {
    auto *self = static_cast<NspireCb *>(user);
    if (self->fn && *self->fn) (*self->fn)(remaining);
  }
};

std::string desktop_info_json(const nspire_devinfo &info) {
  const auto &ver = info.versions[NSPIRE_VER_OS];
  const auto &b1 = info.versions[NSPIRE_VER_BOOT1];
  const auto &b2 = info.versions[NSPIRE_VER_BOOT2];
  std::string json = "{";
  json += "\"free_storage\":" + std::to_string(info.storage.free);
  json += ",\"total_storage\":" + std::to_string(info.storage.total);
  json += ",\"free_ram\":" + std::to_string(info.ram.free);
  json += ",\"total_ram\":" + std::to_string(info.ram.total);
  json += ",\"version\":" + version_json(ver.major, ver.minor / 10, ver.minor % 10, ver.build);
  json += ",\"boot1_version\":" + version_json(b1.major, b1.minor / 10, b1.minor % 10, b1.build);
  json += ",\"boot2_version\":" + version_json(b2.major, b2.minor / 10, b2.minor % 10, b2.build);
  json += ",\"hw_type\":" + hw_json(info.hw_type);
  json += ",\"clock_speed\":" + std::to_string(info.clock_speed);
  json += ",\"lcd\":{\"width\":" + std::to_string(info.lcd.width) +
          ",\"height\":" + std::to_string(info.lcd.height) + ",\"bpp\":" + std::to_string(info.lcd.bbp) +
          ",\"sample_mode\":" + std::to_string(info.lcd.sample_mode) + "}";
  json += ",\"os_extension\":\"" + json_escape(c_field(info.extensions.os, 8)) + "\"";
  json += ",\"file_extension\":\"" + json_escape(c_field(info.extensions.file, 8)) + "\"";
  json += ",\"name\":\"" + json_escape(c_field(info.device_name, 20)) + "\"";
  json += ",\"id\":\"" + json_escape(c_field(info.electronic_id, 28)) + "\"";
  json += ",\"run_level\":" + run_json(info.runlevel);
  json += ",\"battery\":" + battery_json(info.batt.status);
  json += ",\"is_charging\":" + std::string(info.batt.is_charging ? "true" : "false");
  json += ",\"family\":\"nspire\"";
  json += "}";
  return json;
}

std::string android_info_json(const nspire_devinfo &info, bool is_cx2) {
  const auto &ver = info.versions[NSPIRE_VER_OS];
  std::string json = "{";
  json += "\"name\":\"" + json_escape(c_field(info.device_name, 20)) + "\"";
  json += ",\"id\":\"" + json_escape(c_field(info.electronic_id, 28)) + "\"";
  json += ",\"free_storage\":" + std::to_string(info.storage.free);
  json += ",\"total_storage\":" + std::to_string(info.storage.total);
  json += ",\"free_ram\":" + std::to_string(info.ram.free);
  json += ",\"total_ram\":" + std::to_string(info.ram.total);
  json += ",\"clock_speed\":" + std::to_string(info.clock_speed);
  json += ",\"family\":\"nspire\"";
  json += ",\"is_cx_ii\":" + std::string(is_cx2 ? "true" : "false");
  json += ",\"version\":" + version_json(ver.major, ver.minor / 10, ver.minor % 10, ver.build);
  json += "}";
  return json;
}

nspire_handle_t *nspire_of(uint8_t bus, uint8_t addr) {
  auto it = g_sessions.find({bus, addr});
  if (it == g_sessions.end() || !it->second || !it->second->nsp) {
    throw Error(it == g_sessions.end() ? "Failed to find device" : "Device closed");
  }
  return it->second->nsp;
}

std::vector<FileInfo> nspire_list(nspire_handle_t *handle, const std::string &path) {
  std::string shown = path.empty() ? "/" : path;
  nspire_dir_info *dir = nullptr;
  nspire_check(nspire_dirlist(handle, shown.c_str(), &dir), false);
  std::vector<FileInfo> out;
  if (!dir) return out;
  for (uint64_t i = 0; i < dir->num; ++i) {
    FileInfo info;
    info.path = c_field(dir->items[i].name, sizeof dir->items[i].name);
    info.is_dir = dir->items[i].type == NSPIRE_DIR;
    info.date = dir->items[i].date;
    info.size = dir->items[i].size;
    out.push_back(std::move(info));
  }
  nspire_dirlist_free(dir);
  return out;
}

#ifndef __ANDROID__
libusb_context *usb_ctx() {
  static libusb_context *ctx = nullptr;
  if (!ctx && libusb_init(&ctx) != 0) throw Error("LibUSB error");
  return ctx;
}

class LibusbIo : public BulkIo {
 public:
  LibusbIo(libusb_device_handle *dev, int iface, uint8_t ep_in, uint8_t ep_out)
      : dev_(dev), iface_(iface), ep_in_(ep_in), ep_out_(ep_out) {}
  ~LibusbIo() override {
    if (!dev_) return;
    libusb_release_interface(dev_, iface_);
    libusb_close(dev_);
  }
  void write_all(const uint8_t *data, size_t n) override {
    size_t off = 0;
    while (off < n) {
      int transferred = 0;
      int rc = libusb_bulk_transfer(dev_, ep_out_, const_cast<uint8_t *>(data + off),
                                    static_cast<int>(n - off), &transferred, 8000);
      if (rc == LIBUSB_ERROR_TIMEOUT) throw Error::timeout();
      if (rc != 0) throw Error(libusb_user_message(rc));
      if (transferred <= 0) throw Error("empty USB write");
      off += static_cast<size_t>(transferred);
    }
  }
  size_t read_some(uint8_t *buf, size_t cap) override {
    int transferred = 0;
    int rc = libusb_bulk_transfer(dev_, ep_in_, buf, static_cast<int>(cap), &transferred, 8000);
    if (rc == LIBUSB_ERROR_TIMEOUT) throw Error::timeout();
    if (rc != 0) throw Error(libusb_user_message(rc));
    return static_cast<size_t>(std::max(transferred, 0));
  }

 private:
  libusb_device_handle *dev_ = nullptr;
  int iface_ = 0;
  uint8_t ep_in_ = 0;
  uint8_t ep_out_ = 0;
};

std::unique_ptr<BulkIo> open_link_io(libusb_device *dev) {
  libusb_config_descriptor *config = nullptr;
  if (libusb_get_config_descriptor(dev, 0, &config) != 0) libusb_get_active_config_descriptor(dev, &config);
  if (!config) throw Error("calculator has no bulk endpoints");
  struct Cand {
    int iface;
    uint8_t in;
    uint8_t out;
  };
  std::vector<Cand> cands;
  for (int i = 0; i < config->bNumInterfaces; ++i) {
    const auto &iface = config->interface[i];
    for (int alt = 0; alt < iface.num_altsetting; ++alt) {
      const auto &desc = iface.altsetting[alt];
      uint8_t ep_in = 0;
      uint8_t ep_out = 0;
      for (int e = 0; e < desc.bNumEndpoints; ++e) {
        const auto &ep = desc.endpoint[e];
        if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
        if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) ep_in = ep.bEndpointAddress;
        else ep_out = ep.bEndpointAddress;
      }
      if (ep_in && ep_out) cands.push_back({desc.bInterfaceNumber, ep_in, ep_out});
    }
  }
  libusb_free_config_descriptor(config);
  if (cands.empty()) throw Error("calculator has no bulk endpoints");
  Cand cand = cands.front();
  libusb_device_handle *handle = nullptr;
  int rc = libusb_open(dev, &handle);
  if (rc) throw Error(libusb_user_message(rc));
  if (libusb_kernel_driver_active(handle, cand.iface) == 1) libusb_detach_kernel_driver(handle, cand.iface);
  rc = libusb_claim_interface(handle, cand.iface);
  if (rc) {
    libusb_close(handle);
    throw Error(libusb_user_message(rc));
  }
  return std::make_unique<LibusbIo>(handle, cand.iface, cand.in, cand.out);
}

libusb_device *find_usb(uint8_t bus, uint8_t addr) {
  libusb_device **list = nullptr;
  ssize_t n = libusb_get_device_list(usb_ctx(), &list);
  if (n < 0) throw Error("LibUSB error");
  libusb_device *found = nullptr;
  for (ssize_t i = 0; i < n; ++i) {
    if (libusb_get_bus_number(list[i]) == bus && libusb_get_device_address(list[i]) == addr) {
      found = libusb_ref_device(list[i]);
      break;
    }
  }
  libusb_free_device_list(list, 1);
  if (!found) throw Error("Failed to find device");
  return found;
}

uint16_t product_of(libusb_device *dev) {
  libusb_device_descriptor desc{};
  if (libusb_get_device_descriptor(dev, &desc) != 0) throw Error("LibUSB error");
  return desc.idProduct;
}

std::string open_nspire(libusb_device *dev, uint8_t bus, uint8_t addr, bool cx2) {
  Error last("Failed to open calculator");
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
    libusb_device_handle *handle = nullptr;
    int rc = libusb_open(dev, &handle);
    if (rc) {
      last = Error(libusb_user_message(rc));
      continue;
    }
    nspire_handle_t *nsp = nullptr;
    rc = nspire_init(&nsp, handle, cx2);
    if (rc) {
      libusb_close(handle);
      last = Error(nspire_user_message(rc, false));
      continue;
    }
    nspire_devinfo info{};
    rc = nspire_device_info(nsp, &info);
    if (rc) {
      nspire_free(nsp);
      libusb_close(handle);
      last = Error(nspire_user_message(rc, false));
      continue;
    }
    auto &s = slot(bus, addr);
    s.reset_nspire();
    s.nsp = nsp;
    s.usb = handle;
    s.cached_info = desktop_info_json(info);
    return s.cached_info;
  }
  throw last;
}
#endif

#ifdef __ANDROID__
extern "C" int nlink_android_bulk(unsigned char ep, void *ptr, int len, int *transferred, unsigned int timeout);

class AndroidBulk : public BulkIo {
 public:
  AndroidBulk(uint8_t ep_in, uint8_t ep_out) : ep_in_(ep_in), ep_out_(ep_out) {}
  void write_all(const uint8_t *data, size_t n) override {
    size_t off = 0;
    while (off < n) {
      int transferred = 0;
      int rc = nlink_android_bulk(ep_out_, const_cast<uint8_t *>(data + off), static_cast<int>(n - off),
                                  &transferred, 8000);
      if (rc != 0) throw Error("USB write failed (" + std::to_string(rc) + ")");
      if (transferred <= 0) throw Error("empty USB write");
      off += static_cast<size_t>(transferred);
    }
  }
  size_t read_some(uint8_t *buf, size_t cap) override {
    int transferred = 0;
    int rc = nlink_android_bulk(ep_in_, buf, static_cast<int>(cap), &transferred, 8000);
    if (rc != 0) {
      if (rc == -1) throw Error::timeout();
      throw Error("USB read failed (" + std::to_string(rc) + ")");
    }
    return static_cast<size_t>(std::max(transferred, 0));
  }

 private:
  uint8_t ep_in_;
  uint8_t ep_out_;
};
#endif

std::string add_ndless(std::string json, uint8_t bus, uint8_t addr) {
  static const char *dirs[] = {"/ndless", "ndless", "/documents/ndless"};
  for (const char *dir : dirs) {
    std::vector<FileInfo> entries;
    try {
      entries = list_dir(bus, addr, dir);
    } catch (const Error &) {
      continue;
    }
    for (const auto &entry : entries) {
      if (entry.is_dir) continue;
      if (entry.path.size() == std::strlen("ndless_resources.tns") &&
          ::strncasecmp(entry.path.c_str(), "ndless_resources.tns", entry.path.size()) == 0) {
        json.insert(json.size() - 1, ",\"ndless\":\"\"");
        return json;
      }
    }
  }
  return json;
}

std::string tar_name(const std::string &remote) {
  std::string p = remote;
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  for (char &c : p)
    if (c == '\\') c = '/';
  if (p.empty()) throw Error("Invalid backup path");
  size_t start = 0;
  while (start <= p.size()) {
    auto slash = p.find('/', start);
    std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part == ".." || part == ".") throw Error("Invalid backup path");
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return p;
}

std::string calc_path_from_tar(const std::string &name) {
  std::string normalized = name;
  for (char &c : normalized)
    if (c == '\\') c = '/';
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < normalized.size()) {
    auto slash = normalized.find('/', start);
    std::string part = normalized.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part == "..") throw Error("Refusing to restore an unsafe path");
    if (!part.empty() && part != ".") parts.push_back(part);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  if (parts.empty()) throw Error("Refusing to restore an unsafe path");
  std::string out = "";
  for (const auto &part : parts) out += "/" + part;
  return out;
}

bool skip_backup_name(const std::string &name) {
  return name.empty() || name == "." || name == ".." ||
         (name.size() == std::strlen("NspireLogs.zip") &&
          ::strncasecmp(name.c_str(), "NspireLogs.zip", name.size()) == 0);
}

void collect_tree(uint8_t bus, uint8_t addr, const std::string &remote, std::vector<std::string> &dirs,
                  std::vector<std::pair<std::string, uint64_t>> &files) {
  for (const auto &entry : list_dir(bus, addr, remote)) {
    if (skip_backup_name(entry.path)) continue;
    std::string child = join_path(remote, entry.path);
    if (entry.is_dir) {
      dirs.push_back(child);
      collect_tree(bus, addr, child, dirs, files);
    } else {
      files.emplace_back(child, entry.size);
    }
  }
}

void mkdir_exists_ok(uint8_t bus, uint8_t addr, const std::string &path) {
  if (path.empty() || path == "/") return;
  try {
    make_dir(bus, addr, path);
  } catch (const Error &err) {
    try {
      list_dir(bus, addr, path);
      return;
    } catch (const Error &) {
    }
    std::string msg = err.what();
    for (char &c : msg) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (msg.find("exist") == std::string::npos) throw;
  }
}

void ensure_dir(uint8_t bus, uint8_t addr, const std::string &path) {
  std::string trimmed = path;
  while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  if (trimmed.empty()) return;
  std::string cur;
  size_t start = 0;
  while (start < trimmed.size()) {
    auto slash = trimmed.find('/', start);
    std::string part = trimmed.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) {
      cur += "/" + part;
      mkdir_exists_ok(bus, addr, cur);
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
}

}  // namespace

void set_io_timeout(uint32_t ms) { nspire_set_io_timeout(ms); }

std::vector<ListedDevice> enumerate_devices() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  std::vector<ListedDevice> listed;
#ifndef __ANDROID__
  libusb_device **list = nullptr;
  ssize_t n = libusb_get_device_list(usb_ctx(), &list);
  if (n < 0) throw Error("LibUSB error");
  std::vector<Key> live;
  for (ssize_t i = 0; i < n; ++i) {
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
    if (desc.idVendor != 0x0451) continue;
    Kind kind = kind_of(desc.idProduct);
    if (!kind.family) continue;
    uint8_t bus = libusb_get_bus_number(list[i]);
    uint8_t addr = libusb_get_device_address(list[i]);
    live.emplace_back(bus, addr);
    auto &s = slot(bus, addr);
    s.name = kind.name;
    s.family = kind.family;
    s.is_cx_ii = desc.idProduct == 0xe022;
    ListedDevice row;
    row.bus = bus;
    row.address = addr;
    row.name = s.name;
    row.family = s.family;
    row.is_cx_ii = s.is_cx_ii;
    listed.push_back(row);
  }
  libusb_free_device_list(list, 1);
  for (auto it = g_sessions.begin(); it != g_sessions.end();) {
    if (std::find(live.begin(), live.end(), it->first) == live.end()) it = g_sessions.erase(it);
    else ++it;
  }
#endif
  return listed;
}

std::string enumerate_json() {
  auto rows = enumerate_devices();
  std::string json = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i) json += ',';
    const auto &row = rows[i];
    json += "{\"busNumber\":" + std::to_string(row.bus);
    json += ",\"address\":" + std::to_string(row.address);
    json += ",\"name\":\"" + json_escape(row.name) + "\"";
    json += ",\"family\":\"" + json_escape(row.family) + "\"";
    json += ",\"isCxIi\":" + std::string(row.is_cx_ii ? "true" : "false");
    json += ",\"needsDrivers\":false}";
  }
  json += "]";
  return json;
}

std::string open_device(uint8_t bus, uint8_t addr) {
#ifdef __ANDROID__
  (void)bus;
  (void)addr;
  throw Error("Use nlink_open_android on Android");
#else
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    char *json = nullptr;
    char *err = nullptr;
    return evo_call(nlink_evo_info(bus, addr, &json, &err), json, err);
  }
  auto existing = g_sessions.find({bus, addr});
  if (existing != g_sessions.end() && existing->second) {
    if (existing->second->link) return existing->second->link->info_json();
    if (existing->second->nsp && !existing->second->cached_info.empty()) return existing->second->cached_info;
  }
  libusb_device *dev = find_usb(bus, addr);
  struct Unref {
    libusb_device *dev;
    ~Unref() {
      if (dev) libusb_unref_device(dev);
    }
  } unref{dev};
  uint16_t pid = product_of(dev);
  Kind kind = kind_of(pid);
  if (!kind.family) throw Error("Failed to find device");
  auto &s = slot(bus, addr);
  s.name = kind.name;
  s.family = kind.family;
  s.is_cx_ii = pid == 0xe022;
  if (pid == 0xe018) {
    s.link.reset();
    s.reset_nspire();
    char *json = nullptr;
    char *err = nullptr;
    return evo_call(nlink_evo_open(bus, addr, &json, &err), json, err);
  }
  if (pid != 0xe012 && pid != 0xe022) {
    nlink_evo_close(bus, addr);
    s.reset_nspire();
    auto io = open_link_io(dev);
    s.link = pid == 0xe001 ? open_dbus(std::move(io)) : open_dusb(std::move(io));
    return s.link->info_json();
  }
  nlink_evo_close(bus, addr);
  s.link.reset();
  std::string json = open_nspire(dev, bus, addr, pid == 0xe022);
  return add_ndless(std::move(json), bus, addr);
#endif
}

std::string open_android(int fd, uint8_t ep_in, uint8_t ep_out, bool is_cx2) {
#ifndef __ANDROID__
  (void)fd;
  (void)ep_in;
  (void)ep_out;
  (void)is_cx2;
  throw Error("nlink_open_android is only available on Android");
#else
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  nlink_evo_close(0, 0);
  auto &s = slot(0, 0);
  s.link.reset();
  s.reset_nspire();
  nspire_check(nspire_android_setup(fd, ep_in, ep_out), true);
  Error last("Failed to open calculator");
  nspire_handle_t *handle = nullptr;
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(700 * attempt));
    int rc = nspire_init(&handle, nullptr, is_cx2);
    if (rc == 0) break;
    last = Error(nspire_user_message(rc, true));
    handle = nullptr;
    if (attempt == 4) throw last;
  }
  nspire_devinfo info{};
  int rc = nspire_device_info(handle, &info);
  if (rc) {
    nspire_free(handle);
    throw Error(nspire_user_message(rc, true));
  }
  s.nsp = handle;
  s.family = "nspire";
  s.is_cx_ii = is_cx2;
  s.cached_info = android_info_json(info, is_cx2);
  return s.cached_info;
#endif
}

std::string open_android_product(int fd, uint8_t ep_in, uint8_t ep_out, uint16_t product) {
#ifndef __ANDROID__
  (void)fd;
  (void)ep_in;
  (void)ep_out;
  (void)product;
  throw Error("nlink_open_android_product is only available on Android");
#else
  if (product == 0xe012 || product == 0xe022) return open_android(fd, ep_in, ep_out, product == 0xe022);
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  nlink_evo_close(0, 0);
  auto &s = slot(0, 0);
  s.link.reset();
  s.reset_nspire();
  nspire_check(nspire_android_setup(fd, ep_in, ep_out), true);
  Kind kind = kind_of(product);
  s.family = kind.family ? kind.family : "dusb";
  s.name = kind.name ? kind.name : "";
  if (product == 0xe018) {
    char *json = nullptr;
    char *err = nullptr;
    return evo_call(nlink_evo_open_android(ep_in, ep_out, &json, &err), json, err);
  }
  auto io = std::make_unique<AndroidBulk>(ep_in, ep_out);
  s.link = product == 0xe001 ? open_dbus(std::move(io)) : open_dusb(std::move(io));
  return s.link->info_json();
#endif
}

void close_device(uint8_t bus, uint8_t addr) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  bool evo = nlink_evo_connected(bus, addr);
  nlink_evo_close(bus, addr);
  auto it = g_sessions.find({bus, addr});
  if (it == g_sessions.end()) {
    if (!evo) throw Error("Device lost");
    return;
  }
  it->second->link.reset();
  it->second->reset_nspire();
}

std::string device_info(uint8_t bus, uint8_t addr) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    char *json = nullptr;
    char *err = nullptr;
    return evo_call(nlink_evo_info(bus, addr, &json, &err), json, err);
  }
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second && it->second->link) return it->second->link->info_json();
  auto *handle = nspire_of(bus, addr);
  nspire_devinfo info{};
  nspire_check(nspire_device_info(handle, &info), false);
#ifdef __ANDROID__
  return android_info_json(info, true);
#else
  std::string json = desktop_info_json(info);
  it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second) it->second->cached_info = json;
  return add_ndless(std::move(json), bus, addr);
#endif
}

NspireMeta nspire_meta(uint8_t bus, uint8_t addr) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto *handle = nspire_of(bus, addr);
  nspire_devinfo info{};
  nspire_check(nspire_device_info(handle, &info), false);
  NspireMeta meta;
  meta.name = c_field(info.device_name, sizeof info.device_name);
  meta.os_extension = c_field(info.extensions.os, sizeof info.extensions.os);
  meta.id = c_field(info.electronic_id, sizeof info.electronic_id);
  return meta;
}

PathAttr path_attr(uint8_t bus, uint8_t addr, const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  PathAttr attr;
  if (link_connected(bus, addr)) return attr;
  nspire_dir_item item{};
  if (nspire_attr(nspire_of(bus, addr), path.c_str(), &item) != 0) return attr;
  attr.found = true;
  attr.is_dir = item.type == NSPIRE_DIR;
  attr.size = item.size;
  return attr;
}

std::vector<FileInfo> list_dir(uint8_t bus, uint8_t addr, const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    char *json = nullptr;
    char *err = nullptr;
    std::string text = evo_call(nlink_evo_list(bus, addr, path.c_str(), &json, &err), json, err);
    std::vector<FileInfo> out;
    size_t pos = 0;
    while ((pos = text.find("\"path\"", pos)) != std::string::npos) {
      auto colon = text.find(':', pos);
      auto q1 = text.find('"', colon + 1);
      auto q2 = q1;
      if (q1 != std::string::npos) {
        q2 = q1 + 1;
        while (q2 < text.size() && text[q2] != '"') {
          if (text[q2] == '\\' && q2 + 1 < text.size()) q2 += 2;
          else ++q2;
        }
      }
      if (q1 == std::string::npos || q2 >= text.size()) break;
      FileInfo info;
      info.path = text.substr(q1 + 1, q2 - q1 - 1);
      auto end = text.find('}', q2);
      auto field = [&](const char *key) -> size_t {
        auto at = text.find(key, q2);
        if (at == std::string::npos || (end != std::string::npos && at > end)) return std::string::npos;
        return text.find(':', at);
      };
      auto dir_at = field("\"isDir\"");
      if (dir_at != std::string::npos) info.is_dir = text.compare(dir_at + 1, 4, "true") == 0 ||
                                                     text.find("true", dir_at) < (end == std::string::npos ? text.size() : end);
      auto date_at = field("\"date\"");
      if (date_at != std::string::npos) info.date = std::strtoull(text.c_str() + date_at + 1, nullptr, 10);
      auto size_at = field("\"size\"");
      if (size_at != std::string::npos) info.size = std::strtoull(text.c_str() + size_at + 1, nullptr, 10);
      out.push_back(std::move(info));
      pos = q2 + 1;
    }
    return out;
  }
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second && it->second->link) return it->second->link->list(path);
  return nspire_list(nspire_of(bus, addr), path);
}

void download_file(uint8_t bus, uint8_t addr, const std::string &remote, uint64_t size, const std::string &dest_dir,
                   ProgressFn progress) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    char *err = nullptr;
    int rc = nlink_evo_download(bus, addr, remote.c_str(), dest_dir.c_str(), &err);
    if (rc != 0) evo_call(rc, nullptr, err);
    if (progress) progress(0);
    return;
  }
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second && it->second->link) {
    it->second->link->download(remote, dest_dir);
    if (progress) progress(0);
    return;
  }
  if (size > MAX_FILE_SIZE) {
    throw Error("File is " + std::to_string(size) + " bytes, which exceeds the " + std::to_string(MAX_FILE_SIZE) +
                " byte safety limit. This can indicate a corrupted directory entry.");
  }
  auto *handle = nspire_of(bus, addr);
  size_t len = static_cast<size_t>(size);
#ifdef __ANDROID__
  nspire_dir_item item{};
  if (nspire_attr(handle, remote.c_str(), &item) == 0) len = std::max(len, static_cast<size_t>(item.size));
  len = len + 256 * 1024;
  if (size == 0) len = std::max(len, static_cast<size_t>(1024 * 1024));
  if (len > MAX_FILE_SIZE) len = static_cast<size_t>(MAX_FILE_SIZE);
  progress_reset(size);
#endif
  std::vector<uint8_t> buf(len);
  size_t got = 0;
  NspireCb cb{&progress};
  nspire_check(nspire_file_read(handle, remote.c_str(), buf.data(), buf.size(), &got, NspireCb::thunk, &cb), false);
#ifdef __ANDROID__
  progress_finish();
#endif
  std::string folder = dest_dir.empty() ? "." : dest_dir;
  create_dir(folder);
  std::string name = basename_of(remote);
  if (!name.empty()) write_file_bytes(folder + "/" + name, buf.data(), got);
  if (progress) progress(0);
}

void download_dir(uint8_t bus, uint8_t addr, const std::string &remote, const std::string &dest_dir, ProgressFn progress) {
  create_dir(dest_dir);
  for (const auto &entry : list_dir(bus, addr, remote)) {
    if (entry.path.empty() || entry.path == "." || entry.path == "..") continue;
    std::string child = join_path(remote, entry.path);
    if (entry.is_dir) download_dir(bus, addr, child, dest_dir + "/" + entry.path, progress);
    else download_file(bus, addr, child, entry.size, dest_dir, progress);
  }
}

void upload_file(uint8_t bus, uint8_t addr, const std::string &dest_dir, const std::string &src, ProgressFn progress) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    char *err = nullptr;
    int rc = nlink_evo_upload(bus, addr, dest_dir.c_str(), src.c_str(), &err);
    if (rc != 0) evo_call(rc, nullptr, err);
    if (progress) progress(0);
    return;
  }
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second && it->second->link) {
    it->second->link->upload(dest_dir, src);
    if (progress) progress(0);
    return;
  }
  auto buf = read_file(src);
  if (buf.size() > MAX_FILE_SIZE) {
    throw Error("File is " + std::to_string(buf.size()) + " bytes, which exceeds the " + std::to_string(MAX_FILE_SIZE) +
                " byte safety limit.");
  }
  std::string name = basename_of(src);
  if (name.empty()) throw Error("Failed to get file name");
  std::string remote = join_path(dest_dir, name);
#ifdef __ANDROID__
  progress_reset(buf.size());
#endif
  NspireCb cb{&progress};
  nspire_check(nspire_file_write(nspire_of(bus, addr), remote.c_str(), buf.data(), buf.size(), NspireCb::thunk, &cb),
               false);
#ifdef __ANDROID__
  progress_finish();
#endif
  if (progress) progress(0);
}

void make_dir(uint8_t bus, uint8_t addr, const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
  nspire_check(nspire_dir_create(nspire_of(bus, addr), path.c_str()), false);
}

void remove_file(uint8_t bus, uint8_t addr, const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    char *err = nullptr;
    int rc = nlink_evo_remove(bus, addr, path.c_str(), &err);
    if (rc != 0) evo_call(rc, nullptr, err);
    return;
  }
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second && it->second->link) {
    it->second->link->remove(path);
    return;
  }
  nspire_check(nspire_file_delete(nspire_of(bus, addr), path.c_str()), false);
}

void remove_dir(uint8_t bus, uint8_t addr, const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
  nspire_check(nspire_dir_delete(nspire_of(bus, addr), path.c_str()), false);
}

void move_path(uint8_t bus, uint8_t addr, const std::string &src, const std::string &dest) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
  nspire_check(nspire_file_move(nspire_of(bus, addr), src.c_str(), dest.c_str()), false);
}

void copy_path(uint8_t bus, uint8_t addr, const std::string &src, const std::string &dest) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
  nspire_check(nspire_file_copy(nspire_of(bus, addr), src.c_str(), dest.c_str()), false);
}

void upload_os(uint8_t bus, uint8_t addr, const std::string &src, ProgressFn progress) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
  auto buf = read_file(src);
#ifdef __ANDROID__
  progress_reset(buf.size());
#endif
  NspireCb cb{&progress};
  nspire_check(nspire_os_send(nspire_of(bus, addr), buf.data(), buf.size(), NspireCb::thunk, &cb), false);
#ifdef __ANDROID__
  progress_finish();
#endif
  if (progress) progress(0);
}

void backup_device(uint8_t bus, uint8_t addr, const std::string &dest, ProgressFn progress) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_sessions.find({bus, addr});
  if (nlink_evo_connected(bus, addr)) throw Error("Not supported on this calculator.");
  if (it != g_sessions.end() && it->second && it->second->link) {
    it->second->link->backup(dest);
    if (progress) progress(0);
    return;
  }
#ifdef __ANDROID__
  (void)dest;
  throw Error("Backup is not available in the Android build yet.");
#else
  std::vector<std::string> dirs;
  std::vector<std::pair<std::string, uint64_t>> files;
  collect_tree(bus, addr, "/", dirs, files);
  write_backup_archive(dest, [&](const auto &emit) {
    for (const auto &dir : dirs) {
      try {
        emit(tar_name(dir), true, {});
      } catch (const Error &) {
      }
    }
    for (const auto &file : files) {
      if (file.second > MAX_FILE_SIZE) continue;
      std::vector<uint8_t> buf;
      try {
        size_t len = static_cast<size_t>(file.second);
        buf.assign(len, 0);
        size_t got = 0;
        NspireCb cb{&progress};
        nspire_check(nspire_file_read(nspire_of(bus, addr), file.first.c_str(), buf.data(), buf.size(), &got,
                                      NspireCb::thunk, &cb),
                     false);
        buf.resize(got);
        emit(tar_name(file.first), false, buf);
      } catch (const Error &) {
      }
    }
  });
#endif
}

void restore_device(uint8_t bus, uint8_t addr, const std::string &src, ProgressFn progress) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
#ifdef __ANDROID__
  (void)src;
  (void)progress;
  throw Error("Restore is not available in the Android build yet.");
#else
  uint32_t restored = 0;
  read_backup_archive(src, [&](const std::string &name, bool is_dir, const std::vector<uint8_t> &data) {
    std::string remote;
    try {
      remote = calc_path_from_tar(name);
    } catch (const Error &) {
      return;
    }
    if (is_dir) {
      ensure_dir(bus, addr, remote);
      ++restored;
      return;
    }
    auto slash = remote.find_last_of('/');
    if (slash != std::string::npos && slash > 0) ensure_dir(bus, addr, remote.substr(0, slash));
    if (data.size() > MAX_FILE_SIZE) return;
    try {
      NspireCb cb{&progress};
      nspire_check(nspire_file_write(nspire_of(bus, addr), remote.c_str(), const_cast<uint8_t *>(data.data()),
                                     data.size(), NspireCb::thunk, &cb),
                   false);
      ++restored;
    } catch (const Error &) {
    }
  });
  if (restored == 0) throw Error("Restore wrote no files");
#endif
}

void rom_dump_device(uint8_t bus, uint8_t addr, const std::string &dest, ProgressFn progress) {
#ifdef __ANDROID__
  (void)bus;
  (void)addr;
  (void)dest;
  (void)progress;
  throw Error("ROM dump only works on a TI-84 Plus or Silver Edition. The CE and Evo cannot dump a ROM this way.");
#else
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  nlink_evo_close(bus, addr);
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second) it->second->link.reset();
  libusb_device *dev = find_usb(bus, addr);
  struct Unref {
    libusb_device *d;
    ~Unref() {
      if (d) libusb_unref_device(d);
    }
  } unref{dev};
  uint16_t pid = product_of(dev);
  if (pid != 0xe003 && pid != 0xe008) {
    throw Error(
        "ROM dump only works on a TI-84 Plus or Silver Edition. The CE and Evo cannot dump a ROM this way.");
  }
  auto io = open_link_io(dev);
  auto rom = dump_rom(*io);
  auto slash = dest.find_last_of('/');
  if (slash != std::string::npos) create_dir(dest.substr(0, slash));
  write_file_bytes(dest, rom.data(), rom.size());
  if (progress) progress(0);
#endif
}

void rom_dump_android(int fd, uint8_t ep_in, uint8_t ep_out, const std::string &dest, ProgressFn progress) {
#ifndef __ANDROID__
  (void)fd;
  (void)ep_in;
  (void)ep_out;
  (void)dest;
  (void)progress;
  throw Error("nlink_rom_dump_android is only available on Android");
#else
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  nlink_evo_close(0, 0);
  auto &s = slot(0, 0);
  s.link.reset();
  s.reset_nspire();
  nspire_check(nspire_android_setup(fd, ep_in, ep_out), true);
  AndroidBulk io(ep_in, ep_out);
  auto rom = dump_rom(io);
  auto slash = dest.find_last_of('/');
  if (slash != std::string::npos) create_dir(dest.substr(0, slash));
  write_file_bytes(dest, rom.data(), rom.size());
  if (progress) progress(0);
#endif
}

Image screenshot_device(uint8_t bus, uint8_t addr) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (nlink_evo_connected(bus, addr)) {
    uint8_t *rgba = nullptr;
    int w = 0;
    int h = 0;
    char *err = nullptr;
    int rc = nlink_evo_screenshot(bus, addr, &rgba, &w, &h, &err);
    if (rc != 0) evo_call(rc, nullptr, err);
    Image image;
    image.width = static_cast<uint16_t>(w);
    image.height = static_cast<uint16_t>(h);
    size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    image.rgba.assign(rgba, rgba + n);
    nlink_evo_buf_free(rgba, n);
    return image;
  }
  auto it = g_sessions.find({bus, addr});
  if (it != g_sessions.end() && it->second && it->second->link) return it->second->link->screenshot();
  nspire_image *image = nullptr;
  nspire_check(nspire_screenshot(nspire_of(bus, addr), &image), false);
  return image_from_nspire(image);
}

Image view_frame_device(uint8_t bus, uint8_t addr) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (link_connected(bus, addr)) throw Error("Live view is only available on a TI-Nspire running nlink-view.");
  uint8_t *bytes = nullptr;
  uint32_t len = 0;
  int rc = nspire_view_frame(nspire_of(bus, addr), &bytes, &len);
  if (rc != 0) {
    const char *text = nspire_strerror(rc);
    std::string detail = text ? text : ("libnspire error " + std::to_string(rc));
    throw Error(detail + ". Live view needs nlink-view running on the calculator.");
  }
  struct Free {
    uint8_t *p;
    ~Free() { std::free(p); }
  } guard{bytes};
  if (!bytes || len < 24) {
    throw Error("The calculator closed the view stream without a frame. Start nlink-view on the TI-Nspire.");
  }
  return decode_view_frame(bytes, len);
}

void exit_exam(uint8_t bus, uint8_t addr) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  require_nspire_only(bus, addr);
  bool in_exam = false;
  for (const auto &entry : list_dir(bus, addr, "/")) {
    if (entry.is_dir && entry.path == "Press-to-Test") in_exam = true;
  }
  if (!in_exam) throw Error("Calculator does not appear to be in exam mode (no Press-to-Test folder).");
#include "exit_test_mode.inc"
  ProgressFn none;
  NspireCb cb{&none};
  int rc = nspire_file_write(nspire_of(bus, addr), "/Press-to-Test/Exit Test Mode.tns",
                             const_cast<unsigned char *>(kExitTestMode), kExitTestModeLen, NspireCb::thunk, &cb);
#ifdef __ANDROID__
  (void)rc;
#else
  if (rc != 0) {
    std::string message = nspire_user_message(rc, false);
    if (message.find("disconnected") == std::string::npos) throw Error(message);
  }
#endif
}

}  // namespace nlink
