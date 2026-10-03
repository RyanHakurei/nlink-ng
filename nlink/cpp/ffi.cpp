#include "device.hpp"
#include "nlink_ffi.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>

extern "C" void nlink_evo_set_progress_hook(void (*hook)(uint64_t, uint64_t));

extern "C" void nlink_on_evo_progress(uint64_t remaining, uint64_t total) {
  nlink::progress_update(remaining, total);
}

namespace {
struct InstallEvoProgress {
  InstallEvoProgress() { nlink_evo_set_progress_hook(nlink_on_evo_progress); }
};
InstallEvoProgress g_install_evo_progress;

void fill_empty(NLinkString *p) {
  if (!p) return;
  p->data = nullptr;
  p->len = 0;
}

void fill_ok(NLinkString *out, const std::string &value) {
  if (!out) return;
  if (value.find('\0') != std::string::npos) {
    fill_empty(out);
    return;
  }
  char *data = static_cast<char *>(std::malloc(value.size() + 1));
  if (!data) {
    fill_empty(out);
    return;
  }
  std::memcpy(data, value.data(), value.size());
  data[value.size()] = '\0';
  out->data = data;
  out->len = value.size();
}

int fill_err(NLinkString *err, const std::string &message) {
  fill_ok(err, message);
  return -1;
}

std::string cstr(const char *p) {
  if (!p) throw nlink::Error("null string");
  return p;
}

struct ProgressBridge {
  NLinkProgressCb cb = nullptr;
  void *user = nullptr;
  uint64_t last_total = 0;
  void operator()(uint64_t remaining) {
    if (last_total < remaining) last_total = remaining;
    uint64_t total = last_total > remaining ? last_total : remaining;
    nlink::progress_update(remaining, total);
    if (cb) cb(user, remaining, total);
  }
};

template <typename F>
int guard(NLinkString *err, F &&fn) {
  try {
    fn();
    return 0;
  } catch (const std::exception &ex) {
    return fill_err(err, ex.what());
  } catch (...) {
    return fill_err(err, "unknown error");
  }
}

void publish_image(NLinkImage *out, nlink::Image shot) {
  if (!out) return;
  const size_t n = shot.rgba.size();
  uint8_t *rgba = nullptr;
  if (n) {
    rgba = static_cast<uint8_t *>(std::malloc(n));
    if (!rgba) throw std::bad_alloc();
    std::memcpy(rgba, shot.rgba.data(), n);
  }
  out->rgba = rgba;
  out->width = shot.width;
  out->height = shot.height;
  out->stride = static_cast<int>(shot.width) * 4;
}
}  // namespace

namespace nlink {
bool cli_run();
}

extern "C" void nlink_string_free(NLinkString s) {
  std::free(s.data);
}

extern "C" void nlink_progress_get(uint64_t *out_done, uint64_t *out_total) {
  auto got = nlink::progress_get();
  if (out_done) *out_done = got.first;
  if (out_total) *out_total = got.second;
}

extern "C" int nlink_enumerate(NLinkString *out_json, NLinkString *out_err) {
  fill_empty(out_json);
  fill_empty(out_err);
  return guard(out_err, [&] { fill_ok(out_json, nlink::enumerate_json()); });
}

extern "C" int nlink_open(uint8_t bus, uint8_t addr, NLinkString *out_json, NLinkString *out_err) {
  fill_empty(out_json);
  fill_empty(out_err);
  return guard(out_err, [&] { fill_ok(out_json, nlink::open_device(bus, addr)); });
}

extern "C" int nlink_open_android(int32_t fd, uint8_t ep_in, uint8_t ep_out, uint8_t is_cx2,
                                  NLinkString *out_json, NLinkString *out_err) {
  fill_empty(out_json);
  fill_empty(out_err);
  return guard(out_err, [&] {
    fill_ok(out_json, nlink::open_android(fd, ep_in, ep_out, is_cx2 != 0));
  });
}

extern "C" int nlink_open_android_product(int32_t fd, uint8_t ep_in, uint8_t ep_out, uint16_t product,
                                          NLinkString *out_json, NLinkString *out_err) {
  fill_empty(out_json);
  fill_empty(out_err);
  return guard(out_err, [&] {
    fill_ok(out_json, nlink::open_android_product(fd, ep_in, ep_out, product));
  });
}

extern "C" int nlink_close(uint8_t bus, uint8_t addr, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::close_device(bus, addr); });
}

extern "C" int nlink_info(uint8_t bus, uint8_t addr, NLinkString *out_json, NLinkString *out_err) {
  fill_empty(out_json);
  fill_empty(out_err);
  return guard(out_err, [&] { fill_ok(out_json, nlink::device_info(bus, addr)); });
}

