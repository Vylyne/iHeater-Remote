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

static void fc10Values(uint16_t v[kMaxWriteRegs]) {
  for (int i = 0; i < kMaxWriteRegs; i++) v[i] = (uint16_t)(0x1200 + 0x0101 * i);
}

static void fc10Echo(uint16_t count, uint8_t out[8]) {
  out[0] = 1;
  out[1] = 0x10;
  out[2] = 0x03;
  out[3] = 0x10;
  out[4] = (uint8_t)(count >> 8);
  out[5] = (uint8_t)(count & 0xFF);
  setCrc(out, 6);
}

void test_build_write_multiple_layout() {
  uint16_t v[kMaxWriteRegs];
  fc10Values(v);
  uint8_t f[kMaxFrameLen];
  TEST_ASSERT_EQUAL(139, buildWriteMultiple(0x0310, v, 65, f, sizeof f));
  const uint8_t head[] = {0x01, 0x10, 0x03, 0x10, 0x00, 0x41, 0x82};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(head, f, 7);
  for (int i = 0; i < 65; i++) {
    TEST_ASSERT_EQUAL_HEX8(v[i] >> 8, f[7 + 2 * i]);
    TEST_ASSERT_EQUAL_HEX8(v[i] & 0xFF, f[8 + 2 * i]);
  }
  uint16_t crc = modbus_crc16(f, 137);
  TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, f[137]);
  TEST_ASSERT_EQUAL_HEX8(crc >> 8, f[138]);
}

void test_build_write_multiple_rejects() {
  uint16_t v[kMaxWriteRegs + 1] = {};
  uint8_t f[kMaxFrameLen + 4];
  TEST_ASSERT_EQUAL(0, buildWriteMultiple(0x0310, v, 0, f, sizeof f));
  TEST_ASSERT_EQUAL(0, buildWriteMultiple(0x0310, v, 66, f, sizeof f));
  TEST_ASSERT_EQUAL(0, buildWriteMultiple(0x0310, v, 65, f, 138));
}

void test_parse_write_multiple_ok() {
  uint16_t v[kMaxWriteRegs];
  fc10Values(v);
  uint8_t f[kMaxFrameLen], resp[8];
  buildWriteMultiple(0x0310, v, 65, f, sizeof f);
  fc10Echo(65, resp);
  Reply r = parseReply(f, resp, 8);
  TEST_ASSERT_EQUAL((int)ReplyKind::Ok, (int)r.kind);
  TEST_ASSERT_EQUAL(0, r.count);
  TEST_ASSERT_EQUAL((int)ReplyKind::Incomplete, (int)parseReply(f, resp, 7).kind);
}

void test_parse_write_multiple_wrong_count() {
  uint16_t v[kMaxWriteRegs];
  fc10Values(v);
  uint8_t f[kMaxFrameLen], resp[8];
  buildWriteMultiple(0x0310, v, 65, f, sizeof f);
  fc10Echo(0x40, resp);
  TEST_ASSERT_EQUAL((int)ReplyKind::Invalid, (int)parseReply(f, resp, 8).kind);
}

void test_parse_write_multiple_exception() {
  uint16_t v[kMaxWriteRegs];
  fc10Values(v);
  uint8_t f[kMaxFrameLen];
  buildWriteMultiple(0x0310, v, 65, f, sizeof f);
  uint8_t resp[5] = {0x01, 0x90, 0x04, 0, 0};
  setCrc(resp, 3);
  Reply r = parseReply(f, resp, 5);
  TEST_ASSERT_EQUAL((int)ReplyKind::Exception, (int)r.kind);
  TEST_ASSERT_EQUAL_HEX8(0x04, r.exception);
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
  TEST_ASSERT_EQUAL_STRING("none", faultName(0x00));
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

// --- Automatic fault clearing -----------------------------------------------
static Reply faultInputs(uint8_t fault, bool clearable, uint8_t clearsLeft, uint32_t uptime,
                         uint8_t lastResult = 0, IheaterState state = IheaterState::Fault) {
  Reply r = inputsReply(1, state, uptime, 0, clearable);
  r.regs[0x04] = fault;
  r.regs[0x07] = clearsLeft;
  r.regs[0x08] = lastResult;
  return r;
}
static Reply okWriteReply() {
  Reply r;
  r.kind = ReplyKind::Ok;
  return r;
}
// Policy with settings read and a snapshot in Fault 0x07, clearable, 3 clears left.
static void readyFaulted(UartPollPolicy &p, uint32_t uptime = 100) {
  feedInputs(p, faultInputs(0x07, true, 3, uptime), 0);
  feedSettings(p, 0);
}

void test_reply_after_no_reply_rereads_settings() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(1, IheaterState::Idle, 100), 0);
  feedSettings(p, 0);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
  p.onNoReply(p.cycleRead(), 400);
  p.onNoReply(p.cycleRead(), 800);
  p.onNoReply(p.cycleRead(), 1200);
  TEST_ASSERT_EQUAL(LinkStatus::NoReply, p.link(1200));
  feedInputs(p, inputsReply(1, IheaterState::Idle, 101), 1400);  // no uptime drop
  TEST_ASSERT_FALSE(p.hasLimits());
  TEST_ASSERT_EQUAL(PollKind::ReadSettings, p.followUp(1400).kind);
}

