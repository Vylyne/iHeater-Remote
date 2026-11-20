#pragma once

#include <stddef.h>
#include <stdint.h>

// Modbus RTU master frames for the iHeater UART link. Pure, no Arduino dependency.
namespace iheaterlink::policy {

constexpr uint8_t kSlaveAddr = 1;
constexpr uint8_t kFcReadHolding = 0x03;
constexpr uint8_t kFcReadInput = 0x04;
constexpr uint8_t kFcWriteSingle = 0x06;
constexpr size_t kRequestLen = 8;
constexpr uint8_t kMaxReadRegs = 32;

// 8 on success; 0 when fc is not 0x03/0x04 or count is 0 or > kMaxReadRegs.
size_t buildReadRequest(uint8_t fc, uint16_t addr, uint16_t count, uint8_t out[kRequestLen]);
size_t buildWriteSingle(uint16_t addr, uint16_t value, uint8_t out[kRequestLen]);  // always 8

enum class ReplyKind : uint8_t { Incomplete, Ok, Exception, Invalid };

struct Reply {
  ReplyKind kind = ReplyKind::Incomplete;
  uint8_t exception = 0;  // set for Exception
  uint8_t count = 0;      // registers in regs for an Ok read; 0 for an Ok write
  uint16_t regs[kMaxReadRegs] = {};
};

// Judges buf (everything received so far) against the request that was sent.
Reply parseReply(const uint8_t req[kRequestLen], const uint8_t *buf, size_t len);

}  // namespace iheaterlink::policy
