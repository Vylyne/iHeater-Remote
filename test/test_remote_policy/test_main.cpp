#include <unity.h>

#include "RemotePolicy.h"
#include "ReportPolicy.h"

extern "C" {
#include "modbus_rtu.h"
}

using namespace iheaterlink::policy;

static const TargetRange R{45, 90};

void test_valid_range() {
  TEST_ASSERT_TRUE(validRange(45, 90));
  TEST_ASSERT_TRUE(validRange(11, 11));
  TEST_ASSERT_FALSE(validRange(10, 90));
  TEST_ASSERT_FALSE(validRange(60, 50));
  TEST_ASSERT_FALSE(validRange(45, 91));
}

void test_clamp_off() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, clampTargetC(0.0f, R));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, clampTargetC(-5.0f, R));
}

void test_clamp_low_to_min() {
  TEST_ASSERT_EQUAL_FLOAT(45.0f, clampTargetC(40.0f, R));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, clampTargetC(40.0f, TargetRange{50, 90}));
}

void test_clamp_passthrough() {
  TEST_ASSERT_EQUAL_FLOAT(60.0f, clampTargetC(60.0f, R));
  TEST_ASSERT_EQUAL_FLOAT(85.0f, clampTargetC(85.0f, R));
}

void test_clamp_above_max() {
  TEST_ASSERT_EQUAL_FLOAT(90.0f, clampTargetC(95.0f, R));
  TEST_ASSERT_EQUAL_FLOAT(70.0f, clampTargetC(85.0f, TargetRange{45, 70}));
}

void test_wifi_connected_none() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(false, true, true, 100000, 0) == WifiAction::None);
}

void test_wifi_sta_reconnect_due() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(false, true, false, 30000, 0) == WifiAction::Reconnect);
}

void test_wifi_sta_reconnect_not_due() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(false, true, false, 29999, 0) == WifiAction::None);
}

void test_wifi_ap_retry_due() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(true, true, false, 120000, 0) == WifiAction::RetryFromAp);
}

void test_wifi_ap_retry_not_due() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(true, true, false, 119999, 0) == WifiAction::None);
}

void test_wifi_ap_no_credentials() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(true, false, false, 500000, 0) == WifiAction::None);
}

void test_wifi_wraparound() {
  TEST_ASSERT_TRUE(wifiSupervisorAction(false, true, false, 5, 0xFFFFFFF0u) == WifiAction::None);
  TEST_ASSERT_TRUE(wifiSupervisorAction(false, true, false, 30000, 0xFFFFFFF0u) ==
                   WifiAction::Reconnect);
}

void test_link_loss_within_grace() {
  TEST_ASSERT_FALSE(cutHeatForLinkLoss(true, true, false, 599999, 0, 600000));
}

void test_link_loss_after_grace() {
  TEST_ASSERT_TRUE(cutHeatForLinkLoss(true, true, false, 600000, 0, 600000));
}

void test_link_loss_healthy() {
  TEST_ASSERT_FALSE(cutHeatForLinkLoss(true, true, true, 10000000, 0, 600000));
}

void test_link_loss_not_heating() {
  TEST_ASSERT_FALSE(cutHeatForLinkLoss(true, false, false, 10000000, 0, 600000));
}

void test_link_loss_manual() {
  TEST_ASSERT_FALSE(cutHeatForLinkLoss(false, true, false, 10000000, 0, 600000));
}

void test_link_loss_wraparound() {
  TEST_ASSERT_FALSE(cutHeatForLinkLoss(true, true, false, 5, 0xFFFFFFF0u, 600000));
}

void test_tx_power_valid() {
  const int valid[] = {34, 44, 52, 60, 68, 78};
  for (int v : valid) TEST_ASSERT_TRUE(validTxPower(v));
  const int invalid[] = {0, 33, 80, 84};
  for (int v : invalid) TEST_ASSERT_FALSE(validTxPower(v));
}

