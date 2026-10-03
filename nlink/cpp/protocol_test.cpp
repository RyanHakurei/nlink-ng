#include "dbus.hpp"
#include "dusb.hpp"
#include "nlink_internal.hpp"
#include "paths.hpp"
#include "romdump.hpp"
#include "ti8x.hpp"
#include "viewframe.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

std::string hex_of(const uint8_t *data, size_t n) {
  static const char *hex = "0123456789abcdef";
  std::string out;
  size_t limit = n < 48 ? n : 48;
  for (size_t i = 0; i < limit; ++i) {
    out.push_back(hex[data[i] >> 4]);
    out.push_back(hex[data[i] & 0x0f]);
  }
  if (n > limit) out += "...";
  return out;
}

bool same_bytes(const std::vector<uint8_t> &got, const uint8_t *expect, size_t n) {
  return got.size() == n && (n == 0 || std::memcmp(got.data(), expect, n) == 0);
}

bool window_eq(const std::vector<uint8_t> &hay, const uint8_t *needle, size_t n) {
  if (n == 0) return true;
  if (hay.size() < n) return false;
  for (size_t i = 0; i + n <= hay.size(); ++i) {
    if (std::memcmp(hay.data() + i, needle, n) == 0) return true;
  }
  return false;
}

void run(const char *name, std::string (*fn)()) {
  try {
    std::string why = fn();
    if (why.empty()) {
      std::cout << "ok " << name << "\n";
    } else {
      std::cout << "FAIL " << name << " " << why << "\n";
      ++g_fails;
    }
  } catch (const std::exception &ex) {
    std::cout << "FAIL " << name << " " << ex.what() << "\n";
    ++g_fails;
  }
}

std::string expect_error(const char *what, void (*fn)()) {
  try {
    fn();
  } catch (const nlink::Error &ex) {
    if (std::string(ex.what()) != what) return std::string("message ") + ex.what();
    return "";
  } catch (const std::exception &ex) {
    return std::string("other exception ") + ex.what();
  }
  return "expected error";
}

std::vector<nlink::Var> sample_vars() {
  nlink::Var hello;
  hello.name = "HELLO";
  hello.type_id = 0x05;
  hello.size = 12;
  hello.archived = false;
  hello.version = 0;
  nlink::Var data;
  data.name = "DATA";
  data.type_id = 0x15;
  data.size = 4;
  data.archived = true;
  data.version = 0;
  return {hello, data};
}

bool any_dir(const std::vector<nlink::FileInfo> &files, const char *path) {
  for (const nlink::FileInfo &file : files) {
    if (file.path == path && file.is_dir) return true;
  }
  return false;
}

bool any_path(const std::vector<nlink::FileInfo> &files, const char *path) {
  for (const nlink::FileInfo &file : files) {
    if (file.path == path) return true;
  }
  return false;
}

std::string test_virtual_tree() {
  std::vector<nlink::Var> vars = sample_vars();
  std::vector<nlink::FileInfo> root = nlink::list_vars(vars, "/");
  if (!any_dir(root, "RAM")) return "root missing RAM";
  if (!any_dir(root, "Archive")) return "root missing Archive";
  if (any_path(root, "Apps")) return "Apps should be omitted";
  std::vector<nlink::FileInfo> ram = nlink::list_vars(vars, "/RAM");
  if (!any_path(ram, "Program")) return "RAM missing Program";
  std::vector<nlink::FileInfo> progs = nlink::list_vars(vars, "/RAM/Program");
  if (progs.size() != 1) return "program count";
  if (progs[0].path != "HELLO" || progs[0].size != 12) return "HELLO entry";
  nlink::Var found = nlink::find_var(vars, "/Archive/AppVar/DATA");
  if (!found.archived) return "DATA not archived";
  std::string folder = expect_error("choose a variable, not a folder", [] {
    std::vector<nlink::Var> owned = sample_vars();
    nlink::find_var(owned, "/RAM");
  });
  if (!folder.empty()) return folder;
  std::string missing = expect_error("variable not found", [] {
    std::vector<nlink::Var> owned = sample_vars();
    nlink::find_var(owned, "/RAM/Program/NOPE");
  });
  if (!missing.empty()) return missing;
  std::string deep = expect_error("that folder does not exist", [] {
    std::vector<nlink::Var> owned = sample_vars();
    nlink::list_vars(owned, "/a/b/c");
  });
  if (!deep.empty()) return deep;
  if (!nlink::dest_archived("/Archive/Program") || nlink::dest_archived("/RAM/Program")) return "dest_archived";
  return "";
}