void test_clear_when_clearable() {
  UartPollPolicy p;
  readyFaulted(p);
  PollRequest r = p.followUp(0);
  TEST_ASSERT_EQUAL(PollKind::WriteClear, r.kind);
  TEST_ASSERT_EQUAL_HEX16(0x0101, r.addr);
  TEST_ASSERT_EQUAL(7, r.value);
}

void test_no_clear_when_not_clearable() {
  UartPollPolicy p;
  feedInputs(p, faultInputs(0x07, false, 3, 100), 0);
  feedSettings(p, 0);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_no_clear_without_budget() {
  UartPollPolicy p;
  feedInputs(p, faultInputs(0x07, true, 0, 100), 0);
  feedSettings(p, 0);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_clear_refused_rereads() {
  UartPollPolicy p;
  readyFaulted(p);
  PollRequest r = p.followUp(0);
  p.onReply(r, exceptionReply(0x03), 10);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  feedInputs(p, faultInputs(0x07, true, 3, 101), 200);
  PollRequest again = p.followUp(200);
  TEST_ASSERT_EQUAL(PollKind::WriteClear, again.kind);
  TEST_ASSERT_EQUAL(7, again.value);
}

void test_clear_erase_failure_blocks() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onReply(p.followUp(0), exceptionReply(0x04), 10);
  TEST_ASSERT_EQUAL(ClearPhase::Blocked, p.clearPhase());
  TEST_ASSERT_EQUAL(7, p.blockedFault());
  feedInputs(p, faultInputs(0x07, true, 3, 101), 200);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(200).kind);
  feedInputs(p, faultInputs(0x01, true, 3, 102), 400);
  PollRequest r = p.followUp(400);
  TEST_ASSERT_EQUAL(PollKind::WriteClear, r.kind);
  TEST_ASSERT_EQUAL(1, r.value);
}

void test_clear_success_verified() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onReply(p.followUp(0), okWriteReply(), 10);
  TEST_ASSERT_EQUAL(ClearPhase::AwaitRestart, p.clearPhase());
  p.onNoReply(p.cycleRead(), 1010);
  TEST_ASSERT_EQUAL(LinkStatus::Restarting, p.link(1010));
  p.onNoReply(p.cycleRead(), 5000);
  TEST_ASSERT_EQUAL(LinkStatus::Restarting, p.link(5000));
  feedInputs(p, faultInputs(0, false, 2, 2, 1, IheaterState::Idle), 5400);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_TRUE(p.lastClearOk());
  TEST_ASSERT_EQUAL(PollKind::ReadSettings, p.followUp(5400).kind);
}

void test_lost_clear_reply_then_restart_counts() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onNoReply(p.followUp(0), 10);
  TEST_ASSERT_EQUAL(ClearPhase::AwaitRestart, p.clearPhase());
  feedInputs(p, faultInputs(0, false, 2, 3, 1, IheaterState::Idle), 5400);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_TRUE(p.lastClearOk());
}

void test_lost_clear_reply_without_restart_blocks() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onNoReply(p.followUp(0), 10);
  feedInputs(p, faultInputs(0x07, false, 2, 101), 200);
  TEST_ASSERT_EQUAL(ClearPhase::Blocked, p.clearPhase());
  TEST_ASSERT_EQUAL(7, p.blockedFault());
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(200).kind);
}

