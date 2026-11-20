#include <string.h>

#include <unity.h>

#include "UartFrames.h"
#include "UartPollPolicy.h"
#include "UartRegisters.h"

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

static void fillInputs(uint16_t r[22]) {
  memset(r, 0, 22 * sizeof(uint16_t));
  r[0] = 1;
  r[1] = 0x0102;
  r[3] = 2;
  r[9] = 524;
  r[10] = 1182;
  r[14] = 430;
  r[15] = 1;
  r[16] = 0x0005;
  r[17] = 0x0001;
}

void test_decode_inputs_fields() {
  uint16_t r[22];
  fillInputs(r);
  IheaterSnapshot s{};
  TEST_ASSERT_TRUE(decodeInputs(r, 22, s));
  TEST_ASSERT_EQUAL(1, s.proto);
  TEST_ASSERT_EQUAL(0x0102, s.fwVersion);
  TEST_ASSERT_EQUAL((int)IheaterState::Heating, (int)s.state);
  TEST_ASSERT_EQUAL(524, s.airT10);
  TEST_ASSERT_EQUAL(1182, s.heaterT10);
  TEST_ASSERT_EQUAL(430, s.pwmPermille);
  TEST_ASSERT_TRUE(s.fanOn);
  TEST_ASSERT_EQUAL_UINT32(65541u, s.uptimeS);
}

void test_decode_negative_and_invalid() {
  uint16_t r[22];
  fillInputs(r);
  r[9] = 0xFF9C;
  r[10] = 0x8000;
  IheaterSnapshot s{};
  TEST_ASSERT_TRUE(decodeInputs(r, 22, s));
  TEST_ASSERT_EQUAL(-100, s.airT10);
  TEST_ASSERT_EQUAL(kNoReading, s.heaterT10);
}

void test_decode_wrong_count() {
  uint16_t r[22] = {0};
  IheaterSnapshot s{};
  TEST_ASSERT_FALSE(decodeInputs(r, 21, s));
}

void test_decode_settings() {
  uint16_t r[10] = {850, 900, 450, 850, 60, 5, 400, 350, 600, 100};
  IheaterLimits l{};
  TEST_ASSERT_TRUE(decodeSettings(r, 10, l));
  TEST_ASSERT_EQUAL(450, l.minTargetT10);
  TEST_ASSERT_EQUAL(850, l.maxAirTargetT10);
  TEST_ASSERT_EQUAL(60, l.linkHoldS);
  TEST_ASSERT_EQUAL(100, l.clearMarginT10);
  TEST_ASSERT_FALSE(decodeSettings(r, 9, l));
}

void test_fault_names() {
  TEST_ASSERT_EQUAL_STRING("air over-temperature", faultName(0x07));
  TEST_ASSERT_EQUAL_STRING("unknown fault", faultName(0x42));
}

void test_uart_target_inside() {
  TEST_ASSERT_EQUAL(600, uartTargetT10(60.0f, {45, 85}, 450, 850));
  TEST_ASSERT_EQUAL(624, uartTargetT10(62.36f, {45, 85}, 450, 850));
}

void test_uart_target_clamps() {
  TEST_ASSERT_EQUAL(450, uartTargetT10(40.0f, {45, 85}, 450, 850));
  TEST_ASSERT_EQUAL(850, uartTargetT10(90.0f, {45, 85}, 450, 850));
  TEST_ASSERT_EQUAL(700, uartTargetT10(85.0f, {50, 70}, 450, 850));
}

void test_uart_target_off() {
  TEST_ASSERT_EQUAL(0, uartTargetT10(0.0f, {45, 85}, 450, 850));
  TEST_ASSERT_EQUAL(0, uartTargetT10(-3.0f, {45, 85}, 450, 850));
}

void test_uart_range_empty() {
  RangeT10 r{};
  TEST_ASSERT_FALSE(effectiveRangeT10({11, 40}, 450, 850, r));
  TEST_ASSERT_EQUAL(0, uartTargetT10(30.0f, {11, 40}, 450, 850));
}

// --- UartPollPolicy ---------------------------------------------------------
static Reply inputsReply(uint16_t proto = 1, IheaterState state = IheaterState::Idle,
                         uint32_t uptime = 100, int16_t airTarget = 0, bool clearable = false) {
  Reply r;
  r.kind = ReplyKind::Ok;
  r.count = kInputCount;
  r.regs[0x00] = proto;
  r.regs[0x03] = (uint16_t)state;
  r.regs[0x05] = clearable ? 1 : 0;
  r.regs[0x0C] = (uint16_t)airTarget;
  r.regs[0x10] = (uint16_t)(uptime & 0xFFFF);
  r.regs[0x11] = (uint16_t)(uptime >> 16);
  return r;
}
static Reply settingsReply() {
  Reply r;
  r.kind = ReplyKind::Ok;
  r.count = kSettingsCount;
  r.regs[2] = 450;
  r.regs[3] = 850;
  return r;
}
static Reply exceptionReply(uint8_t code) {
  Reply r;
  r.kind = ReplyKind::Exception;
  r.exception = code;
  return r;
}
static void feedInputs(UartPollPolicy &p, const Reply &r, uint32_t now = 0) {
  p.onReply(p.cycleRead(), r, now);
}
static void feedSettings(UartPollPolicy &p, uint32_t now = 0) {
  p.onReply({PollKind::ReadSettings, kSettingsBase, kSettingsCount}, settingsReply(), now);
}

void test_cycle_read_is_input_block() {
  UartPollPolicy p;
  PollRequest r = p.cycleRead();
  TEST_ASSERT_EQUAL(PollKind::ReadInputs, r.kind);
  TEST_ASSERT_EQUAL_HEX16(0x0000, r.addr);
  TEST_ASSERT_EQUAL(22, r.value);
}

