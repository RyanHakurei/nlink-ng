#pragma once

#include "nlink_internal.hpp"

namespace nlink {

std::vector<uint8_t> romdump_packet(uint16_t cmd, const uint8_t *data, size_t n);

}  // namespace nlink
