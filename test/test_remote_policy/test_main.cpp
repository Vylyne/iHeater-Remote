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

void setup() {
  delay(2000);
  UNITY_BEGIN();
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
