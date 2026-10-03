#include "device.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "license_text.inc"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace nlink {
namespace {
struct Dev {
  uint8_t bus = 0;
  uint8_t addr = 0;
};

std::vector<std::string> cmdline() {
#ifdef _WIN32
  int argc = 0;
  LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> out;
  if (!wide) return out;
  for (int i = 0; i < argc; ++i) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
      out.emplace_back();
      continue;
    }
    std::string arg(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), bytes, nullptr, nullptr);
    out.push_back(std::move(arg));
  }
  LocalFree(wide);
  return out;
#else
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> out;
  std::string cur;
  for (char c : raw) {
    if (c == '\0') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
#endif
}

std::filesystem::path cwd() {
  const char *appimage = std::getenv("APPIMAGE");
  const char *appdir = std::getenv("APPDIR");
  const char *owd = std::getenv("OWD");
  if (appimage && *appimage && appdir && *appdir && owd && *owd) return owd;
  return std::filesystem::current_path();
}

std::filesystem::path resolve(const std::string &path) {
  std::filesystem::path file(path);
  if (file.is_absolute()) return file;
  return cwd() / file;
}

[[noreturn]] void usage_error(const std::string &message) {
  std::cerr << "error: " << message << "\n";
  std::exit(2);
}

void print_help() {
  std::cout
      << "n-link 1.0.0\n"
      << "Transfer files with a TI-Nspire, TI-84, or SilverLink calculator\n\n"
      << "Usage: n-link <command> [arguments]\n\n"
      << "Commands:\n"
      << "  upload <files>... <dest>          Upload files to the calculator\n"
      << "  download <files>... <dest>        Download files or directories\n"
      << "  upload-os <file> [--no-check-os]  Install a .tcc/.tco/.tcc2/.tco2/.tct2 OS file\n"
      << "  copy <from> <to>                  Copy a file on the calculator\n"
      << "  move <from> <to>                  Move a file or directory\n"
      << "  mkdir <path>                      Create a directory\n"
      << "  rmdir <path>                      Delete a directory\n"
      << "  rm <paths>...                     Delete files\n"
      << "  ls <path>                         List a directory\n"
      << "  backup [dest]                     Backup to a .tar.gz (default nlink-ng-backup.tar.gz)\n"
      << "  restore <archive>                 Restore a .tar.gz backup\n"
      << "  screenshot [dest]                 Save a PNG (default nspire-screenshot.png)\n"
      << "  exit-exam                         Exit Press-to-Test / exam mode\n"
      << "  license                           View license information\n";
}

std::optional<Dev> first_nspire() {
  std::vector<ListedDevice> list;
  try {
    list = enumerate_devices();
  } catch (const std::exception &error) {
    std::cerr << "Failed to enumerate USB devices: " << error.what() << "\n";
    return std::nullopt;
  }
  for (const auto &device : list) {
    if (device.family != "nspire") continue;
    try {
      open_device(device.bus, device.address);
      return Dev{device.bus, device.address};
    } catch (const std::exception &error) {
      std::cerr << "Failed to initialize calculator: " << error.what() << "\n";
    }
  }
  return std::nullopt;
}

std::optional<Dev> first_listed() {
  try {
    auto list = enumerate_devices();
    if (list.empty()) {
      std::cerr << "Couldn't find any device\n";
      return std::nullopt;
    }
    try {
      open_device(list.front().bus, list.front().address);
      return Dev{list.front().bus, list.front().address};
    } catch (const std::exception &error) {
      std::cerr << "Failed to initialize calculator: " << error.what() << "\n";
      return std::nullopt;
    }
  } catch (const std::exception &error) {
    std::cerr << "Failed to enumerate USB devices: " << error.what() << "\n";
    return std::nullopt;
  }
}

std::string basename_of(const std::string &path) {
  std::string trimmed = path;
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  auto slash = trimmed.find_last_of('/');
  if (slash == std::string::npos) return trimmed;
  return trimmed.substr(slash + 1);
}

std::string join_calc(const std::string &parent, const std::string &name) {
  std::string base = parent;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.empty()) return "/" + name;
  return base + "/" + name;
}