extern "C" int nlink_list_dir(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_json,
                              NLinkString *out_err) {
  fill_empty(out_json);
  fill_empty(out_err);
  return guard(out_err, [&] {
    fill_ok(out_json, nlink::file_list_json(nlink::list_dir(bus, addr, cstr(path))));
  });
}

extern "C" int nlink_download_file(uint8_t bus, uint8_t addr, const char *remote, uint64_t size,
                                   const char *dest_dir, NLinkProgressCb cb, void *user,
                                   NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::download_file(bus, addr, cstr(remote), size, cstr(dest_dir), progress);
  });
}

extern "C" int nlink_download_dir(uint8_t bus, uint8_t addr, const char *remote, const char *dest_dir,
                                  NLinkProgressCb cb, void *user, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::download_dir(bus, addr, cstr(remote), cstr(dest_dir), progress);
  });
}

extern "C" int nlink_upload_file(uint8_t bus, uint8_t addr, const char *dest_dir, const char *src,
                                 NLinkProgressCb cb, void *user, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::upload_file(bus, addr, cstr(dest_dir), cstr(src), progress);
  });
}

extern "C" int nlink_mkdir(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::make_dir(bus, addr, cstr(path)); });
}

extern "C" int nlink_rm(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::remove_file(bus, addr, cstr(path)); });
}

extern "C" int nlink_rmdir(uint8_t bus, uint8_t addr, const char *path, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::remove_dir(bus, addr, cstr(path)); });
}

extern "C" int nlink_move(uint8_t bus, uint8_t addr, const char *src, const char *dest,
                          NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::move_path(bus, addr, cstr(src), cstr(dest)); });
}

extern "C" int nlink_copy(uint8_t bus, uint8_t addr, const char *src, const char *dest,
                          NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::copy_path(bus, addr, cstr(src), cstr(dest)); });
}

extern "C" int nlink_upload_os(uint8_t bus, uint8_t addr, const char *src, NLinkProgressCb cb, void *user,
                               NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::upload_os(bus, addr, cstr(src), progress);
  });
}

extern "C" int nlink_backup(uint8_t bus, uint8_t addr, const char *dest, NLinkProgressCb cb, void *user,
                            NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::backup_device(bus, addr, cstr(dest), progress);
  });
}

extern "C" int nlink_rom_dump(uint8_t bus, uint8_t addr, const char *dest, NLinkProgressCb cb, void *user,
                              NLinkString *out_err) {
  fill_empty(out_err);
#ifdef __ANDROID__
  (void)bus;
  (void)addr;
  (void)dest;
  (void)cb;
  (void)user;
  return fill_err(out_err, "Use nlink_rom_dump_android on Android");
#else
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::rom_dump_device(bus, addr, cstr(dest), progress);
  });
#endif
}

extern "C" int nlink_rom_dump_android(int32_t fd, uint8_t ep_in, uint8_t ep_out, const char *dest,
                                      NLinkProgressCb cb, void *user, NLinkString *out_err) {
  fill_empty(out_err);
#ifndef __ANDROID__
  (void)fd;
  (void)ep_in;
  (void)ep_out;
  (void)dest;
  (void)cb;
  (void)user;
  return fill_err(out_err, "nlink_rom_dump_android is only available on Android");
#else
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::rom_dump_android(fd, ep_in, ep_out, cstr(dest), progress);
  });
#endif
}

extern "C" int nlink_restore(uint8_t bus, uint8_t addr, const char *src, NLinkProgressCb cb, void *user,
                             NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] {
    ProgressBridge progress{cb, user, 0};
    nlink::restore_device(bus, addr, cstr(src), progress);
  });
}

extern "C" void nlink_image_free(NLinkImage img) {
  if (!img.rgba || img.height <= 0 || img.stride <= 0) return;
  std::free(img.rgba);
}

extern "C" void nlink_set_io_timeout(uint32_t ms) { nlink::set_io_timeout(ms); }

extern "C" int nlink_screenshot(uint8_t bus, uint8_t addr, NLinkImage *out, NLinkString *out_err) {
  fill_empty(out_err);
  if (out) *out = NLinkImage{nullptr, 0, 0, 0};
  return guard(out_err, [&] { publish_image(out, nlink::screenshot_device(bus, addr)); });
}

extern "C" int nlink_view_frame(uint8_t bus, uint8_t addr, NLinkImage *out, NLinkString *out_err) {
  fill_empty(out_err);
  if (out) *out = NLinkImage{nullptr, 0, 0, 0};
  return guard(out_err, [&] { publish_image(out, nlink::view_frame_device(bus, addr)); });
}

extern "C" int nlink_exit_exam_mode(uint8_t bus, uint8_t addr, NLinkString *out_err) {
  fill_empty(out_err);
  return guard(out_err, [&] { nlink::exit_exam(bus, addr); });
}

extern "C" int nlink_cli_run(void) {
#ifdef __ANDROID__
  return -1;
#else
  return nlink::cli_run() ? 0 : -1;
#endif
}
