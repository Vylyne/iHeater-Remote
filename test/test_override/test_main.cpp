#include <unity.h>

#include "KlipperOverride.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

void test_press_in_manual_source_is_not_override() {
  KlipperOverride o;
  o.target(60);
  TEST_ASSERT_FALSE(o.press(false));
  TEST_ASSERT_FALSE(o.active());
  TEST_ASSERT_TRUE(o.target(70));
}

void test_press_starts_override_with_baseline() {
  KlipperOverride o;
  o.target(60);
  TEST_ASSERT_TRUE(o.press(true));
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_FALSE(o.target(60));
  TEST_ASSERT_TRUE(o.active());
}

void test_same_target_within_tolerance_ignored() {
  KlipperOverride o;
  o.target(60);
  o.press(true);
  TEST_ASSERT_FALSE(o.target(59.99f));
  TEST_ASSERT_FALSE(o.target(60.04f));
  TEST_ASSERT_TRUE(o.active());
}

void test_changed_target_ends_override() {
  KlipperOverride o;
  o.target(60);
  o.press(true);
  TEST_ASSERT_TRUE(o.target(65));
  TEST_ASSERT_FALSE(o.active());
  TEST_ASSERT_EQUAL_FLOAT(65, o.klipperTargetC());
}

void test_change_to_zero_ends_override() {
  KlipperOverride o;
  o.target(60);
  o.press(true);
  TEST_ASSERT_TRUE(o.target(0));
  TEST_ASSERT_FALSE(o.active());
}

void test_second_press_keeps_baseline() {
  KlipperOverride o;
  o.target(60);
  o.press(true);
  o.press(true);
  TEST_ASSERT_FALSE(o.target(60));
  TEST_ASSERT_TRUE(o.target(0));
}

void test_unknown_baseline_adopts_first_target() {
  KlipperOverride o;
  TEST_ASSERT_TRUE(o.press(true));
  TEST_ASSERT_FALSE(o.hasKlipperTarget());
  TEST_ASSERT_FALSE(o.target(0));
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_TRUE(o.target(60));
  TEST_ASSERT_FALSE(o.active());
}

void test_end_returns_klipper_target() {
  KlipperOverride o;
  o.target(60);
  o.press(true);
  TEST_ASSERT_EQUAL_FLOAT(60, o.end());
  TEST_ASSERT_FALSE(o.active());
  TEST_ASSERT_TRUE(o.target(60));
}

void test_end_without_target_returns_zero() {
  KlipperOverride o;
  o.press(true);
  TEST_ASSERT_EQUAL_FLOAT(0, o.end());
  TEST_ASSERT_FALSE(o.active());
}

void test_reset_ends_override_and_forgets_target() {
  KlipperOverride o;
  o.target(60);
  o.press(true);
  o.reset();
  TEST_ASSERT_FALSE(o.active());
  TEST_ASSERT_FALSE(o.hasKlipperTarget());
  TEST_ASSERT_EQUAL_FLOAT(0, o.klipperTargetC());
  TEST_ASSERT_TRUE(o.target(0));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_press_in_manual_source_is_not_override);
  RUN_TEST(test_press_starts_override_with_baseline);
  RUN_TEST(test_same_target_within_tolerance_ignored);
  RUN_TEST(test_changed_target_ends_override);
  RUN_TEST(test_change_to_zero_ends_override);
  RUN_TEST(test_second_press_keeps_baseline);
  RUN_TEST(test_unknown_baseline_adopts_first_target);
  RUN_TEST(test_end_returns_klipper_target);
  RUN_TEST(test_end_without_target_returns_zero);
  RUN_TEST(test_reset_ends_override_and_forgets_target);
  return UNITY_END();
}