void test_token_normalize() {
  char out[kPairTokenLen + 1];
  TEST_ASSERT_TRUE(normalizePairToken("7k3p-9qxm", out));
  TEST_ASSERT_EQUAL_STRING("7K3P9QXM", out);
  TEST_ASSERT_TRUE(normalizePairToken("--7K3P9QXM", out));
  TEST_ASSERT_EQUAL_STRING("7K3P9QXM", out);
  TEST_ASSERT_FALSE(normalizePairToken("7K3P-9QX", out));
  TEST_ASSERT_FALSE(normalizePairToken("7K3P9QXMM", out));
  TEST_ASSERT_FALSE(normalizePairToken("7K3P9QXI", out));
  TEST_ASSERT_FALSE(normalizePairToken("7K3P9QXU", out));
  TEST_ASSERT_FALSE(normalizePairToken("7K3P 9QXM", out));
  TEST_ASSERT_FALSE(normalizePairToken("", out));
  TEST_ASSERT_FALSE(normalizePairToken(nullptr, out));
}

void test_token_from_bits() {
  char out[kPairTokenLen + 1];
  pairTokenFromBits(0, out);
  TEST_ASSERT_EQUAL_STRING("00000000", out);
  pairTokenFromBits(0xFFFFFFFFFFull, out);
  TEST_ASSERT_EQUAL_STRING("ZZZZZZZZ", out);
  pairTokenFromBits(1, out);
  TEST_ASSERT_EQUAL_STRING("00000001", out);
  pairTokenFromBits(31ull << 35, out);
  TEST_ASSERT_EQUAL_STRING("Z0000000", out);
  pairTokenFromBits((1ull << 40) | 1, out);
  TEST_ASSERT_EQUAL_STRING("00000001", out);
}

void test_token_format() {
  char out[kPairTokenLen + 2];
  formatPairToken("7K3P9QXM", out);
  TEST_ASSERT_EQUAL_STRING("7K3P-9QXM", out);
}

void test_heater_object_name() {
  TEST_ASSERT_TRUE(isHeaterVirtualObject("heater_virtual chamber"));
  TEST_ASSERT_FALSE(isHeaterVirtualObject("heater_virtual"));
  TEST_ASSERT_FALSE(isHeaterVirtualObject("heater_virtual "));
  TEST_ASSERT_FALSE(isHeaterVirtualObject("heater_generic chamber"));
  TEST_ASSERT_FALSE(isHeaterVirtualObject("heater_virtualx a"));
}

void test_match_paired_heater() {
  const char *a[] = {"AAAA0000", "7K3P9QXM"};
  TEST_ASSERT_EQUAL_INT(1, matchPairedHeater(a, 2, "7k3p-9qxm"));
  TEST_ASSERT_EQUAL_INT(kHeaterNone, matchPairedHeater(a, 2, "BBBB1111"));
  const char *d[] = {"7K3P9QXM", "7k3p-9qxm"};
  TEST_ASSERT_EQUAL_INT(kHeaterDuplicate, matchPairedHeater(d, 2, "7K3P9QXM"));
  const char *n[] = {nullptr, "", "7K3P9QXM"};
  TEST_ASSERT_EQUAL_INT(2, matchPairedHeater(n, 3, "7K3P9QXM"));
  TEST_ASSERT_EQUAL_INT(kHeaterNone, matchPairedHeater(a, 2, "bad"));
}

void test_link_healthy() {
  TEST_ASSERT_TRUE(moonrakerLinkHealthy(true, true, KlippyState::Ready));
  TEST_ASSERT_FALSE(moonrakerLinkHealthy(false, true, KlippyState::Ready));
  TEST_ASSERT_FALSE(moonrakerLinkHealthy(true, false, KlippyState::Ready));
  TEST_ASSERT_FALSE(moonrakerLinkHealthy(true, true, KlippyState::NotReady));
  TEST_ASSERT_FALSE(moonrakerLinkHealthy(true, true, KlippyState::Unknown));
}

void test_backoff() {
  TEST_ASSERT_EQUAL_UINT32(2000, nextBackoffMs(0));
  TEST_ASSERT_EQUAL_UINT32(4000, nextBackoffMs(2000));
  TEST_ASSERT_EQUAL_UINT32(30000, nextBackoffMs(16000));
  TEST_ASSERT_EQUAL_UINT32(30000, nextBackoffMs(30000));
}

