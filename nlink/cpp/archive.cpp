#include "nlink_internal.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <vector>

namespace nlink {
namespace {
void put_octal(char *dst, size_t n, uint64_t value) {
  std::memset(dst, '0', n);
  dst[n - 1] = '\0';
  for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
    dst[i] = static_cast<char>('0' + (value & 7));
    value >>= 3;
  }
}

void checksum(char header[512]) {
  std::memset(header + 148, ' ', 8);
  unsigned sum = 0;
  for (int i = 0; i < 512; ++i) sum += static_cast<unsigned char>(header[i]);
  std::snprintf(header + 148, 8, "%06o", sum);
  header[154] = '\0';
  header[155] = ' ';
}

void fill_header(char header[512], const std::string &name, uint64_t size, char typeflag) {
  std::memset(header, 0, 512);
  std::string shown = name.size() > 99 ? name.substr(0, 99) : name;
  std::memcpy(header, shown.data(), shown.size());
  put_octal(header + 100, 8, typeflag == '5' ? 0755 : 0644);
  put_octal(header + 108, 8, 0);
  put_octal(header + 116, 8, 0);
  put_octal(header + 124, 12, size);
  put_octal(header + 136, 12, static_cast<uint64_t>(std::time(nullptr)));
  header[156] = typeflag;
  std::memcpy(header + 257, "ustar", 5);
  header[262] = '0';
  header[263] = '0';
  checksum(header);
}

bool read_exact(gzFile file, void *dst, size_t n) {
  auto *p = static_cast<unsigned char *>(dst);
  size_t got = 0;
  while (got < n) {
    int r = gzread(file, p + got, static_cast<unsigned>(n - got));
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

std::string header_name(const char header[512]) {
  std::string name(header, strnlen(header, 100));
  std::string prefix(header + 345, strnlen(header + 345, 155));
  if (!prefix.empty()) return prefix + "/" + name;
  return name;
}

uint64_t octal(const char *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] == '\0' || p[i] == ' ') continue;
    if (p[i] < '0' || p[i] > '7') break;
    v = (v << 3) + static_cast<uint64_t>(p[i] - '0');
  }
  return v;
}
}

class TarGzWriter {
 public:
  explicit TarGzWriter(const std::string &path) {
    file_ = gzopen(path.c_str(), "wb");
    if (!file_) throw Error("failed to create " + path);
  }

  ~TarGzWriter() {
    if (file_) gzclose(file_);
  }

  void add(const std::string &name, bool is_dir, const uint8_t *data, size_t n) {
    if (name.size() > 99) {
      char link[512];
      fill_header(link, "././@LongLink", name.size() + 1, 'L');
      write_block(link, 512);
      std::vector<uint8_t> payload(name.begin(), name.end());
      payload.push_back(0);
      write_padded(payload.data(), payload.size());
    }
    char header[512];
    fill_header(header, name, is_dir ? 0 : n, is_dir ? '5' : '0');
    write_block(header, 512);
    if (!is_dir && n) write_padded(data, n);
  }

  void finish() {
    char zero[1024];
    std::memset(zero, 0, sizeof zero);
    write_block(zero, sizeof zero);
    if (gzclose(file_) != Z_OK) {
      file_ = nullptr;
      throw Error("failed to finish backup archive");
    }
    file_ = nullptr;
  }

 private:
  gzFile file_ = nullptr;

  void write_block(const void *data, size_t n) {
    if (gzwrite(file_, data, static_cast<unsigned>(n)) == 0) throw Error("failed to write backup archive");
  }

  void write_padded(const uint8_t *data, size_t n) {
    write_block(data, n);
    size_t pad = (512 - (n % 512)) % 512;
    if (pad) {
      char zeros[512];
      std::memset(zeros, 0, pad);
      write_block(zeros, pad);
    }
  }
};

void write_backup_archive(
    const std::string &path,
    const std::function<void(const std::function<void(const std::string &, bool, const std::vector<uint8_t> &)> &)>
        &collect) {
  TarGzWriter writer(path);
  uint32_t wrote = 0;
  collect([&](const std::string &name, bool is_dir, const std::vector<uint8_t> &bytes) {
    writer.add(name, is_dir, bytes.data(), bytes.size());
    ++wrote;
  });
  writer.finish();
  if (wrote == 0) throw Error("Backup contained no files");
}

void read_backup_archive(
    const std::string &path,
    const std::function<void(const std::string &, bool, const std::vector<uint8_t> &)> &each) {
  gzFile file = gzopen(path.c_str(), "rb");
  if (!file) throw Error("failed to open " + path);
  std::string long_name;
  while (true) {
    char header[512];
    if (!read_exact(file, header, 512)) {
      gzclose(file);
      throw Error("backup archive ended early");
    }
    bool zero = true;
    for (unsigned char c : header) {
      if (c) {
        zero = false;
        break;
      }
    }
    if (zero) break;
    uint64_t size = octal(header + 124, 12);
    char type = header[156];
    std::string name = long_name.empty() ? header_name(header) : long_name;
    long_name.clear();
    if (size > MAX_FILE_SIZE && type != '5') {
      uint64_t skip = size + ((512 - (size % 512)) % 512);
      std::vector<uint8_t> junk(4096);
      while (skip) {
        size_t n = skip > junk.size() ? junk.size() : static_cast<size_t>(skip);
        if (!read_exact(file, junk.data(), n)) {
          gzclose(file);
          throw Error("backup archive ended early");
        }
        skip -= n;
      }
      continue;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size && !read_exact(file, data.data(), data.size())) {
      gzclose(file);
      throw Error("backup archive ended early");
    }
    size_t pad = (512 - (size % 512)) % 512;
    if (pad) {
      char junk[512];
      if (!read_exact(file, junk, pad)) {
        gzclose(file);
        throw Error("backup archive ended early");
      }
    }
    if (type == 'L' || name == "././@LongLink") {
      while (!data.empty() && data.back() == 0) data.pop_back();
      long_name.assign(data.begin(), data.end());
      continue;
    }
    if (type == 'x' || type == 'g') continue;
    bool is_dir = type == '5' || (!name.empty() && name.back() == '/');
    each(name, is_dir, data);
  }
  gzclose(file);
}
}  // namespace nlink