std::string test_round_trip_program() {
  nlink::TiEntry entry;
  entry.var.name = "HELLO";
  entry.var.type_id = 0x05;
  entry.var.size = 3;
  entry.var.archived = true;
  entry.var.version = 1;
  entry.data = {'a', 'b', 'c'};
  std::vector<uint8_t> bytes = nlink::write_8xp(entry);
  if (bytes.size() < 8 || std::memcmp(bytes.data(), "**TI83F*", 8) != 0) return "signature";
  if (std::memcmp(bytes.data() + 11, "nlink-ng", 8) != 0) return "comment";
  std::vector<nlink::TiEntry> parsed = nlink::parse_ti(bytes.data(), bytes.size());
  if (parsed.size() != 1) return "count";
  if (parsed[0].var.name != "HELLO") return "name";
  if (parsed[0].var.type_id != 0x05) return "type";
  if (!parsed[0].var.archived) return "archived";
  if (parsed[0].var.version != 1) return "version";
  if (parsed[0].data.size() != 3 || std::memcmp(parsed[0].data.data(), "abc", 3) != 0) return "data";
  return "";
}

std::string test_rejects_bad_checksum() {
  nlink::TiEntry entry;
  entry.var.name = "A";
  entry.var.type_id = 0x00;
  entry.var.size = 1;
  entry.var.archived = false;
  entry.var.version = 0;
  entry.data = {1};
  std::vector<uint8_t> bytes = nlink::write_8xp(entry);
  bytes.back() ^= 0xff;
  try {
    nlink::parse_ti(bytes.data(), bytes.size());
  } catch (const nlink::Error &ex) {
    if (std::string(ex.what()) != "TI variable file checksum does not match") return ex.what();
    return "";
  }
  return "expected checksum error";
}

std::string test_ack_packet() {
  std::vector<uint8_t> ready = nlink::dbus_encode(0x23, 0x68, nullptr, 0);
  const uint8_t ready_expect[] = {0x23, 0x68, 0x00, 0x00};
  if (!same_bytes(ready, ready_expect, sizeof ready_expect)) return "ready " + hex_of(ready.data(), ready.size());
  const uint8_t payload[] = {0x01, 0x02};
  std::vector<uint8_t> pkt = nlink::dbus_encode(0x73, 0x15, payload, 2);
  const uint8_t expect[] = {0x73, 0x15, 0x02, 0x00, 0x01, 0x02, 0x03, 0x00};
  if (!same_bytes(pkt, expect, sizeof expect)) return "data " + hex_of(pkt.data(), pkt.size());
  return "";
}

std::string test_scripted_ready() {
  std::vector<uint8_t> ack = nlink::dbus_encode(0x73, 0x56, nullptr, 0);
  auto pipe = std::make_unique<nlink::Pipe>(std::move(ack));
  std::unique_ptr<nlink::DbusCalc> calc = nlink::DbusCalc::handshake(std::move(pipe));
  if (calc->model() != "TI-83 Plus / TI-84 Plus") return "model " + calc->model();
  auto *raw = dynamic_cast<nlink::Pipe *>(calc->transport());
  if (raw == nullptr) return "transport";
  std::vector<uint8_t> expect = nlink::dbus_encode(0x23, 0x68, nullptr, 0);
  if (raw->written != expect) return "written " + hex_of(raw->written.data(), raw->written.size());
  const std::string info =
      "{\"name\":\"TI-83 Plus / TI-84 Plus\",\"family\":\"silverlink\",\"id\":\"\",\"free_storage\":0,"
      "\"total_storage\":0,\"free_ram\":0,\"total_ram\":0,\"is_cx_ii\":false,\"version\":{\"major\":0,"
      "\"minor\":0,\"patch\":0,\"build\":0}}";
  if (calc->info_json() != info) return "info " + calc->info_json();
  return "";
}

