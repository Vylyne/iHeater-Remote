#include <string.h>

#include <unity.h>

#include "UartFrames.h"

extern "C" {
#include "modbus_rtu.h"
}

using namespace iheaterlink::policy;

// Fake slave: input register a reads 0x100 + a for a <= 0x15, else exception 0x02.
// Holding 0x0100 stores its write; other writes return 0x02.
struct Fake {
  uint16_t target = 0;
};

static uint8_t fakeReadInput(void *, uint16_t addr, uint16_t *out) {
  if (addr > 0x15) return MODBUS_EX_ILLEGAL_ADDRESS;
  *out = (uint16_t)(0x100 + addr);
  return 0;
}
static uint8_t fakeReadHolding(void *, uint16_t, uint16_t *) {
  return MODBUS_EX_ILLEGAL_ADDRESS;
}
static uint8_t fakeWriteHolding(void *ctx, uint16_t addr, uint16_t value) {
  if (addr != 0x0100) return MODBUS_EX_ILLEGAL_ADDRESS;
  static_cast<Fake *>(ctx)->target = value;
  return 0;
}

static Fake g_fake;
static ModbusHandlers handlers() {
  return ModbusHandlers{fakeReadInput, fakeReadHolding, fakeWriteHolding, &g_fake};
}

static void setCrc(uint8_t *f, size_t bodyLen) {
  uint16_t c = modbus_crc16(f, bodyLen);
  f[bodyLen] = (uint8_t)(c & 0xFF);
  f[bodyLen + 1] = (uint8_t)(c >> 8);
}

// Runs req through the fake slave; returns the reply length.
static size_t answer(const uint8_t *req, uint8_t *resp, size_t cap) {
  ModbusHandlers h = handlers();
  int n = modbus_handle_frame(req, kRequestLen, resp, cap, &h);
  TEST_ASSERT_TRUE(n > 0);
  return (size_t)n;
}

void setUp() { g_fake.target = 0; }
void tearDown() {}

void test_build_read_vector() {  // classic Modbus example
  uint8_t f[kRequestLen];
  TEST_ASSERT_EQUAL(8, buildReadRequest(kFcReadHolding, 0x0000, 10, f));
  const uint8_t want[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, 8);
}

void test_build_read_rejects() {
  uint8_t f[kRequestLen];
  TEST_ASSERT_EQUAL(0, buildReadRequest(0x06, 0, 1, f));
  TEST_ASSERT_EQUAL(0, buildReadRequest(kFcReadInput, 0, 0, f));
  TEST_ASSERT_EQUAL(0, buildReadRequest(kFcReadInput, 0, 33, f));
}

void test_loopback_read_inputs() {
  uint8_t req[kRequestLen], resp[64];
  TEST_ASSERT_EQUAL(8, buildReadRequest(kFcReadInput, 0x0000, 22, req));
  size_t n = answer(req, resp, sizeof resp);
  TEST_ASSERT_EQUAL(49, n);
  Reply r = parseReply(req, resp, n);
  TEST_ASSERT_EQUAL((int)ReplyKind::Ok, (int)r.kind);
  TEST_ASSERT_EQUAL(22, r.count);
  for (int i = 0; i < 22; i++) TEST_ASSERT_EQUAL_HEX16(0x100 + i, r.regs[i]);
}

void test_loopback_exception() {
  uint8_t req[kRequestLen], resp[64];
  TEST_ASSERT_EQUAL(8, buildReadRequest(kFcReadInput, 0x0030, 1, req));
  size_t n = answer(req, resp, sizeof resp);
  Reply r = parseReply(req, resp, n);
  TEST_ASSERT_EQUAL((int)ReplyKind::Exception, (int)r.kind);
  TEST_ASSERT_EQUAL_HEX8(0x02, r.exception);
}

