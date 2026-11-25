#include <unity.h>

#include "TempHistory.h"
#include "UartRegisters.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

static HistSample sample(int16_t a, int16_t h, int16_t t) {
  HistSample s;
  s.airT10 = a;
  s.heaterT10 = h;
  s.targetT10 = t;
  return s;
}

static void assertSame(const HistSample &want, const HistSample &got) {
  TEST_ASSERT_EQUAL_INT16(want.airT10, got.airT10);
  TEST_ASSERT_EQUAL_INT16(want.heaterT10, got.heaterT10);
  TEST_ASSERT_EQUAL_INT16(want.targetT10, got.targetT10);
}

static void assertNoData(const HistSample &got) {
  TEST_ASSERT_EQUAL_INT16(kNoReading, got.airT10);
  TEST_ASSERT_EQUAL_INT16(kNoReading, got.heaterT10);
  TEST_ASSERT_EQUAL_INT16(kNoReading, got.targetT10);
}

void test_first_tick_samples() {
  TempHistory h;
  TEST_ASSERT_EQUAL_UINT32(1, h.tick(5000, sample(1, 2, 3)));
  TEST_ASSERT_EQUAL_UINT32(1, h.size());
  TEST_ASSERT_EQUAL_UINT32(5000, h.newestMs());
}

void test_no_sample_before_period() {
  TempHistory h;
  h.tick(5000, sample(1, 2, 3));
  TEST_ASSERT_EQUAL_UINT32(0, h.tick(14999, sample(4, 5, 6)));
  TEST_ASSERT_EQUAL_UINT32(1, h.size());
}

void test_sample_on_grid() {
  TempHistory h;
  h.tick(5000, sample(1, 2, 3));
  HistSample s2 = sample(4, 5, 6);
  TEST_ASSERT_EQUAL_UINT32(1, h.tick(15300, s2));
  TEST_ASSERT_EQUAL_UINT32(15000, h.newestMs());
  assertSame(s2, h.at(1));
}

void test_stall_fills_no_data() {
  TempHistory h;
  h.tick(0, sample(1, 2, 3));
  HistSample s = sample(7, 8, 9);
  TEST_ASSERT_EQUAL_UINT32(3, h.tick(35000, s));
  TEST_ASSERT_EQUAL_UINT32(4, h.size());
  assertNoData(h.at(1));
  assertNoData(h.at(2));
  assertSame(s, h.at(3));
  TEST_ASSERT_EQUAL_UINT32(30000, h.newestMs());
}

void test_ring_keeps_360() {
  TempHistory h;
  HistSample last;
  for (int i = 0; i < 400; i++) {
    last = sample((int16_t)i, 0, 0);
    h.tick((uint32_t)i * 10000, last);
  }
  TEST_ASSERT_EQUAL_UINT32(360, h.size());
  assertSame(last, h.at(359));
  TEST_ASSERT_EQUAL_INT16(40, h.at(0).airT10);
}

void test_stall_over_an_hour() {
  TempHistory h;
  h.tick(0, sample(1, 2, 3));
  HistSample s = sample(7, 8, 9);
  h.tick(5u * 3600u * 1000u, s);
  TEST_ASSERT_EQUAL_UINT32(360, h.size());
  for (size_t i = 0; i < 359; i++) assertNoData(h.at(i));
  assertSame(s, h.at(359));
  TEST_ASSERT_EQUAL_UINT32(5u * 3600u * 1000u, h.newestMs());
}

void test_period_across_millis_wrap() {
  TempHistory h;
  h.tick(0xFFFFE000u, sample(1, 2, 3));
  TEST_ASSERT_EQUAL_UINT32(1, h.tick(0x00001800u, sample(4, 5, 6)));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(0xFFFFE000u + 10000u), h.newestMs());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_tick_samples);
  RUN_TEST(test_no_sample_before_period);
  RUN_TEST(test_sample_on_grid);
  RUN_TEST(test_stall_fills_no_data);
  RUN_TEST(test_ring_keeps_360);
  RUN_TEST(test_stall_over_an_hour);
  RUN_TEST(test_period_across_millis_wrap);
  return UNITY_END();
}