bool download_one(Dev dev, const std::string &remote, const std::filesystem::path &dest_file, uint64_t size) {
  if (size > MAX_FILE_SIZE) {
    std::cerr << "Refusing to download " << remote << ": size " << size << " exceeds safety limit "
              << MAX_FILE_SIZE << "\n";
    return false;
  }
  try {
    if (dest_file.has_parent_path()) std::filesystem::create_directories(dest_file.parent_path());
    auto parent = dest_file.parent_path().string();
    download_file(dev.bus, dev.addr, remote, size, parent, [](uint64_t) {});
    std::cout << "Download " << remote << ": Ok\n";
    return true;
  } catch (const std::exception &error) {
    std::cerr << "Failed to transfer " << remote << ": " << error.what() << "\n";
    return false;
  }
}

std::pair<uint32_t, uint32_t> download_tree(Dev dev, const std::string &remote,
                                            const std::filesystem::path &dest_dir) {
  try {
    std::filesystem::create_directories(dest_dir);
  } catch (const std::exception &error) {
    std::cerr << "Failed to create " << dest_dir.string() << ": " << error.what() << "\n";
    return {0, 1};
  }
  std::vector<FileInfo> list;
  try {
    list = list_dir(dev.bus, dev.addr, remote);
  } catch (const std::exception &error) {
    std::cerr << "Failed to list directory " << remote << ": " << error.what() << "\n";
    return {0, 1};
  }
  uint32_t ok = 0;
  uint32_t fail = 0;
  for (const auto &item : list) {
    if (item.path.empty() || item.path == "." || item.path == "..") continue;
    std::string child_remote = join_calc(remote, item.path);
    auto child_local = dest_dir / item.path;
    if (item.is_dir) {
      auto nested = download_tree(dev, child_remote, child_local);
      ok += nested.first;
      fail += nested.second;
    } else if (download_one(dev, child_remote, child_local, item.size)) {
      ++ok;
    } else {
      ++fail;
    }
  }
  return {ok, fail};
}

void cmd_upload(const std::vector<std::string> &args) {
  if (args.size() < 2) usage_error("upload requires at least one file and a destination");
  std::string dest = args.back();
  while (!dest.empty() && dest.back() == '/') dest.pop_back();
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    std::filesystem::path file = resolve(args[i]);
    std::string name = file.filename().string();
    if (name.empty()) {
      std::cerr << "Failed to get file name\n";
      continue;
    }
    std::error_code exists_ec;
    if (!std::filesystem::is_regular_file(file, exists_ec)) {
      std::cerr << "Failed to read " << args[i] << ": " << (exists_ec ? exists_ec.message() : "not a file")
                << "\n";
      continue;
    }
    try {
      upload_file(dev->bus, dev->addr, dest, file.string(), [](uint64_t) {});
      std::cout << "Upload " << dest << ": Ok\n";
    } catch (const std::exception &error) {
      std::cerr << "Failed: " << error.what() << "\n";
    }
  }
}

void cmd_download(const std::vector<std::string> &args) {
  if (args.size() < 2) usage_error("download requires at least one path and a destination");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  auto dest_root = resolve(args.back());
  try {
    std::filesystem::create_directories(dest_root);
  } catch (const std::exception &error) {
    std::cerr << "Failed to create " << dest_root.string() << ": " << error.what() << "\n";
    return;
  }
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    const std::string &file = args[i];
    PathAttr attr = path_attr(dev->bus, dev->addr, file);
    bool directory = attr.found && attr.is_dir;
    if (!attr.found) {
      try {
        list_dir(dev->bus, dev->addr, file);
        directory = true;
      } catch (const std::exception &error) {
        std::cerr << "Failed to read file info for " << file << ": " << error.what() << "\n";
        continue;
      }
    }
    if (directory) {
      std::string base = basename_of(file);
      auto local = base.empty() ? dest_root : dest_root / base;
      auto counts = download_tree(*dev, file, local);
      std::cout << "Downloaded folder " << file << " => " << local.string() << ": " << counts.first
                << " file(s) ok, " << counts.second << " failed\n";
    } else {
      std::string name = basename_of(file);
      auto dest_path = name.empty() ? dest_root / "download" : dest_root / name;
      download_one(*dev, file, dest_path, attr.size);
    }
  }
}

