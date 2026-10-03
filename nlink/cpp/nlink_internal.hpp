#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nlink {

inline constexpr uint64_t MAX_FILE_SIZE = 256ull * 1024ull * 1024ull;

inline constexpr const char *TIMEOUT_MESSAGE =
    "Timed out waiting for the calculator. This can happen with a corrupted filesystem, a stuck "
    "USB transfer, or a very large file. Disconnect and reconnect the calculator if this persists.";

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;

  static Error timeout() { return Error(TIMEOUT_MESSAGE); }

  static Error busy() {
    return Error(
        "The calculator is busy with another operation. Wait for it to finish, or disconnect and "
        "reconnect if it is stuck.");
  }
};

class BulkIo {
 public:
  virtual ~BulkIo() = default;
  virtual void write_all(const uint8_t *data, size_t n) = 0;
  virtual size_t read_some(uint8_t *buf, size_t cap) = 0;
};

class Pipe : public BulkIo {
 public:
  std::vector<uint8_t> written;
  std::vector<uint8_t> incoming;
  size_t read_pos = 0;

  explicit Pipe(std::vector<uint8_t> in = {}) : incoming(std::move(in)) {}

  void write_all(const uint8_t *data, size_t n) override {
    written.insert(written.end(), data, data + n);
  }

  size_t read_some(uint8_t *buf, size_t cap) override {
    if (read_pos >= incoming.size()) throw Error("calculator closed the USB pipe");
    size_t n = incoming.size() - read_pos;
    if (n > cap) n = cap;
    std::memcpy(buf, incoming.data() + read_pos, n);
    read_pos += n;
    return n;
  }
};

class ByteBuf {
 public:
  void fill_from(BulkIo &io, size_t need) {
    uint8_t tmp[4096];
    while (data_.size() < need) {
      size_t n = io.read_some(tmp, sizeof tmp);
      if (n == 0) throw Error("empty USB read");
      data_.insert(data_.end(), tmp, tmp + n);
    }
  }

  std::vector<uint8_t> take(size_t n) {
    if (data_.size() < n) throw Error("short USB packet");
    std::vector<uint8_t> out(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(n));
    data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
  }

  void clear() { data_.clear(); }

 private:
  std::vector<uint8_t> data_;
};

struct Var {
  std::string name;
  uint8_t type_id = 0;
  uint64_t size = 0;
  bool archived = false;
  uint32_t version = 0;
};

struct FileInfo {
  std::string path;
  bool is_dir = false;
  uint64_t date = 0;
  uint64_t size = 0;
};

struct TiEntry {
  Var var;
  std::vector<uint8_t> data;
};

struct Image {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<uint8_t> rgba;
};

class LinkCalc {
 public:
  virtual ~LinkCalc() = default;
  virtual std::string info_json() const = 0;
  virtual std::string model() const = 0;
  virtual std::vector<FileInfo> list(const std::string &path) = 0;
  virtual void download(const std::string &remote, const std::string &dest_dir) = 0;
  virtual void upload(const std::string &dest_dir, const std::string &src) = 0;
  virtual void remove(const std::string &remote) = 0;
  virtual Image screenshot() = 0;
  virtual void backup(const std::string &dest) = 0;
};

void progress_reset(uint64_t total);
void progress_set_remaining(uint64_t remaining);
void progress_update(uint64_t remaining, uint64_t total);
void progress_finish();
std::pair<uint64_t, uint64_t> progress_get();

const char *type_name(uint8_t type_id);
const char *file_ext(uint8_t type_id);
std::vector<FileInfo> list_vars(const std::vector<Var> &vars, const std::string &path);
const Var &find_var(const std::vector<Var> &vars, const std::string &remote);
bool dest_archived(const std::string &dest_dir);

std::vector<TiEntry> parse_ti(const uint8_t *bytes, size_t n);
std::vector<uint8_t> write_group(const std::vector<TiEntry> &entries);
std::vector<uint8_t> write_8xp(const TiEntry &entry);

std::unique_ptr<LinkCalc> open_dbus(std::unique_ptr<BulkIo> io);
std::unique_ptr<LinkCalc> open_dusb(std::unique_ptr<BulkIo> io);

bool can_rom_dump(const std::string &model);
bool can_ram_backup(const std::string &model);
std::string format_ti_clock(uint64_t seconds);
size_t effective_buffer(size_t offered, const std::string &model);
Image decode_lcd(const uint8_t *data, size_t n);

std::vector<uint8_t> dump_rom(BulkIo &io);

Image decode_view_frame(const uint8_t *bytes, size_t n);

std::string json_escape(const std::string &s);
std::string file_list_json(const std::vector<FileInfo> &files);
std::string make_calc_info(const std::string &family, const std::string &name, uint64_t free_ram,
                           uint64_t total_ram, uint64_t free_storage, uint64_t total_storage,
                           uint64_t major, uint64_t minor, uint64_t patch, const std::string *clock,
                           const std::string *battery, const char *ram_note);

void write_png(const std::string &path, const Image &image);

void write_backup_archive(
    const std::string &path,
    const std::function<void(const std::function<void(const std::string &, bool, const std::vector<uint8_t> &)> &)>
        &collect);
void read_backup_archive(
    const std::string &path,
    const std::function<void(const std::string &, bool, const std::vector<uint8_t> &)> &each);

std::string nspire_user_message(int code, bool android_text);
std::string libusb_user_message(int code);
void nspire_check(int code, bool android_text);

}  // namespace nlink