void test_interval_elapsed() {
  TEST_ASSERT_TRUE(intervalElapsed(1000, 0, 1000));
  TEST_ASSERT_FALSE(intervalElapsed(999, 0, 1000));
  TEST_ASSERT_FALSE(intervalElapsed(500, 0xFFFFFF00u, 1000));
  TEST_ASSERT_TRUE(intervalElapsed(744, 0xFFFFFF00u, 1000));
}

void test_default_port() {
  TEST_ASSERT_EQUAL_UINT16(443, defaultMoonrakerPort(true));
  TEST_ASSERT_EQUAL_UINT16(7125, defaultMoonrakerPort(false));
}

void test_keep_stored_api_key() {
  TEST_ASSERT_TRUE(keepStoredApiKey(false, true));    // same endpoint
  TEST_ASSERT_FALSE(keepStoredApiKey(true, true));    // new endpoint
  TEST_ASSERT_FALSE(keepStoredApiKey(false, false));  // new key typed
  TEST_ASSERT_FALSE(keepStoredApiKey(true, false));
}

void test_handshake_hint() {
  TEST_ASSERT_EQUAL(HandshakeHint::ApiKey, handshakeFailureHint(401, 7125, false));
  TEST_ASSERT_EQUAL(HandshakeHint::ApiKey, handshakeFailureHint(403, 443, true));
  TEST_ASSERT_EQUAL(HandshakeHint::TurnTlsOn, handshakeFailureHint(400, 443, false));
  TEST_ASSERT_EQUAL(HandshakeHint::CheckEndpoint, handshakeFailureHint(400, 443, true));
  TEST_ASSERT_EQUAL(HandshakeHint::CheckEndpoint, handshakeFailureHint(400, 7125, false));
  TEST_ASSERT_EQUAL(HandshakeHint::CheckEndpoint, handshakeFailureHint(404, 443, false));
  TEST_ASSERT_EQUAL(HandshakeHint::CheckEndpoint, handshakeFailureHint(502, 443, true));
  TEST_ASSERT_EQUAL(HandshakeHint::CheckEndpoint, handshakeFailureHint(503, 80, false));
  TEST_ASSERT_EQUAL(HandshakeHint::CheckEndpoint, handshakeFailureHint(0, 7125, false));
}

void test_connect_failure_tls_hint() {
  TEST_ASSERT_TRUE(suggestTlsForPort(443, false));
  TEST_ASSERT_FALSE(suggestTlsForPort(443, true));
  TEST_ASSERT_FALSE(suggestTlsForPort(7125, false));
  TEST_ASSERT_FALSE(suggestTlsForPort(80, false));
}

void test_vendored_crc_vector() {
  const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  TEST_ASSERT_EQUAL_HEX16(0xCDC5, modbus_crc16(frame, sizeof frame));  // wire bytes C5 CD
}

static ResumeRecord sealedRec() {
  ResumeRecord r{};
  r.mode = 3;
  r.timerMinutes = 30;
  r.remainingS = 1200;
  sealResume(r);
  return r;
}

void test_resume_sealed_valid() { TEST_ASSERT_TRUE(resumeValid(sealedRec(), true, 7)); }

void test_resume_rejects_power_on() { TEST_ASSERT_FALSE(resumeValid(sealedRec(), false, 7)); }

void test_resume_rejects_zeroed() {
  ResumeRecord r{};
  TEST_ASSERT_FALSE(resumeValid(r, true, 7));
}

void test_resume_rejects_flipped_bit() {
  ResumeRecord r = sealedRec();
  r.remainingS ^= 1;
  TEST_ASSERT_FALSE(resumeValid(r, true, 7));
}

void test_resume_rejects_bad_mode() {
  ResumeRecord r{};
  r.mode = 8;
  r.timerMinutes = 30;
  r.remainingS = 1200;
  sealResume(r);
  TEST_ASSERT_FALSE(resumeValid(r, true, 7));
}

void test_resume_rejects_expired_timer() {
  ResumeRecord r{};
  r.mode = 3;
  r.timerMinutes = 30;
  r.remainingS = 0;
  sealResume(r);
  TEST_ASSERT_FALSE(resumeValid(r, true, 7));
  r.timerMinutes = 0;
  sealResume(r);
  TEST_ASSERT_TRUE(resumeValid(r, true, 7));
}