void cmd_upload_os(std::vector<std::string> args) {
  bool no_check = false;
  std::vector<std::string> files;
  for (const auto &arg : args) {
    if (arg == "--no-check-os") no_check = true;
    else if (!arg.empty() && arg[0] == '-') usage_error("unknown argument " + arg);
    else files.push_back(arg);
  }
  if (files.size() != 1) usage_error("upload-os requires an OS file");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  NspireMeta info;
  try {
    info = nspire_meta(dev->bus, dev->addr);
  } catch (const std::exception &error) {
    std::cerr << "Failed to obtain device info: " << error.what() << "\n";
    return;
  }
  std::filesystem::path file = resolve(files[0]);
  std::string ext = file.extension().string();
  if (ext != info.os_extension) {
    if (no_check) {
      std::cerr << "Warning: " << info.name << " expects file of type " << info.os_extension << "\n";
    } else {
      std::cerr << "Error: " << info.name << " expects file of type " << info.os_extension << "\n";
      std::cerr << "Provide --no-check-os to bypass this check.\n";
      std::exit(1);
    }
  }
  try {
    upload_os(dev->bus, dev->addr, file.string(), [](uint64_t) {});
  } catch (const std::exception &error) {
    std::string message = error.what();
    if (message.find("failed to open") != std::string::npos || message.find("Failed to open") != std::string::npos) {
      std::cerr << "Failed to open file: " << message << "\n";
      std::exit(1);
    }
    if (message.find("failed to read") != std::string::npos) {
      std::cerr << "Failed to read OS file: " << message << "\n";
      std::exit(1);
    }
    std::cerr << "OS Upload failed: " << message << "\n";
  }
}

void cmd_copy(const std::vector<std::string> &args) {
  if (args.size() != 2) usage_error("copy requires a source and a destination");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  try {
    copy_path(dev->bus, dev->addr, args[0], args[1]);
    std::cout << "Copy " << args[0] << " => " << args[1] << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Failed to copy file or directory: " << error.what() << "\n";
  }
}

void cmd_move(const std::vector<std::string> &args) {
  if (args.size() != 2) usage_error("move requires a source and a destination");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  try {
    move_path(dev->bus, dev->addr, args[0], args[1]);
    std::cout << "Move " << args[0] << " => " << args[1] << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Failed to move file or directory: " << error.what() << "\n";
  }
}

void cmd_mkdir(const std::vector<std::string> &args) {
  if (args.size() != 1) usage_error("mkdir requires a path");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  try {
    make_dir(dev->bus, dev->addr, args[0]);
    std::cout << "Create " << args[0] << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Failed to create directory: " << error.what() << "\n";
  }
}

void cmd_rmdir(const std::vector<std::string> &args) {
  if (args.size() != 1) usage_error("rmdir requires a path");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  try {
    remove_dir(dev->bus, dev->addr, args[0]);
    std::cout << "Remove " << args[0] << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Failed to delete directory: " << error.what() << "\n";
  }
}

void cmd_rm(const std::vector<std::string> &args) {
  if (args.empty()) usage_error("rm requires at least one path");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  for (const auto &path : args) {
    try {
      remove_file(dev->bus, dev->addr, path);
      PathAttr attr = path_attr(dev->bus, dev->addr, path);
      if (attr.found) {
        std::cerr << "Delete " << path
                  << " reported success, but it is still on the calculator (type="
                  << (attr.is_dir ? "Directory" : "File") << ", size=" << attr.size
                  << "). The OS may be protecting or recreating this file.\n";
      } else {
        std::cout << "Delete " << path << ": Ok\n";
      }
    } catch (const std::exception &error) {
      std::cerr << "Failed to delete " << path << ": " << error.what() << "\n";
    }
  }
}

void cmd_ls(const std::vector<std::string> &args) {
  if (args.size() != 1) usage_error("ls requires a path");
  auto dev = first_nspire();
  if (!dev) {
    std::cerr << "Couldn't find any device\n";
    return;
  }
  try {
    for (const auto &item : list_dir(dev->bus, dev->addr, args[0])) {
      std::cout.width(10);
      std::cout << item.size << ' ' << item.path << (item.is_dir ? "/" : "") << "\n";
    }
  } catch (const std::exception &error) {
    std::cerr << "Failed to list directory: " << error.what() << "\n";
    std::cerr << TIMEOUT_MESSAGE << "\n";
  }
}

