#include <Arduino.h>
#include <unity.h>

#include "RemotePolicy.h"

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

void test_resolve_vc_target() {
  TEST_ASSERT_EQUAL_FLOAT(
      60.0f, resolveMoonrakerTargetC(MoonrakerInputs{true, 60.0f, 0.0f}, 60.0f, 100, R));
}

void test_resolve_vc_zero_ignores_hot_bed() {
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, resolveMoonrakerTargetC(MoonrakerInputs{true, 0.0f, 110.0f}, 60.0f, 100, R));
}

void test_resolve_vc_clamped() {
  TEST_ASSERT_EQUAL_FLOAT(
      70.0f, resolveMoonrakerTargetC(MoonrakerInputs{true, 78.0f, 0.0f}, 60.0f, 100,
                                     TargetRange{45, 70}));
}

void test_resolve_bed_rule_on() {
  TEST_ASSERT_EQUAL_FLOAT(
      60.0f, resolveMoonrakerTargetC(MoonrakerInputs{false, 0.0f, 110.0f}, 60.0f, 100, R));
}

void test_resolve_bed_rule_off() {
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, resolveMoonrakerTargetC(MoonrakerInputs{false, 0.0f, 90.0f}, 60.0f, 100, R));
}

void test_stale_before_timeout() {
  TEST_ASSERT_FALSE(moonrakerStale(29999, 0, 30000));
}

void test_stale_after_timeout() {
  TEST_ASSERT_TRUE(moonrakerStale(30000, 0, 30000));
}

void test_stale_wraparound() {
  TEST_ASSERT_FALSE(moonrakerStale(5, 0xFFFFFFF0u, 30000));
  TEST_ASSERT_TRUE(moonrakerStale(30000, 0xFFFFFFF0u, 30000));
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

void test_moonraker_reset_status() {
  TEST_ASSERT_TRUE(moonrakerStatusIsReset(""));
  TEST_ASSERT_TRUE(moonrakerStatusIsReset(nullptr));
  TEST_ASSERT_FALSE(moonrakerStatusIsReset("standby"));
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

void setup() {
  delay(2000);
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
  RUN_TEST(test_moonraker_reset_status);
  RUN_TEST(test_tx_power_valid);
  RUN_TEST(test_valid_range);
  RUN_TEST(test_clamp_off);
  RUN_TEST(test_clamp_low_to_min);
  RUN_TEST(test_clamp_passthrough);
  RUN_TEST(test_clamp_above_max);
  RUN_TEST(test_resolve_vc_target);
  RUN_TEST(test_resolve_vc_zero_ignores_hot_bed);
  RUN_TEST(test_resolve_vc_clamped);
  RUN_TEST(test_resolve_bed_rule_on);
  RUN_TEST(test_resolve_bed_rule_off);
  RUN_TEST(test_stale_before_timeout);
  RUN_TEST(test_stale_after_timeout);
  RUN_TEST(test_stale_wraparound);
  UNITY_END();
}

void loop() {}