std::string test_scripted_backup() {
  std::vector<uint8_t> incoming = nlink::dbus_encode(0x73, 0x56, nullptr, 0);
  const uint8_t var[] = {0x02, 0x00, 0x13, 0x02, 0x00, 0x02, 0x00, 0x95, 0x9d};
  std::vector<uint8_t> var_pkt = nlink::dbus_encode(0x73, 0x06, var, sizeof var);
  incoming.insert(incoming.end(), var_pkt.begin(), var_pkt.end());
  std::vector<uint8_t> ack = nlink::dbus_encode(0x73, 0x56, nullptr, 0);
  incoming.insert(incoming.end(), ack.begin(), ack.end());
  const uint8_t s1[] = {0x11, 0x22};
  const uint8_t s2[] = {0x33, 0x44};
  const uint8_t s3[] = {0x55, 0x66};
  std::vector<uint8_t> d1 = nlink::dbus_encode(0x73, 0x15, s1, 2);
  std::vector<uint8_t> d2 = nlink::dbus_encode(0x73, 0x15, s2, 2);
  std::vector<uint8_t> d3 = nlink::dbus_encode(0x73, 0x15, s3, 2);
  incoming.insert(incoming.end(), d1.begin(), d1.end());
  incoming.insert(incoming.end(), d2.begin(), d2.end());
  incoming.insert(incoming.end(), d3.begin(), d3.end());
  std::vector<uint8_t> ready = nlink::dbus_encode(0x73, 0x56, nullptr, 0);
  ready.insert(ready.end(), incoming.begin(), incoming.end());
  auto pipe = std::make_unique<nlink::Pipe>(std::move(ready));
  std::unique_ptr<nlink::DbusCalc> calc = nlink::DbusCalc::handshake(std::move(pipe));
  std::vector<uint8_t> file = calc->recv_backup();
  if (file.size() < 8 || std::memcmp(file.data(), "**TI83F*", 8) != 0) return "signature";
  const uint8_t a[] = {0x11, 0x22};
  const uint8_t c[] = {0x55, 0x66};
  if (!window_eq(file, a, 2)) return "missing section 1";
  if (!window_eq(file, c, 2)) return "missing section 3";
  return "";
}

std::string test_directory_blob() {
  uint8_t name[8] = {};
  std::memcpy(name, "HELLO", 5);
  std::vector<uint8_t> header = nlink::dbus_var_header(4, 0x05, name, 0, 0);
  std::optional<nlink::Var> parsed = nlink::dbus_parse_header(header.data(), header.size());
  if (!parsed || parsed->name != "HELLO") return "name";
  header[12] = 0x80;
  parsed = nlink::dbus_parse_header(header.data(), header.size());
  if (!parsed || !parsed->archived || parsed->type_id != 0x05) return "archived";
  return "";
}

std::string test_moody_header() {
  const uint8_t data[] = {
      0x00, 0x01, 'A',  0x00, 0x00, 0x06, 0x00, 0x02, 0x00, 0x00, 0x04, 0xf0, 0x07, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x05, 0x01, 0x00, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x09, 0x00, 0x41, 0x01, 0x00, 0x42,
      0x01,
  };
  nlink::Var var = nlink::parse_var_header(data, sizeof data);
  if (var.name != "A") return "name";
  if (var.type_id != 0) return "type";
  if (var.size != 9) return "size " + std::to_string(var.size);
  if (var.archived) return "archived";
  return "";
}

