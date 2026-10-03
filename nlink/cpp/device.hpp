#pragma once

#include "nlink_internal.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nlink {

using ProgressFn = std::function<void(uint64_t remaining)>;

struct ListedDevice {
  uint8_t bus = 0;
  uint8_t address = 0;
  std::string name;
  std::string family;
  bool is_cx_ii = false;
  bool needs_drivers = false;
};

struct NspireMeta {
  std::string name;
  std::string os_extension;
  std::string id;
};

struct PathAttr {
  bool found = false;
  bool is_dir = false;
  uint64_t size = 0;
};

std::vector<ListedDevice> enumerate_devices();
std::string enumerate_json();

std::string open_device(uint8_t bus, uint8_t addr);
std::string open_android(int fd, uint8_t ep_in, uint8_t ep_out, bool is_cx2);
std::string open_android_product(int fd, uint8_t ep_in, uint8_t ep_out, uint16_t product);
void close_device(uint8_t bus, uint8_t addr);
std::string device_info(uint8_t bus, uint8_t addr);
NspireMeta nspire_meta(uint8_t bus, uint8_t addr);
PathAttr path_attr(uint8_t bus, uint8_t addr, const std::string &path);

std::vector<FileInfo> list_dir(uint8_t bus, uint8_t addr, const std::string &path);
void download_file(uint8_t bus, uint8_t addr, const std::string &remote, uint64_t size,
                   const std::string &dest_dir, ProgressFn progress);
void download_dir(uint8_t bus, uint8_t addr, const std::string &remote, const std::string &dest_dir,
                  ProgressFn progress);
void upload_file(uint8_t bus, uint8_t addr, const std::string &dest_dir, const std::string &src,
                 ProgressFn progress);
void make_dir(uint8_t bus, uint8_t addr, const std::string &path);
void remove_file(uint8_t bus, uint8_t addr, const std::string &path);
void remove_dir(uint8_t bus, uint8_t addr, const std::string &path);
void move_path(uint8_t bus, uint8_t addr, const std::string &src, const std::string &dest);
void copy_path(uint8_t bus, uint8_t addr, const std::string &src, const std::string &dest);
void upload_os(uint8_t bus, uint8_t addr, const std::string &src, ProgressFn progress);
void backup_device(uint8_t bus, uint8_t addr, const std::string &dest, ProgressFn progress);
void restore_device(uint8_t bus, uint8_t addr, const std::string &src, ProgressFn progress);
void rom_dump_device(uint8_t bus, uint8_t addr, const std::string &dest, ProgressFn progress);
void rom_dump_android(int fd, uint8_t ep_in, uint8_t ep_out, const std::string &dest, ProgressFn progress);
Image screenshot_device(uint8_t bus, uint8_t addr);
Image view_frame_device(uint8_t bus, uint8_t addr);
void exit_exam(uint8_t bus, uint8_t addr);
void set_io_timeout(uint32_t ms);

}  // namespace nlink