void test_setup_save_rule() {
  // (uartOutput, wasKlipper, nowKlipper, bindingChanged, outputChanged)
  TEST_ASSERT_FALSE(setupSaveStopsHeat(true, false, false, false, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(false, false, false, false, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(true, true, false, false, false));
  TEST_ASSERT_FALSE(setupSaveStopsHeat(true, true, true, false, false));
  TEST_ASSERT_FALSE(setupSaveStopsHeat(false, true, true, false, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(true, true, true, true, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(false, true, true, true, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(true, false, true, false, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(false, false, true, false, false));
  TEST_ASSERT_TRUE(setupSaveStopsHeat(true, false, false, false, true));
}

// --- Link-loss carry across a software restart -----------------------------
static LinkLossRecord sealedLoss(uint32_t unhealthyMs) {
  LinkLossRecord r{};
  sealLinkLoss(r, unhealthyMs);
  return r;
}

void test_link_loss_carry_continues_accumulated_time() {
  uint32_t carried = 0;
  TEST_ASSERT_TRUE(carriedLinkLoss(sealedLoss(540000), true, carried));
  TEST_ASSERT_EQUAL_UINT32(540000, carried);
  TEST_ASSERT_FALSE(linkLossExpired(carried, kLinkLossGraceMs));
  // Boot counts from millis() 0, so the baseline is 0 - carried: the cut comes
  // after the remaining 60 s, not a fresh 10 minutes.
  const uint32_t since = linkLossBaselineMs(carried);
  TEST_ASSERT_FALSE(cutHeatForLinkLoss(true, true, false, 59000, since, kLinkLossGraceMs));
  TEST_ASSERT_TRUE(cutHeatForLinkLoss(true, true, false, 60000, since, kLinkLossGraceMs));
}

void test_link_loss_carry_already_expired() {
  uint32_t carried = 0;
  TEST_ASSERT_TRUE(carriedLinkLoss(sealedLoss(kLinkLossGraceMs), true, carried));
  TEST_ASSERT_TRUE(linkLossExpired(carried, kLinkLossGraceMs));
  TEST_ASSERT_TRUE(carriedLinkLoss(sealedLoss(kLinkLossGraceMs + 5000), true, carried));
  TEST_ASSERT_TRUE(linkLossExpired(carried, kLinkLossGraceMs));
  TEST_ASSERT_FALSE(linkLossExpired(kLinkLossGraceMs - 1, kLinkLossGraceMs));
}

void test_link_loss_carry_saturates() {
  // A very long outage must not wrap the stored value back below the grace.
  uint32_t carried = 0;
  TEST_ASSERT_TRUE(carriedLinkLoss(sealedLoss(0xFFFFFFF0u), true, carried));
  TEST_ASSERT_TRUE(linkLossExpired(carried, kLinkLossGraceMs));
  const uint32_t since = linkLossBaselineMs(carried);
  TEST_ASSERT_TRUE(cutHeatForLinkLoss(true, true, false, 3000, since, kLinkLossGraceMs));
}

void test_link_loss_carry_rejects_power_on() {
  uint32_t carried = 123;
  TEST_ASSERT_FALSE(carriedLinkLoss(sealedLoss(540000), false, carried));
  TEST_ASSERT_EQUAL_UINT32(0, carried);
}

void test_link_loss_carry_rejects_garbage() {
  uint32_t carried = 123;
  LinkLossRecord zero{};
  TEST_ASSERT_FALSE(carriedLinkLoss(zero, true, carried));
  TEST_ASSERT_EQUAL_UINT32(0, carried);
  LinkLossRecord r = sealedLoss(540000);
  r.unhealthyMs ^= 1;  // bad check
  TEST_ASSERT_FALSE(carriedLinkLoss(r, true, carried));
  r = sealedLoss(540000);
  r.magic ^= 1;
  TEST_ASSERT_FALSE(carriedLinkLoss(r, true, carried));
  r = sealedLoss(540000);
  r.check ^= 0x80000000u;
  TEST_ASSERT_FALSE(carriedLinkLoss(r, true, carried));
}

void test_link_loss_carry_healthy_clears() {
  LinkLossRecord r = sealedLoss(540000);
  clearLinkLoss(r);
  uint32_t carried = 123;
  TEST_ASSERT_FALSE(carriedLinkLoss(r, true, carried));
  TEST_ASSERT_EQUAL_UINT32(0, carried);
}

void test_link_loss_carry_zero_is_valid() {
  // Sealed at boot with nothing accumulated yet: counting restarts from 0.
  uint32_t carried = 123;
  TEST_ASSERT_TRUE(carriedLinkLoss(sealedLoss(0), true, carried));
  TEST_ASSERT_EQUAL_UINT32(0, carried);
  TEST_ASSERT_FALSE(linkLossExpired(carried, kLinkLossGraceMs));
}

void test_link_loss_carry_adds_boot_time() {
  // Time already spent in setup() counts: 540 s carried + 7 s of boot.
  TEST_ASSERT_EQUAL_UINT32(547000, linkLossCarryPlus(540000, 7000));
  TEST_ASSERT_EQUAL_UINT32(7000, linkLossCarryPlus(0, 7000));
  TEST_ASSERT_FALSE(linkLossExpired(linkLossCarryPlus(540000, 7000), kLinkLossGraceMs));
  // Boot time alone can use up the grace.
  TEST_ASSERT_TRUE(linkLossExpired(linkLossCarryPlus(595000, 5000), kLinkLossGraceMs));
  // Saturates at the grace instead of wrapping.
  TEST_ASSERT_EQUAL_UINT32(kLinkLossGraceMs, linkLossCarryPlus(kLinkLossGraceMs, 0xFFFFFFFFu));
  TEST_ASSERT_EQUAL_UINT32(kLinkLossGraceMs, linkLossCarryPlus(0xFFFFFFF0u, 0x20u));
  // The sum stays a valid record value.
  uint32_t carried = 0;
  LinkLossRecord r{};
  sealLinkLoss(r, linkLossCarryPlus(590000, 20000));
  TEST_ASSERT_TRUE(carriedLinkLoss(r, true, carried));
  TEST_ASSERT_TRUE(linkLossExpired(carried, kLinkLossGraceMs));
}

static ReportInputs upIdle() {
  ReportInputs in{};
  in.link = LinkStatus::Up;
  in.hasSnapshot = true;
  in.state = IheaterState::Idle;
  in.airT10 = 215;
  in.heaterT10 = 230;
  in.pwmPermille = 0;
  in.fanOn = false;
  in.fault = 0;
  in.targetT10 = 0;
  return in;
}

void test_link_and_state_names() {
  TEST_ASSERT_EQUAL_STRING("no-reply", linkStatusName(LinkStatus::NoReply));
  TEST_ASSERT_EQUAL_STRING("hold", iheaterStateName(IheaterState::Hold));
  TEST_ASSERT_EQUAL_STRING("unknown", iheaterStateName(static_cast<IheaterState>(9)));
  TEST_ASSERT_EQUAL_STRING("missing", pluginStateName(PluginState::Missing));
}

void test_report_values_null_when_link_not_up() {
  ReportInputs in = upIdle();
  in.link = LinkStatus::Restarting;
  const ReportValues v = reportValues(in);
  TEST_ASSERT_FALSE(v.hasAir);
  TEST_ASSERT_FALSE(v.hasElement);
  TEST_ASSERT_FALSE(v.hasPower);
}

void test_report_values_null_on_no_reading() {
  ReportInputs in = upIdle();
  in.airT10 = kNoReading;
  const ReportValues v = reportValues(in);
  TEST_ASSERT_FALSE(v.hasAir);
  TEST_ASSERT_TRUE(v.hasElement);
}

void test_report_values_null_without_snapshot() {
  ReportInputs in = upIdle();
  in.hasSnapshot = false;
  const ReportValues v = reportValues(in);
  TEST_ASSERT_FALSE(v.hasAir);
  TEST_ASSERT_FALSE(v.hasElement);
  TEST_ASSERT_FALSE(v.hasPower);
}

void test_report_interval_idle_and_active() {
  ReportInputs in = upIdle();
  TEST_ASSERT_EQUAL_UINT32(15000, reportIntervalMs(in));
  TEST_ASSERT_EQUAL_UINT32(15, reportValues(in).nextWithinS);
  in.targetT10 = 600;
  TEST_ASSERT_EQUAL_UINT32(5000, reportIntervalMs(in));
  in = upIdle();
  in.state = IheaterState::Heating;
  TEST_ASSERT_EQUAL_UINT32(5000, reportIntervalMs(in));
  in.state = IheaterState::Hold;
  TEST_ASSERT_EQUAL_UINT32(5000, reportIntervalMs(in));
}

void test_report_first_send_due() {
  ReportScheduler s;
  TEST_ASSERT_TRUE(s.due(upIdle(), 0));
}

void test_report_not_due_while_in_flight() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0);
  TEST_ASSERT_FALSE(s.due(in, 20000));
  s.onAccepted();
  TEST_ASSERT_TRUE(s.due(in, 20000));
}

void test_report_change_waits_min_gap() {
  ReportScheduler s;
  ReportInputs in = upIdle();
  s.onSent(in, 1000);
  s.onAccepted();
  in.airT10 = 216;
  TEST_ASSERT_FALSE(s.due(in, 1999));
  TEST_ASSERT_TRUE(s.due(in, 2000));
}

void test_report_power_change_by_percent() {
  ReportScheduler s;
  ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onAccepted();
  in.pwmPermille = 9;
  TEST_ASSERT_FALSE(s.due(in, 5000));
  in.pwmPermille = 10;
  TEST_ASSERT_TRUE(s.due(in, 5000));
}

void test_report_heartbeat_idle_15s() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onAccepted();
  TEST_ASSERT_FALSE(s.due(in, 14999));
  TEST_ASSERT_TRUE(s.due(in, 15000));
}

void test_report_heartbeat_active_5s() {
  ReportScheduler s;
  ReportInputs in = upIdle();
  in.targetT10 = 600;
  s.onSent(in, 0);
  s.onAccepted();
  TEST_ASSERT_FALSE(s.due(in, 4999));
  TEST_ASSERT_TRUE(s.due(in, 5000));
}

void test_report_target_change_is_change() {
  ReportScheduler s;
  ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onAccepted();
  in.targetT10 = 600;
  TEST_ASSERT_TRUE(s.due(in, 1000));
}

void test_report_link_drop_sends_nulls() {
  ReportScheduler s;
  ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onAccepted();
  in.link = LinkStatus::NoReply;
  TEST_ASSERT_TRUE(s.due(in, 1000));
  TEST_ASSERT_FALSE(reportValues(in).hasAir);
}

void test_report_method_not_found_stops_until_reset() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onMethodNotFound();
  TEST_ASSERT_EQUAL(PluginState::Missing, s.plugin());
  TEST_ASSERT_FALSE(s.due(in, 60000));
  s.reset();
  TEST_ASSERT_TRUE(s.due(in, 60000));
  TEST_ASSERT_EQUAL(PluginState::Unknown, s.plugin());
}

void test_report_force_next_sends_without_change() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onAccepted();
  s.forceNext();
  TEST_ASSERT_FALSE(s.due(in, 999));
  TEST_ASSERT_TRUE(s.due(in, 1000));
  s.onSent(in, 1000);
  s.onAccepted();
  TEST_ASSERT_FALSE(s.due(in, 2000));
}

void test_report_force_next_keeps_in_flight_and_missing() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.forceNext();
  TEST_ASSERT_FALSE(s.due(in, 5000));
  s.onAccepted();
  TEST_ASSERT_TRUE(s.due(in, 5000));

  ReportScheduler m;
  m.onSent(in, 0);
  m.onMethodNotFound();
  m.forceNext();
  TEST_ASSERT_FALSE(m.due(in, 60000));
  TEST_ASSERT_EQUAL(PluginState::Missing, m.plugin());
}