void test_clear_never_landed_retries() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onNoReply(p.followUp(0), 10);
  feedInputs(p, faultInputs(0x07, true, 3, 101), 200);
  TEST_ASSERT_EQUAL(ClearPhase::AwaitRestart, p.clearPhase());
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(200).kind);
  for (uint32_t t = 1100; t < 10000; t += 800) {  // link stays up, nothing changes
    feedInputs(p, faultInputs(0x07, true, 3, 100 + t / 1000), t);
    TEST_ASSERT_EQUAL(ClearPhase::AwaitRestart, p.clearPhase());
  }
  feedInputs(p, faultInputs(0x07, true, 3, 111), 10500);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  PollRequest r = p.followUp(10500);
  TEST_ASSERT_EQUAL(PollKind::WriteClear, r.kind);
  TEST_ASSERT_EQUAL(7, r.value);
}

void test_lost_clear_reply_other_state_times_out() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onNoReply(p.followUp(0), 10);
  feedInputs(p, faultInputs(0, false, 2, 101, 0, IheaterState::Idle), 200);
  TEST_ASSERT_EQUAL(ClearPhase::AwaitRestart, p.clearPhase());
  for (uint32_t t = 1000; t < 10000; t += 800) {
    feedInputs(p, faultInputs(0, false, 2, 100 + t / 1000, 0, IheaterState::Idle), t);
    TEST_ASSERT_EQUAL(ClearPhase::AwaitRestart, p.clearPhase());
  }
  feedInputs(p, faultInputs(0, false, 2, 111, 0, IheaterState::Idle), 10500);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_FALSE(p.lastClearOk());
}

void test_restart_result_one_without_spent_clear_not_ok() {
  UartPollPolicy p;
  readyFaulted(p);
  p.onReply(p.followUp(0), okWriteReply(), 10);
  feedInputs(p, faultInputs(0, false, 3, 2, 1, IheaterState::Idle), 5400);  // still 3 left
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_FALSE(p.lastClearOk());
}

// A real restart is silent for ~5 s, and the clear goes out after ~5 s of safe
// readings, so the first post-restart uptime is at or above the uptime at send.
void test_clear_success_verified_without_uptime_drop() {
  UartPollPolicy p;
  readyFaulted(p, 5);
  p.onReply(p.followUp(0), okWriteReply(), 10);
  p.onNoReply(p.cycleRead(), 1010);
  p.onNoReply(p.cycleRead(), 5000);
  feedInputs(p, faultInputs(0, false, 2, 6, 1, IheaterState::Idle), 5400);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_TRUE(p.lastClearOk());
}

void test_clear_success_verified_equal_uptime() {
  UartPollPolicy p;
  readyFaulted(p, 5);
  p.onReply(p.followUp(0), okWriteReply(), 10);
  p.onNoReply(p.cycleRead(), 1010);
  feedInputs(p, faultInputs(0, false, 2, 5, 1, IheaterState::Idle), 5400);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_TRUE(p.lastClearOk());
}

void test_lost_clear_reply_quiet_then_restart_counts() {
  UartPollPolicy p;
  readyFaulted(p, 5);
  p.onNoReply(p.followUp(0), 10);
  p.onNoReply(p.cycleRead(), 400);
  p.onNoReply(p.cycleRead(), 800);
  p.onNoReply(p.cycleRead(), 1200);
  feedInputs(p, faultInputs(0, false, 2, 6, 1, IheaterState::Idle), 5400);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_TRUE(p.lastClearOk());
}

void test_quiet_return_without_spent_clear_not_ok() {
  UartPollPolicy p;
  readyFaulted(p, 5);
  p.onReply(p.followUp(0), okWriteReply(), 10);
  p.onNoReply(p.cycleRead(), 1010);
  feedInputs(p, faultInputs(0, false, 3, 6, 1, IheaterState::Idle), 5400);  // still 3 left
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_FALSE(p.lastClearOk());
}

