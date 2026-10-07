// goliath/ui/name_crc.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstdint>

namespace eot::ui {

constexpr uint32_t NameCrc(const char *name) {
  uint32_t crc = 0xFFFFFFFFu;
  for (; *name; ++name) {
    char c = *name;
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 32);
    crc ^= static_cast<uint8_t>(c);
    for (int k = 0; k < 8; ++k)
      crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
  }
  return ~crc;
}

static_assert(NameCrc("TCRWindow_Choice") == 0x2CA86B19u, "BUCRC must match the retail headers");

}