std::string test_clock() {
  if (nlink::format_ti_clock(0) != "1997-01-01 00:00:00") return nlink::format_ti_clock(0);
  if (nlink::format_ti_clock(3661) != "1997-01-01 01:01:01") return nlink::format_ti_clock(3661);
  return "";
}

std::string test_ce_buffer() {
  if (nlink::effective_buffer(1024, "TI-84 Plus CE") != 1018) return "ce";
  if (nlink::effective_buffer(250, "TI-84 Plus") != 250) return "250";
  if (nlink::effective_buffer(4096, "TI-84 Plus") != 1024) return "4096";
  if (!nlink::can_rom_dump("TI-84 Plus")) return "84 dump";
  if (nlink::can_rom_dump("TI-84 Plus CE") || nlink::can_rom_dump("TI-84 Evo") || nlink::can_rom_dump("TI-83 Plus")) {
    return "dump reject";
  }
  if (nlink::can_ram_backup("TI-84 Plus CE") || !nlink::can_ram_backup("TI-84 Plus")) return "ram backup";
  return "";
}

void append_bytes(std::vector<uint8_t> &out, const uint8_t *data, size_t n) { out.insert(out.end(), data, data + n); }

void append_be16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_be32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

std::string test_dusb_handshake() {
  const uint8_t alloc[] = {0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0xfa};
  const uint8_t raw_ack[] = {0x00, 0x00, 0x00, 0x02, 0x05, 0xe0, 0x00};
  const uint8_t mode[] = {0x00, 0x00, 0x00, 0x0a, 0x04, 0x00, 0x00, 0x00, 0x04,
                          0x00, 0x12, 0x00, 0x00, 0x07, 0xd0};
  std::vector<uint8_t> incoming;
  append_bytes(incoming, alloc, sizeof alloc);
  append_bytes(incoming, raw_ack, sizeof raw_ack);
  append_bytes(incoming, mode, sizeof mode);
  append_bytes(incoming, raw_ack, sizeof raw_ack);
  std::vector<uint8_t> payload;
  append_be16(payload, 1);
  append_be16(payload, 0x0002);
  payload.push_back(0);
  const char *name = "TI-84 Plus CE";
  append_be16(payload, static_cast<uint16_t>(std::strlen(name)));
  payload.insert(payload.end(), name, name + std::strlen(name));
  std::vector<uint8_t> virt;
  append_be32(virt, static_cast<uint32_t>(payload.size()));
  append_be16(virt, 0x0008);
  virt.insert(virt.end(), payload.begin(), payload.end());
  std::vector<uint8_t> parm;
  append_be32(parm, static_cast<uint32_t>(virt.size()));
  parm.push_back(4);
  parm.insert(parm.end(), virt.begin(), virt.end());
  incoming.insert(incoming.end(), parm.begin(), parm.end());
  auto pipe = std::make_unique<nlink::Pipe>(std::move(incoming));
  std::unique_ptr<nlink::DusbCalc> calc = nlink::DusbCalc::handshake(std::move(pipe));
  if (calc->model() != "TI-84 Plus CE") return "model " + calc->model();
  if (calc->max_raw() != 250) return "max_raw " + std::to_string(calc->max_raw());
  auto *raw = dynamic_cast<nlink::Pipe *>(calc->transport());
  if (raw == nullptr) return "transport";
  const uint8_t prefix[] = {0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x04, 0x00};
  if (raw->written.size() < 9 || std::memcmp(raw->written.data(), prefix, 9) != 0) {
    return "prefix " + hex_of(raw->written.data(), raw->written.size());
  }
  const uint8_t mode_normal[] = {0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x07, 0xd0};
  if (!window_eq(raw->written, mode_normal, sizeof mode_normal)) return "mode window";
  const std::string info =
      "{\"name\":\"TI-84 Plus CE\",\"family\":\"dusb\",\"id\":\"\",\"free_storage\":0,\"total_storage\":0,"
      "\"free_ram\":0,\"total_ram\":0,\"is_cx_ii\":false,\"version\":{\"major\":0,\"minor\":0,\"patch\":0,"
      "\"build\":0},\"ram_note\":\"CE reports no free RAM while the home screen is up\"}";
  if (calc->info_json() != info) return "info " + calc->info_json();
  return "";
}

