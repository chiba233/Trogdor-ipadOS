// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct CoachzUSBDataWindow {
  uint64_t first = 0, blocks = 0;
  bool contains(uint64_t at, uint64_t count) const {
    return blocks && count && at >= first && at - first < blocks && count <= blocks - (at - first);
  }
};
static uint32_t coachz_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t coachz_le64(const uint8_t *p) {
  return (uint64_t)coachz_le32(p) | (uint64_t)coachz_le32(p + 4) << 32;
}
static uint32_t coachz_crc32(const uint8_t *p, size_t n) {
  uint32_t crc = ~0u;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return ~crc;
}
// Read is synchronous and copies exactly count * 512 bytes into the supplied buffer.
// A valid header never authorizes a write: both GPT copies and the whole array must agree.
template<class Read> static bool coachz_data_window(uint64_t medium, Read read, CoachzUSBDataWindow &out, uint8_t *work, size_t work_bytes) {
  out = {};
  if (!work || work_bytes < 33792) return false;
  uint8_t *primary = work, *backup = work + 512, *array = work + 1024, *other = work + 17408;
  CoachzUSBDataWindow candidate;
  static const uint8_t kernel[16] = {0x5d,0x2a,0x3a,0xfe,0x32,0x4f,0xa7,0x41,0xb7,0x25,0xac,0xcc,0x32,0x85,0xa3,0x09};
  static const uint8_t apfs[16] = {0xef,0x57,0x34,0x7c,0x00,0x00,0xaa,0x11,0xaa,0x11,0x00,0x30,0x65,0x43,0xec,0xac};
  if (medium < 68 || !read(1, 1, primary) || memcmp(primary, "EFI PART", 8) ||
      coachz_le32(primary + 8) != 0x10000 || coachz_le32(primary + 12) != 92 ||
      coachz_le32(primary + 20) || coachz_le64(primary + 24) != 1 ||
      coachz_le64(primary + 72) != 2 || coachz_le32(primary + 80) != 128 || coachz_le32(primary + 84) != 128) return false;
  uint32_t checksum = coachz_le32(primary + 16);
  memset(primary + 16, 0, 4);
  if (coachz_crc32(primary, 92) != checksum) return false;
  uint64_t alternate = coachz_le64(primary + 32), last = medium - 1, usable = coachz_le64(primary + 48);
  // vboot repairs the backup at the physical end without relocating the primary alternate_lba.
  if (alternate < 67 || alternate > last || coachz_le64(primary + 40) != 34 ||
      usable < 34 || usable > alternate - 33 ||
      !read(2, 32, array) || coachz_crc32(array, 16384) != coachz_le32(primary + 88) ||
      !read(last, 1, backup) || memcmp(backup, "EFI PART", 8)) return false;
  checksum = coachz_le32(backup + 16);
  memset(backup + 16, 0, 4);
  if (coachz_crc32(backup, 92) != checksum || memcmp(primary + 8, backup + 8, 8) ||
      coachz_le32(backup + 20) || coachz_le64(backup + 24) != last || coachz_le64(backup + 32) != 1 ||
      memcmp(primary + 40, backup + 40, 32) || coachz_le64(backup + 72) != last - 32 ||
      memcmp(primary + 80, backup + 80, 12) || !read(last - 32, 32, other) || memcmp(array, other, 16384)) return false;
  uint64_t end = 33;
  for (unsigned i = 0; i < 128; i++) {
    const uint8_t *entry = array + i * 128;
    if (i >= 4) {
      for (unsigned j = 0; j < 128; j++) if (entry[j]) return false;
      continue;
    }
    if (memcmp(entry, i ? apfs : kernel, 16)) return false;
    uint64_t first = coachz_le64(entry + 32), limit = coachz_le64(entry + 40);
    if (first <= end || first < 34 || limit < first || limit > usable) return false;
    end = limit;
    if (i == 3) {
      if (limit - first + 1 != (8ull << 30) / 512 || coachz_le64(entry + 48)) return false;
      candidate = {first, limit - first + 1};
    }
  }
  out = candidate;
  return out.blocks != 0;
}
