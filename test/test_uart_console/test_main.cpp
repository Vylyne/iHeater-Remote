#include <string.h>
#include <unity.h>

#include <string>

extern "C" {
#include "modbus_rtu.h"
}
#include "UartConsole.h"
#include "UartFrames.h"
#include "UartRegisters.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

#define ARROW "\xE2\x86\x92"
#define ENDASH "\xE2\x80\x93"
#define EMDASH "\xE2\x80\x94"
#define TIMES "\xC3\x97"
#define DEGREE "\xC2\xB0"

namespace {

ConsoleRecord rec() { return ConsoleRecord(); }

void addCrc(uint8_t *f, size_t len) {  // len excludes the CRC; low byte first
  uint16_t c = modbus_crc16(f, len);
  f[len] = (uint8_t)(c & 0xFF);
  f[len + 1] = (uint8_t)(c >> 8);
}

std::string describe(const uint8_t *req, size_t reqLen, const uint8_t *reply, size_t replyLen,
                     ConsoleResult res, uint8_t code = 0) {
  ConsoleRecord r;
  makeConsoleRecord(r, req, reqLen, reply, replyLen, res, code, 5);
  char out[256];
  describeExchange(r, out, sizeof out);
  return out;
}

bool endsWith(const std::string &s, const char *t) {
  size_t n = strlen(t);
  return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}
bool startsWith(const std::string &s, const char *t) { return s.rfind(t, 0) == 0; }

// Ok reply to an FC04 read of 22 registers at 0.
size_t inputsReply(uint8_t *f, int16_t air, int16_t heater, uint16_t pwm) {
  uint16_t regs[22] = {};
  regs[0x00] = 1;  // proto
  regs[0x09] = (uint16_t)air;
  regs[0x0A] = (uint16_t)heater;
  regs[0x0E] = pwm;
  f[0] = 1;
  f[1] = 4;
  f[2] = 44;
  for (int i = 0; i < 22; i++) {
    f[3 + 2 * i] = (uint8_t)(regs[i] >> 8);
    f[4 + 2 * i] = (uint8_t)(regs[i] & 0xFF);
  }
  addCrc(f, 47);
  return 49;
}

const uint8_t kWriteOk[8] = {1, 6, 0, 0, 0, 0, 0, 0};

}  // namespace

void test_ring_seq_and_order() {
  ConsoleRecord st[kConsoleLines], out[10];
  ConsoleRing ring(st, kConsoleLines);
  for (int i = 0; i < 3; i++) ring.push(rec());
  uint32_t dropped = 99;
  TEST_ASSERT_EQUAL_UINT32(3, ring.drain(out, 10, dropped));
  TEST_ASSERT_EQUAL_UINT32(0, dropped);
  TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(2, out[1].seq);
  TEST_ASSERT_EQUAL_UINT32(3, out[2].seq);
  TEST_ASSERT_EQUAL_UINT32(0, ring.drain(out, 10, dropped));
  TEST_ASSERT_EQUAL_UINT32(0, dropped);
}

