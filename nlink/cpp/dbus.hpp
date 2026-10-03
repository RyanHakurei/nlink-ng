#pragma once

#include "nlink_internal.hpp"

#include <optional>

namespace nlink {

std::vector<uint8_t> dbus_encode(uint8_t mid, uint8_t cmd, const uint8_t *data, size_t n);
std::vector<uint8_t> dbus_var_header(uint16_t size, uint8_t type_id, const uint8_t name[8], uint8_t version,
                                     uint8_t flag);
std::optional<Var> dbus_parse_header(const uint8_t *data, size_t n);

class DbusCalc : public LinkCalc {
 public:
  static std::unique_ptr<DbusCalc> handshake(std::unique_ptr<BulkIo> io);
  BulkIo *transport() const;
  std::string model() const override;
  std::string info_json() const override;
  std::vector<FileInfo> list(const std::string &path) override;
  void download(const std::string &remote, const std::string &dest_dir) override;
  void upload(const std::string &dest_dir, const std::string &src) override;
  void remove(const std::string &remote) override;
  Image screenshot() override;
  void backup(const std::string &dest) override;
  std::vector<uint8_t> recv_backup();

 private:
  struct Packet {
    uint8_t mid = 0;
    uint8_t cmd = 0;
    std::vector<uint8_t> data;
  };

  explicit DbusCalc(std::unique_ptr<BulkIo> io);
  uint8_t ready(uint8_t pc_id);
  void reload();
  std::vector<uint8_t> get_var(const std::string &name, uint8_t type_id);
  void put_var(const std::string &name, uint8_t type_id, bool archived, const uint8_t *data, size_t n);
  void delete_var(const std::string &name, uint8_t type_id);
  std::vector<uint8_t> expect_section(uint16_t expected);
  Packet expect(uint8_t cmd);
  void send(uint8_t mid, uint8_t cmd, const uint8_t *data, size_t n);
  Packet recv();

  std::unique_ptr<BulkIo> io_;
  ByteBuf rx_;
  uint8_t pc_id_;
  std::string model_;
  std::vector<Var> vars_;
};

}  // namespace nlink