std::string test_mono_screen() {
  std::vector<uint8_t> blank(768, 0);
  nlink::Image image = nlink::decode_lcd(blank.data(), blank.size());
  if (image.width != 96 || image.height != 64 || image.rgba.size() != 96u * 64u * 4u) return "geometry";
  return "";
}

std::string test_rom_dump() {
  std::vector<uint8_t> incoming = nlink::romdump_packet(0x0001, nullptr, 0);
  uint8_t size[4] = {0x00, 0x08, 0x00, 0x00};
  std::vector<uint8_t> size_pkt = nlink::romdump_packet(0x0003, size, 4);
  incoming.insert(incoming.end(), size_pkt.begin(), size_pkt.end());
  std::vector<uint8_t> block(1024, 0xab);
  std::vector<uint8_t> data_pkt = nlink::romdump_packet(0x0006, block.data(), block.size());
  incoming.insert(incoming.end(), data_pkt.begin(), data_pkt.end());
  uint8_t repeat[4] = {0x00, 0x04, 0x3c, 0x3c};
  std::vector<uint8_t> repeat_pkt = nlink::romdump_packet(0x0007, repeat, 4);
  incoming.insert(incoming.end(), repeat_pkt.begin(), repeat_pkt.end());
  std::vector<uint8_t> exit_pkt = nlink::romdump_packet(0x0002, nullptr, 0);
  incoming.insert(incoming.end(), exit_pkt.begin(), exit_pkt.end());
  nlink::Pipe pipe(std::move(incoming));
  std::vector<uint8_t> rom = nlink::dump_rom(pipe);
  if (rom.size() != 2048) return "len " + std::to_string(rom.size());
  for (size_t i = 0; i < 1024; ++i) {
    if (rom[i] != 0xab) return "data byte";
  }
  for (size_t i = 1024; i < rom.size(); ++i) {
    if (rom[i] != 0x3c) return "repeat byte";
  }
  return "";
}

std::vector<uint8_t> frame_header(uint16_t width, uint16_t height, uint16_t format, const uint8_t *payload, size_t n) {
  std::vector<uint8_t> bytes(24 + n, 0);
  std::memcpy(bytes.data(), "NLNKFRM1", 8);
  bytes[8] = static_cast<uint8_t>(width & 0xff);
  bytes[9] = static_cast<uint8_t>(width >> 8);
  bytes[10] = static_cast<uint8_t>(height & 0xff);
  bytes[11] = static_cast<uint8_t>(height >> 8);
  bytes[12] = static_cast<uint8_t>(format & 0xff);
  bytes[13] = static_cast<uint8_t>(format >> 8);
  bytes[16] = 1;
  bytes[20] = static_cast<uint8_t>(n & 0xff);
  bytes[21] = static_cast<uint8_t>((n >> 8) & 0xff);
  bytes[22] = static_cast<uint8_t>((n >> 16) & 0xff);
  bytes[23] = static_cast<uint8_t>((n >> 24) & 0xff);
  if (n != 0) std::memcpy(bytes.data() + 24, payload, n);
  return bytes;
}

std::string test_gray4() {
  const uint8_t payload[] = {0xf0};
  std::vector<uint8_t> bytes = frame_header(2, 1, 1, payload, 1);
  nlink::Image image = nlink::decode_view_frame(bytes.data(), bytes.size());
  if (image.width != 2 || image.height != 1) return "size";
  const uint8_t white[] = {255, 255, 255, 255};
  const uint8_t black[] = {0, 0, 0, 255};
  if (image.rgba.size() != 8 || std::memcmp(image.rgba.data(), white, 4) != 0 ||
      std::memcmp(image.rgba.data() + 4, black, 4) != 0) {
    return "pixels";
  }
  return "";
}