void test_report_counters() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0);
  s.onAccepted();
  s.onSent(in, 1000);
  s.onRefused();
  s.onSent(in, 2000);
  s.onFailed();
  TEST_ASSERT_EQUAL_UINT32(3, s.sent());
  TEST_ASSERT_EQUAL_UINT32(1, s.refused());
  TEST_ASSERT_EQUAL_UINT32(1, s.failed());
  s.reset();
  TEST_ASSERT_EQUAL_UINT32(3, s.sent());
  TEST_ASSERT_EQUAL_UINT32(1, s.refused());
  TEST_ASSERT_EQUAL_UINT32(1, s.failed());
}

void test_report_wraparound() {
  ReportScheduler s;
  const ReportInputs in = upIdle();
  s.onSent(in, 0xFFFFFF00u);
  s.onAccepted();
  TEST_ASSERT_FALSE(s.due(in, 0xFFFFFF00u + 14999u));
  TEST_ASSERT_TRUE(s.due(in, 0xFFFFFF00u + 15000u));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_token_normalize);
  RUN_TEST(test_token_from_bits);
  RUN_TEST(test_token_format);
  RUN_TEST(test_heater_object_name);
  RUN_TEST(test_match_paired_heater);
  RUN_TEST(test_link_healthy);
  RUN_TEST(test_backoff);
  RUN_TEST(test_interval_elapsed);
  RUN_TEST(test_default_port);
  RUN_TEST(test_keep_stored_api_key);
  RUN_TEST(test_handshake_hint);
  RUN_TEST(test_connect_failure_tls_hint);
  RUN_TEST(test_wifi_connected_none);
  RUN_TEST(test_wifi_sta_reconnect_due);
  RUN_TEST(test_wifi_sta_reconnect_not_due);
  RUN_TEST(test_wifi_ap_retry_due);
  RUN_TEST(test_wifi_ap_retry_not_due);
  RUN_TEST(test_wifi_ap_no_credentials);
  RUN_TEST(test_wifi_wraparound);
  RUN_TEST(test_link_loss_within_grace);
  RUN_TEST(test_link_loss_after_grace);
  RUN_TEST(test_link_loss_healthy);
  RUN_TEST(test_link_loss_not_heating);
  RUN_TEST(test_link_loss_manual);
  RUN_TEST(test_link_loss_wraparound);
  RUN_TEST(test_tx_power_valid);
  RUN_TEST(test_valid_range);
  RUN_TEST(test_clamp_off);
  RUN_TEST(test_clamp_low_to_min);
  RUN_TEST(test_clamp_passthrough);
  RUN_TEST(test_clamp_above_max);
  RUN_TEST(test_vendored_crc_vector);
  RUN_TEST(test_resume_sealed_valid);
  RUN_TEST(test_resume_rejects_power_on);
  RUN_TEST(test_resume_rejects_zeroed);
  RUN_TEST(test_resume_rejects_flipped_bit);
  RUN_TEST(test_resume_rejects_bad_mode);
  RUN_TEST(test_resume_rejects_expired_timer);
  RUN_TEST(test_setup_save_rule);
  RUN_TEST(test_link_loss_carry_continues_accumulated_time);
  RUN_TEST(test_link_loss_carry_already_expired);
  RUN_TEST(test_link_loss_carry_saturates);
  RUN_TEST(test_link_loss_carry_rejects_power_on);
  RUN_TEST(test_link_loss_carry_rejects_garbage);
  RUN_TEST(test_link_loss_carry_healthy_clears);
  RUN_TEST(test_link_loss_carry_zero_is_valid);
  RUN_TEST(test_link_loss_carry_adds_boot_time);
  RUN_TEST(test_link_and_state_names);
  RUN_TEST(test_report_values_null_when_link_not_up);
  RUN_TEST(test_report_values_null_on_no_reading);
  RUN_TEST(test_report_values_null_without_snapshot);
  RUN_TEST(test_report_interval_idle_and_active);
  RUN_TEST(test_report_first_send_due);
  RUN_TEST(test_report_not_due_while_in_flight);
  RUN_TEST(test_report_change_waits_min_gap);
  RUN_TEST(test_report_power_change_by_percent);
  RUN_TEST(test_report_heartbeat_idle_15s);
  RUN_TEST(test_report_heartbeat_active_5s);
  RUN_TEST(test_report_target_change_is_change);
  RUN_TEST(test_report_link_drop_sends_nulls);
  RUN_TEST(test_report_method_not_found_stops_until_reset);
  RUN_TEST(test_report_force_next_sends_without_change);
  RUN_TEST(test_report_force_next_keeps_in_flight_and_missing);
  RUN_TEST(test_report_counters);
  RUN_TEST(test_report_wraparound);
  return UNITY_END();
}