void test_loopback_write_target() {
  uint8_t req[kRequestLen], resp[64];
  TEST_ASSERT_EQUAL(8, buildWriteSingle(0x0100, 600, req));
  size_t n = answer(req, resp, sizeof resp);
  Reply r = parseReply(req, resp, n);
  TEST_ASSERT_EQUAL((int)ReplyKind::Ok, (int)r.kind);
  TEST_ASSERT_EQUAL(0, r.count);
  TEST_ASSERT_EQUAL_HEX16(600, g_fake.target);
}

void test_parse_incomplete_prefix() {
  uint8_t req[kRequestLen], resp[64];
  buildReadRequest(kFcReadInput, 0x0000, 22, req);
  size_t n = answer(req, resp, sizeof resp);
  TEST_ASSERT_EQUAL((int)ReplyKind::Incomplete, (int)parseReply(req, resp, 4).kind);
  TEST_ASSERT_EQUAL((int)ReplyKind::Incomplete, (int)parseReply(req, resp, 1).kind);
  TEST_ASSERT_EQUAL((int)ReplyKind::Incomplete, (int)parseReply(req, resp, 0).kind);
  TEST_ASSERT_EQUAL((int)ReplyKind::Incomplete, (int)parseReply(req, resp, n - 1).kind);
}

void test_parse_trailing_byte_invalid() {
  uint8_t req[kRequestLen], resp[64];
  buildReadRequest(kFcReadInput, 0x0000, 22, req);
  size_t n = answer(req, resp, sizeof resp);
  resp[n] = 0x00;
  TEST_ASSERT_EQUAL((int)ReplyKind::Invalid, (int)parseReply(req, resp, n + 1).kind);
}

void test_parse_bad_crc_invalid() {
  uint8_t req[kRequestLen], resp[64];
  buildReadRequest(kFcReadInput, 0x0000, 22, req);
  size_t n = answer(req, resp, sizeof resp);
  resp[n - 1] ^= 0x01;
  TEST_ASSERT_EQUAL((int)ReplyKind::Invalid, (int)parseReply(req, resp, n).kind);
}

void test_parse_wrong_address_invalid() {
  uint8_t req[kRequestLen], resp[64];
  buildReadRequest(kFcReadInput, 0x0000, 22, req);
  size_t n = answer(req, resp, sizeof resp);
  resp[0] = 0x02;
  setCrc(resp, n - 2);
  TEST_ASSERT_EQUAL((int)ReplyKind::Invalid, (int)parseReply(req, resp, n).kind);
}

void test_parse_byte_count_mismatch() {
  uint8_t req[kRequestLen], resp[64];
  buildReadRequest(kFcReadInput, 0x0000, 22, req);
  size_t n = answer(req, resp, sizeof resp);
  resp[2] = 42;
  setCrc(resp, n - 2);
  TEST_ASSERT_EQUAL((int)ReplyKind::Invalid, (int)parseReply(req, resp, n).kind);
}

void test_parse_write_echo_mismatch() {
  uint8_t req[kRequestLen], resp[64];
  buildWriteSingle(0x0100, 600, req);
  size_t n = answer(req, resp, sizeof resp);
  TEST_ASSERT_EQUAL(8, n);
  resp[4] = 601 >> 8;
  resp[5] = 601 & 0xFF;
  setCrc(resp, 6);
  TEST_ASSERT_EQUAL((int)ReplyKind::Invalid, (int)parseReply(req, resp, n).kind);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_build_read_vector);
  RUN_TEST(test_build_read_rejects);
  RUN_TEST(test_loopback_read_inputs);
  RUN_TEST(test_loopback_exception);
  RUN_TEST(test_loopback_write_target);
  RUN_TEST(test_parse_incomplete_prefix);
  RUN_TEST(test_parse_trailing_byte_invalid);
  RUN_TEST(test_parse_bad_crc_invalid);
  RUN_TEST(test_parse_wrong_address_invalid);
  RUN_TEST(test_parse_byte_count_mismatch);
  RUN_TEST(test_parse_write_echo_mismatch);
  return UNITY_END();
}