std::string test_rgb565() {
  const uint8_t payload[] = {0x00, 0xf8};
  std::vector<uint8_t> bytes = frame_header(1, 1, 2, payload, 2);
  nlink::Image image = nlink::decode_view_frame(bytes.data(), bytes.size());
  if (image.width != 1 || image.height != 1) return "size";
  const uint8_t red[] = {255, 0, 0, 255};
  if (image.rgba.size() != 4 || std::memcmp(image.rgba.data(), red, 4) != 0) return "pixel";
  return "";
}

std::string test_short_header() {
  const uint8_t bytes[] = {'N', 'L', 'N', 'K', 'F', 'R', 'M'};
  try {
    nlink::decode_view_frame(bytes, sizeof bytes);
  } catch (const nlink::Error &ex) {
    if (std::string(ex.what()) != "The view stream did not start with a frame header.") return ex.what();
    return "";
  }
  return "expected error";
}

std::string test_bad_length() {
  const uint8_t payload[] = {0x00, 0xf8};
  std::vector<uint8_t> bytes = frame_header(1, 1, 2, payload, 2);
  bytes[20] = 1;
  try {
    nlink::decode_view_frame(bytes.data(), bytes.size());
  } catch (const nlink::Error &ex) {
    if (std::string(ex.what()) != "The view frame length does not match its header.") return ex.what();
    return "";
  }
  return "expected error";
}

std::string test_progress_and_json() {
  nlink::progress_reset(10);
  std::pair<uint64_t, uint64_t> got = nlink::progress_get();
  if (got.first != 0 || got.second != 10) return "reset";
  nlink::progress_set_remaining(4);
  got = nlink::progress_get();
  if (got.first != 6 || got.second != 10) return "set";
  nlink::progress_set_remaining(12);
  got = nlink::progress_get();
  if (got.first != 0 || got.second != 12) return "grow";
  nlink::progress_update(3, 5);
  got = nlink::progress_get();
  if (got.first != 2 || got.second != 5) return "update";
  nlink::progress_finish();
  got = nlink::progress_get();
  if (got.first != 5 || got.second != 5) return "finish";
  nlink::FileInfo file;
  file.path = "A\"B\\C";
  file.is_dir = false;
  file.date = 1;
  file.size = 2;
  std::string json = nlink::file_list_json({file});
  if (json != "[{\"path\":\"A\\\"B\\\\C\",\"isDir\":false,\"date\":1,\"size\":2}]") return json;
  if (nlink::file_ext(0x05) != std::string("8xp") || std::string(nlink::type_name(0x15)) != "AppVar") {
    return "names";
  }
  return "";
}

}  // namespace

int main() {
  run("virtual_tree_splits_ram_and_archive", test_virtual_tree);
  run("round_trip_program", test_round_trip_program);
  run("rejects_bad_checksum", test_rejects_bad_checksum);
  run("ack_has_no_checksum_and_data_packet_does", test_ack_packet);
  run("scripted_ready_names_the_83_plus", test_scripted_ready);
  run("scripted_backup_writes_three_sections", test_scripted_backup);
  run("directory_blob_lists_a_program", test_directory_blob);
  run("parses_moodys_variable_header", test_moody_header);
  run("clock_epoch_is_new_year_1997", test_clock);
  run("ce_buffer_is_clamped", test_ce_buffer);
  run("handshake_ping_matches_published_bytes", test_dusb_handshake);
  run("mono_screen_is_96_by_64", test_mono_screen);
  run("scripted_dump_expands_repeated_blocks", test_rom_dump);
  run("decodes_gray4_high_nibble_first", test_gray4);
  run("decodes_rgb565_red", test_rgb565);
  run("rejects_a_short_header", test_short_header);
  run("rejects_a_length_that_does_not_match", test_bad_length);
  run("progress_and_json", test_progress_and_json);
  return g_fails == 0 ? 0 : 1;
}