void test_ring_overflow_reports_dropped() {
  static ConsoleRecord st[kConsoleLines], out[200];
  ConsoleRing ring(st, kConsoleLines);
  for (int i = 0; i < 130; i++) ring.push(rec());
  uint32_t dropped = 0;
  TEST_ASSERT_EQUAL_UINT32(100, ring.drain(out, 200, dropped));
  TEST_ASSERT_EQUAL_UINT32(30, dropped);
  TEST_ASSERT_EQUAL_UINT32(31, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(130, out[99].seq);
}

void test_ring_drain_in_batches() {
  ConsoleRecord st[kConsoleLines], out[8];
  ConsoleRing ring(st, kConsoleLines);
  for (int i = 0; i < 20; i++) ring.push(rec());
  uint32_t dropped = 0, next = 1;
  const size_t want[3] = {8, 8, 4};
  for (size_t b = 0; b < 3; b++) {
    TEST_ASSERT_EQUAL_UINT32(want[b], ring.drain(out, 8, dropped));
    TEST_ASSERT_EQUAL_UINT32(0, dropped);
    for (size_t i = 0; i < want[b]; i++) TEST_ASSERT_EQUAL_UINT32(next++, out[i].seq);
  }
}

void test_new_ring_starts_empty() {
  ConsoleRecord st[kConsoleLines], out[4];
  {
    ConsoleRing a(st, kConsoleLines);
    for (int i = 0; i < 5; i++) a.push(rec());
  }
  ConsoleRing b(st, kConsoleLines);
  uint32_t dropped = 0;
  TEST_ASSERT_EQUAL_UINT32(0, b.drain(out, 4, dropped));
  b.push(rec());
  TEST_ASSERT_EQUAL_UINT32(1, b.drain(out, 4, dropped));
  TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
}

void test_record_cuts_bytes() {
  uint8_t req[137], reply[70];
  for (size_t i = 0; i < sizeof req; i++) req[i] = (uint8_t)i;
  for (size_t i = 0; i < sizeof reply; i++) reply[i] = (uint8_t)(i + 1);
  ConsoleRecord r;
  makeConsoleRecord(r, req, sizeof req, reply, sizeof reply, ConsoleResult::Ok, 0, 7);
  TEST_ASSERT_EQUAL_UINT16(137, r.reqLen);
  TEST_ASSERT_EQUAL_UINT16(70, r.replyLen);
  TEST_ASSERT_EQUAL_UINT32(7, r.ms);
  TEST_ASSERT_EQUAL_MEMORY(req, r.req, 32);
  TEST_ASSERT_EQUAL_MEMORY(reply, r.reply, 64);
}

void test_describe_read_inputs() {
  uint8_t req[8], reply[64];
  buildReadRequest(kFcReadInput, 0, 22, req);
  size_t n = inputsReply(reply, 452, 801, 350);
  TEST_ASSERT_EQUAL_STRING("read inputs 0x0000" ENDASH "0x0015 " ARROW
                           " proto 1, air 45.2 " DEGREE "C, heater 80.1 " DEGREE
                           "C, PWM 35%, fault none",
                           describe(req, 8, reply, n, ConsoleResult::Ok).c_str());
}

void test_describe_no_reading() {
  uint8_t req[8], reply[64];
  buildReadRequest(kFcReadInput, 0, 22, req);
  size_t n = inputsReply(reply, kNoReading, 801, 350);
  std::string s = describe(req, 8, reply, n, ConsoleResult::Ok);
  TEST_ASSERT_TRUE(s.find("air " EMDASH ",") != std::string::npos);
}

void test_describe_enter_bootloader() {
  uint8_t req[8];
  buildWriteSingle(kRegBootloader, kBootloaderMagic, req);
  TEST_ASSERT_EQUAL_STRING("enter bootloader " ARROW " ok",
                           describe(req, 8, kWriteOk, 8, ConsoleResult::Ok).c_str());
}

void test_describe_erase_and_boot() {
  uint8_t req[8];
  buildWriteSingle(kRegCommand, 1, req);
  TEST_ASSERT_EQUAL_STRING("erase " ARROW " ok",
                           describe(req, 8, kWriteOk, 8, ConsoleResult::Ok).c_str());
  buildWriteSingle(kRegCommand, 2, req);
  TEST_ASSERT_EQUAL_STRING("boot " ARROW " ok",
                           describe(req, 8, kWriteOk, 8, ConsoleResult::Ok).c_str());
}

void test_describe_chunk() {
  uint16_t vals[3] = {12, 0x1234, 0x5678};
  uint8_t frame[32];
  size_t n = buildWriteMultiple(kRegChunk, vals, 3, frame, sizeof frame);
  TEST_ASSERT_EQUAL_UINT32(15, n);
  std::string s = describe(frame, n, kWriteOk, 8, ConsoleResult::Ok);
  TEST_ASSERT_TRUE(startsWith(s, "chunk 12"));
  TEST_ASSERT_TRUE(endsWith(s, ARROW " ok"));
}

void test_describe_write_target() {
  uint8_t req[8];
  buildWriteSingle(kRegTarget, 450, req);
  TEST_ASSERT_EQUAL_STRING("write 0x0100 = 450 " ARROW " ok",
                           describe(req, 8, kWriteOk, 8, ConsoleResult::Ok).c_str());
}

void test_describe_write_multiple_other() {
  uint16_t vals[2] = {1, 2};
  uint8_t frame[16];
  size_t n = buildWriteMultiple(0x0200, vals, 2, frame, sizeof frame);
  TEST_ASSERT_TRUE(startsWith(describe(frame, n, kWriteOk, 8, ConsoleResult::Ok),
                              "write 0x0200 " TIMES "2 "));
}

void test_describe_exception() {
  uint8_t req[8];
  buildReadRequest(kFcReadInput, 0, 22, req);
  uint8_t reply[5] = {1, 0x84, 2, 0, 0};
  addCrc(reply, 3);
  TEST_ASSERT_TRUE(endsWith(describe(req, 8, reply, 5, ConsoleResult::Exception, 2),
                            ARROW " exception 0x02"));
}

void test_describe_timeout() {
  uint8_t req[8];
  buildReadRequest(kFcReadInput, 0, 22, req);
  TEST_ASSERT_TRUE(
      endsWith(describe(req, 8, nullptr, 0, ConsoleResult::Timeout), ARROW " no reply"));
}

void test_describe_bad_crc() {
  uint8_t req[8];
  buildReadRequest(kFcReadInput, 0, 2, req);
  uint8_t reply[9] = {1, 4, 4, 0, 1, 0, 2, 0, 0};
  addCrc(reply, 7);
  reply[8] ^= 0xFF;
  TEST_ASSERT_TRUE(
      endsWith(describe(req, 8, reply, 9, ConsoleResult::BadReply), ARROW " bad CRC"));
}

void test_describe_too_long() {
  uint8_t req[8], reply[64] = {};
  buildReadRequest(kFcReadInput, 0, 22, req);
  ConsoleRecord r;
  makeConsoleRecord(r, req, 8, reply, 90, ConsoleResult::BadReply, 0, 1);
  char out[200];
  describeExchange(r, out, sizeof out);
  TEST_ASSERT_TRUE(endsWith(out, ARROW " reply too long"));
}

void test_describe_bad_reply_mismatch() {
  uint8_t req[8];
  buildReadRequest(kFcReadInput, 0, 2, req);
  uint8_t reply[7] = {2, 4, 2, 0, 1, 0, 0};
  addCrc(reply, 5);
  TEST_ASSERT_TRUE(endsWith(describe(req, 8, reply, 7, ConsoleResult::BadReply),
                            ARROW " reply does not match"));
}

void test_describe_other_read() {
  uint8_t req[8];
  buildReadRequest(kFcReadHolding, 0x0200, 2, req);
  uint8_t reply[9] = {1, 3, 4, 0, 1, 0, 2, 0, 0};
  addCrc(reply, 7);
  TEST_ASSERT_EQUAL_STRING("read holding 0x0200" ENDASH "0x0201 " ARROW " ok, 2 registers",
                           describe(req, 8, reply, 9, ConsoleResult::Ok).c_str());
}

void test_describe_unknown_fc() {
  uint8_t req[8] = {1, 0x2B, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_TRUE(
      startsWith(describe(req, 8, kWriteOk, 8, ConsoleResult::Ok), "FC 0x2B at 0x0000"));
}

void test_describe_short_request() {
  uint8_t req[3] = {1, 4, 0};
  TEST_ASSERT_TRUE(startsWith(describe(req, 3, nullptr, 0, ConsoleResult::Timeout),
                              "request of 3 bytes"));
}

void test_describe_cuts_to_buffer() {
  uint8_t req[8];
  buildReadRequest(kFcReadInput, 0, 22, req);
  char out[16];
  ConsoleRecord r;
  makeConsoleRecord(r, req, 8, nullptr, 0, ConsoleResult::Timeout, 0, 1);
  describeExchange(r, out, sizeof out);
  TEST_ASSERT_EQUAL_UINT32(15, strlen(out));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ring_seq_and_order);
  RUN_TEST(test_ring_overflow_reports_dropped);
  RUN_TEST(test_ring_drain_in_batches);
  RUN_TEST(test_new_ring_starts_empty);
  RUN_TEST(test_record_cuts_bytes);
  RUN_TEST(test_describe_read_inputs);
  RUN_TEST(test_describe_no_reading);
  RUN_TEST(test_describe_enter_bootloader);
  RUN_TEST(test_describe_erase_and_boot);
  RUN_TEST(test_describe_chunk);
  RUN_TEST(test_describe_write_target);
  RUN_TEST(test_describe_write_multiple_other);
  RUN_TEST(test_describe_exception);
  RUN_TEST(test_describe_timeout);
  RUN_TEST(test_describe_bad_crc);
  RUN_TEST(test_describe_too_long);
  RUN_TEST(test_describe_bad_reply_mismatch);
  RUN_TEST(test_describe_other_read);
  RUN_TEST(test_describe_unknown_fc);
  RUN_TEST(test_describe_short_request);
  RUN_TEST(test_describe_cuts_to_buffer);
  return UNITY_END();
}
