#pragma once

#include "nlink_internal.hpp"

#include <map>
#include <optional>

namespace nlink {

Var parse_var_header(const uint8_t *data, size_t n);

class DusbCalc : public LinkCalc {
 public:
  static std::unique_ptr<DusbCalc> handshake(std::unique_ptr<BulkIo> io);
  BulkIo *transport() const;
  size_t max_raw() const;
  std::string model() const override;
  std::string info_json() const override;
  std::vector<FileInfo> list(const std::string &path) override;
  void download(const std::string &remote, const std::string &dest_dir) override;
  void upload(const std::string &dest_dir, const std::string &src) override;
  void remove(const std::string &remote) override;
  Image screenshot() override;
  void backup(const std::string &dest) override;

 private:
  struct Raw {
    uint8_t kind = 0;
    std::vector<uint8_t> data;
  };
  struct Virt {
    uint16_t kind = 0;
    std::vector<uint8_t> data;
  };

  explicit DusbCalc(std::unique_ptr<BulkIo> io);
  void refresh_info();
  void reload();
  std::vector<uint8_t> get_var(const std::string &name, uint8_t type_id);
  void put_var(const std::string &name, uint8_t type_id, bool archived, uint32_t version, const uint8_t *data,
               size_t n);
  void delete_var(const std::string &name, uint8_t type_id);
  void begin_op();
  std::map<uint16_t, std::vector<uint8_t>> parameters(const uint16_t *ids, size_t count);
  void send_virt(uint16_t kind, const uint8_t *data, size_t n);
  Virt expect_virt(uint16_t kind);
  Virt recv_virt_skip_delay();
  Virt recv_virt();
  void write_raw(uint8_t kind, const uint8_t *data, size_t n);
  Raw read_raw();

  std::unique_ptr<BulkIo> io_;
  ByteBuf rx_;
  size_t max_raw_;
  std::string model_;
  uint32_t product_ = 0;
  uint64_t free_ram_ = 0;
  uint64_t free_flash_ = 0;
  uint64_t total_ram_ = 0;
  uint64_t total_flash_ = 0;
  std::optional<std::string> clock_;
  std::optional<std::string> battery_;
  uint16_t os_major_ = 0;
  uint8_t os_minor_ = 0;
  uint8_t os_patch_ = 0;
  std::vector<Var> vars_;
};

}  // namespace nlink
