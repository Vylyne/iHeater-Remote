#pragma once

#include <stddef.h>
#include <stdint.h>

namespace iheaterlink::policy {

// STM32 ROM bootloader (AN3155) frame builders. Pure functions, no I/O.

constexpr uint8_t kRomAck = 0x79, kRomNack = 0x1F, kRomSync = 0x7F;
constexpr uint8_t kRomGet = 0x00, kRomGetId = 0x02, kRomRead = 0x11,
                  kRomGo = 0x21, kRomWrite = 0x31, kRomErase = 0x43,
                  kRomExtErase = 0x44;
constexpr uint16_t kExpectedPid = 0x445;
constexpr size_t kRomChunkBytes = 256;
constexpr uint32_t kRomReplyMs = 1000, kRomEraseMs = 10000;

uint8_t romXor(const uint8_t *data, size_t len);

// {cmd, ~cmd}
void buildRomCommand(uint8_t cmd, uint8_t out[2]);

// Big-endian address followed by the XOR of its four bytes.
void buildRomAddress(uint32_t addr, uint8_t out[5]);

// Erases pages 0..pages-1. Returns the body length, or 0 when pages is 0 or
// above kLastFlashPage + 1, or when cap is too small.
size_t buildEraseBody(uint16_t pages, bool extended, uint8_t *out, size_t cap);

// len-1, the data, then the XOR of all of those. len must be 1..256 and a
// multiple of 4. Returns the body length, or 0 on refusal.
size_t buildWriteBody(const uint8_t *data, size_t len, uint8_t *out, size_t cap);

// {count-1, ~(count-1)} for count 1..256.
void buildReadCount(size_t count, uint8_t out[2]);

}  // namespace iheaterlink::policy