void test_no_followup_before_snapshot() {
  UartPollPolicy p;
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_reads_settings_after_first_snapshot() {
  UartPollPolicy p;
  feedInputs(p, inputsReply());
  PollRequest r = p.followUp(0);
  TEST_ASSERT_EQUAL(PollKind::ReadSettings, r.kind);
  TEST_ASSERT_EQUAL_HEX16(0x0200, r.addr);
  TEST_ASSERT_EQUAL(10, r.value);
}

void test_no_write_before_desired() {
  UartPollPolicy p;
  feedInputs(p, inputsReply());
  feedSettings(p);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_writes_target_when_different() {
  UartPollPolicy p;
  feedInputs(p, inputsReply());
  feedSettings(p);
  p.setDesired(60.0f, {45, 85});
  PollRequest r = p.followUp(0);
  TEST_ASSERT_EQUAL(PollKind::WriteTarget, r.kind);
  TEST_ASSERT_EQUAL_HEX16(0x0100, r.addr);
  TEST_ASSERT_EQUAL(600, r.value);
  feedInputs(p, inputsReply(1, IheaterState::Heating, 101, 600));
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_off_written_before_limits() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(1, IheaterState::Heating, 100, 600));
  p.setDesired(0.0f, {45, 85});
  PollRequest r = p.followUp(0);
  TEST_ASSERT_EQUAL(PollKind::WriteTarget, r.kind);
  TEST_ASSERT_EQUAL(0, r.value);
}

void test_nonzero_waits_for_limits() {
  UartPollPolicy p;
  feedInputs(p, inputsReply());
  p.setDesired(60.0f, {45, 85});
  TEST_ASSERT_EQUAL(PollKind::ReadSettings, p.followUp(0).kind);
}

void test_no_target_write_while_faulted() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(1, IheaterState::Fault, 100, 0, false));
  feedSettings(p);
  p.setDesired(60.0f, {45, 85});
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_target_refusal_not_retried() {
  UartPollPolicy p;
  feedInputs(p, inputsReply());
  feedSettings(p);
  p.setDesired(60.0f, {45, 85});
  PollRequest w = p.followUp(0);
  TEST_ASSERT_EQUAL(600, w.value);
  p.onReply(w, exceptionReply(0x03), 10);
  feedInputs(p, inputsReply(), 200);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(200).kind);
  p.setDesired(65.0f, {45, 85});
  PollRequest w2 = p.followUp(200);
  TEST_ASSERT_EQUAL(PollKind::WriteTarget, w2.kind);
  TEST_ASSERT_EQUAL(650, w2.value);
}

void test_restart_rereads_settings() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(1, IheaterState::Idle, 100));
  feedSettings(p);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
  feedInputs(p, inputsReply(1, IheaterState::Idle, 3), 200);
  TEST_ASSERT_EQUAL(PollKind::ReadSettings, p.followUp(200).kind);
}

void test_proto_mismatch_never_writes() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(2));
  p.setDesired(60.0f, {45, 85});
  TEST_ASSERT_FALSE(p.protocolSupported());
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
  feedSettings(p);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_empty_range_desired_zero() {
  UartPollPolicy p;
  feedInputs(p, inputsReply());
  feedSettings(p);
  p.setDesired(30.0f, {11, 40});
  TEST_ASSERT_EQUAL(0, p.desiredT10());
  TEST_ASSERT_TRUE(p.rangeEmpty());
}

void test_link_status_sequence() {
  UartPollPolicy p;
  TEST_ASSERT_EQUAL(LinkStatus::Waiting, p.link(0));
  feedInputs(p, inputsReply(), 100);
  TEST_ASSERT_EQUAL(LinkStatus::Up, p.link(100));
  p.onNoReply(p.cycleRead(), 300);
  p.onNoReply(p.cycleRead(), 500);
  TEST_ASSERT_EQUAL(LinkStatus::Up, p.link(1000));
  TEST_ASSERT_EQUAL(LinkStatus::NoReply, p.link(1200));
  feedInputs(p, inputsReply(), 1400);
  TEST_ASSERT_EQUAL(LinkStatus::Up, p.link(1400));
}

void test_link_status_wraparound() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(), 0xFFFFFF00u);
  TEST_ASSERT_EQUAL(LinkStatus::Up, p.link(0x00000100u));
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
  RUN_TEST(test_decode_inputs_fields);
  RUN_TEST(test_decode_negative_and_invalid);
  RUN_TEST(test_decode_wrong_count);
  RUN_TEST(test_decode_settings);
  RUN_TEST(test_fault_names);
  RUN_TEST(test_uart_target_inside);
  RUN_TEST(test_uart_target_clamps);
  RUN_TEST(test_uart_target_off);
  RUN_TEST(test_uart_range_empty);
  RUN_TEST(test_cycle_read_is_input_block);
  RUN_TEST(test_no_followup_before_snapshot);
  RUN_TEST(test_reads_settings_after_first_snapshot);
  RUN_TEST(test_no_write_before_desired);
  RUN_TEST(test_writes_target_when_different);
  RUN_TEST(test_off_written_before_limits);
  RUN_TEST(test_nonzero_waits_for_limits);
  RUN_TEST(test_no_target_write_while_faulted);
  RUN_TEST(test_target_refusal_not_retried);
  RUN_TEST(test_restart_rereads_settings);
  RUN_TEST(test_proto_mismatch_never_writes);
  RUN_TEST(test_empty_range_desired_zero);
  RUN_TEST(test_link_status_sequence);
  RUN_TEST(test_link_status_wraparound);
  return UNITY_END();
}