void test_quiet_return_without_result_one_not_ok() {
  UartPollPolicy p;
  readyFaulted(p, 5);
  p.onReply(p.followUp(0), okWriteReply(), 10);
  p.onNoReply(p.cycleRead(), 1010);
  feedInputs(p, faultInputs(0, false, 2, 6, 0, IheaterState::Idle), 5400);
  TEST_ASSERT_EQUAL(ClearPhase::Idle, p.clearPhase());
  TEST_ASSERT_FALSE(p.lastClearOk());
}

static Reply bootloaderReply() {
  Reply r;
  r.kind = ReplyKind::Ok;
  r.count = kInputCount;
  for (int i = 0; i < kInputCount; i++) r.regs[i] = 0;
  r.regs[0] = 0x8001;
  r.regs[3] = 2;
  r.regs[4] = 4;
  r.regs[6] = 30;
  return r;
}

void test_bootloader_detected() {
  UartPollPolicy p;
  feedInputs(p, bootloaderReply(), 100);
  TEST_ASSERT_TRUE(p.inBootloader());
  TEST_ASSERT_EQUAL(2, p.bootloader().why);
  TEST_ASSERT_EQUAL(4, p.bootloader().appCheck);
  TEST_ASSERT_EQUAL(30, p.bootloader().timeoutS);
  TEST_ASSERT_EQUAL(LinkStatus::Updating, p.link(100));
}

void test_bootloader_drops_snapshot() {
  UartPollPolicy p;
  feedInputs(p, inputsReply(), 0);
  TEST_ASSERT_TRUE(p.hasSnapshot());
  feedInputs(p, bootloaderReply(), 200);
  TEST_ASSERT_FALSE(p.hasSnapshot());
  TEST_ASSERT_FALSE(p.protocolSupported());
}

void test_bootloader_holds_writes() {
  UartPollPolicy p;
  p.setDesired(60.0f, {11, 90});
  feedInputs(p, bootloaderReply(), 0);
  TEST_ASSERT_EQUAL(PollKind::None, p.followUp(0).kind);
}

void test_bootloader_left_on_app_read() {
  UartPollPolicy p;
  feedInputs(p, bootloaderReply(), 0);
  feedInputs(p, inputsReply(), 200);
  TEST_ASSERT_FALSE(p.inBootloader());
  TEST_ASSERT_EQUAL(LinkStatus::Up, p.link(200));
}

void test_bootloader_silent_ages_to_no_reply() {
  UartPollPolicy p;
  feedInputs(p, bootloaderReply(), 0);
  p.onNoReply(p.cycleRead(), kNoReplyAfterMs);
  TEST_ASSERT_EQUAL(LinkStatus::NoReply, p.link(kNoReplyAfterMs));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_bootloader_detected);
  RUN_TEST(test_bootloader_drops_snapshot);
  RUN_TEST(test_bootloader_holds_writes);
  RUN_TEST(test_bootloader_left_on_app_read);
  RUN_TEST(test_bootloader_silent_ages_to_no_reply);
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
  RUN_TEST(test_build_write_multiple_layout);
  RUN_TEST(test_build_write_multiple_rejects);
  RUN_TEST(test_parse_write_multiple_ok);
  RUN_TEST(test_parse_write_multiple_wrong_count);
  RUN_TEST(test_parse_write_multiple_exception);
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
  RUN_TEST(test_reply_after_no_reply_rereads_settings);
  RUN_TEST(test_clear_when_clearable);
  RUN_TEST(test_no_clear_when_not_clearable);
  RUN_TEST(test_no_clear_without_budget);
  RUN_TEST(test_clear_refused_rereads);
  RUN_TEST(test_clear_erase_failure_blocks);
  RUN_TEST(test_clear_success_verified);
  RUN_TEST(test_lost_clear_reply_then_restart_counts);
  RUN_TEST(test_lost_clear_reply_without_restart_blocks);
  RUN_TEST(test_clear_never_landed_retries);
  RUN_TEST(test_lost_clear_reply_other_state_times_out);
  RUN_TEST(test_restart_result_one_without_spent_clear_not_ok);
  RUN_TEST(test_clear_success_verified_without_uptime_drop);
  RUN_TEST(test_clear_success_verified_equal_uptime);
  RUN_TEST(test_lost_clear_reply_quiet_then_restart_counts);
  RUN_TEST(test_quiet_return_without_spent_clear_not_ok);
  RUN_TEST(test_quiet_return_without_result_one_not_ok);
  return UNITY_END();
}