void cmd_backup(const std::vector<std::string> &args) {
  if (args.size() > 1) usage_error("backup takes an optional destination");
  auto dev = first_listed();
  if (!dev) return;
  auto dest = resolve(args.empty() ? "nlink-ng-backup.tar.gz" : args[0]);
  std::cout << "Backing up calculator to " << dest.string() << "\n";
  try {
    backup_device(dev->bus, dev->addr, dest.string(), [](uint64_t) {});
    std::cout << "Backup " << dest.string() << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Backup failed: " << error.what() << "\n";
  }
}

void cmd_restore(const std::vector<std::string> &args) {
  if (args.size() != 1) usage_error("restore requires a .tar.gz archive");
  auto dev = first_listed();
  if (!dev) return;
  auto archive = resolve(args[0]);
  std::cout << "Restoring " << archive.string() << " onto calculator\n";
  try {
    restore_device(dev->bus, dev->addr, archive.string(), [](uint64_t) {});
    std::cout << "Restore " << archive.string() << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Restore failed: " << error.what() << "\n";
  }
}

void cmd_screenshot(const std::vector<std::string> &args) {
  if (args.size() > 1) usage_error("screenshot takes an optional destination");
  auto dev = first_listed();
  if (!dev) return;
  auto dest = resolve(args.empty() ? "nspire-screenshot.png" : args[0]);
  try {
    Image shot = screenshot_device(dev->bus, dev->addr);
    write_png(dest.string(), shot);
    std::cout << "Screenshot " << dest.string() << ": Ok\n";
  } catch (const std::exception &error) {
    std::cerr << "Screenshot failed: " << error.what() << "\n";
  }
}

void cmd_exit_exam() {
  auto dev = first_listed();
  if (!dev) return;
  try {
    exit_exam(dev->bus, dev->addr);
    std::cout << "Exam-mode exit sent. The calculator should restart out of Press-to-Test.\n";
  } catch (const std::exception &error) {
    std::cerr << "Failed to exit exam mode: " << error.what() << "\n";
  }
}

void cmd_license() {
  std::cout << kLicenseText << "\n";
  std::string notice = kNoticeText;
  const std::string url = "https://github.com/RyanHakurei/nlink-ng";
  auto at = notice.find("{}");
  if (at != std::string::npos) notice.replace(at, 2, url);
  std::cout << notice << "\n";
}
}  // namespace

bool cli_run() {
  auto args = cmdline();
  if (args.size() < 2) return false;
  const std::string &cmd = args[1];
  std::vector<std::string> rest(args.begin() + 2, args.end());
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    print_help();
    std::exit(0);
  }
  if (cmd == "--version" || cmd == "-V" || cmd == "-v") {
    std::cout << "n-link 1.0.0\n";
    std::exit(0);
  }
  if (cmd == "upload") cmd_upload(rest);
  else if (cmd == "download") cmd_download(rest);
  else if (cmd == "upload-os") cmd_upload_os(rest);
  else if (cmd == "copy") cmd_copy(rest);
  else if (cmd == "move") cmd_move(rest);
  else if (cmd == "mkdir") cmd_mkdir(rest);
  else if (cmd == "rmdir") cmd_rmdir(rest);
  else if (cmd == "rm") cmd_rm(rest);
  else if (cmd == "ls") cmd_ls(rest);
  else if (cmd == "backup") cmd_backup(rest);
  else if (cmd == "restore") cmd_restore(rest);
  else if (cmd == "screenshot") cmd_screenshot(rest);
  else if (cmd == "exit-exam") {
    if (!rest.empty()) usage_error("exit-exam takes no arguments");
    cmd_exit_exam();
  } else if (cmd == "license") {
    if (!rest.empty()) usage_error("license takes no arguments");
    cmd_license();
  } else {
    std::cerr << "error: unrecognized subcommand '" << cmd << "'\n\n";
    print_help();
    std::exit(2);
  }
  return true;
}

}  // namespace nlink
