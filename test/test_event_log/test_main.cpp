#include <string.h>
#include <unity.h>

#include <string>

#include "EventLog.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

void test_first_event_has_seq_1() {
  EventLog log;
  const Event &e = log.add(EventKind::Info, "a", 100);
  TEST_ASSERT_EQUAL_UINT32(1, e.seq);
  TEST_ASSERT_EQUAL_UINT16(1, e.count);
  TEST_ASSERT_EQUAL_UINT32(100, e.ms);
  TEST_ASSERT_EQUAL_UINT32(1, log.size());
}

void test_ring_keeps_last_64() {
  EventLog log;
  for (int i = 0; i < 70; i++) {
    std::string t = "e" + std::to_string(i);
    log.add(EventKind::Info, t.c_str(), 0);
  }
  TEST_ASSERT_EQUAL_UINT32(64, log.size());
  TEST_ASSERT_EQUAL_UINT32(7, log.at(0).seq);
  TEST_ASSERT_EQUAL_UINT32(70, log.at(63).seq);
}

void test_repeat_within_60s_collapses() {
  EventLog log;
  log.add(EventKind::Warn, "x", 1000);
  const Event &e = log.add(EventKind::Warn, "x", 61000);
  TEST_ASSERT_EQUAL_UINT32(1, log.size());
  TEST_ASSERT_EQUAL_UINT16(2, log.at(0).count);
  TEST_ASSERT_EQUAL_UINT32(61000, log.at(0).ms);
  TEST_ASSERT_EQUAL_UINT32(1, log.at(0).seq);
  TEST_ASSERT_EQUAL_UINT32(1, e.seq);
}

void test_repeat_after_60s_is_new() {
  EventLog log;
  log.add(EventKind::Warn, "x", 1000);
  log.add(EventKind::Warn, "x", 61001);
  TEST_ASSERT_EQUAL_UINT32(2, log.size());
}

void test_collapse_window_moves_with_latest() {
  EventLog log;
  log.add(EventKind::Info, "x", 0);
  log.add(EventKind::Info, "x", 50000);
  log.add(EventKind::Info, "x", 100000);
  TEST_ASSERT_EQUAL_UINT32(1, log.size());
  TEST_ASSERT_EQUAL_UINT16(3, log.at(0).count);
}

void test_same_text_other_kind_is_new() {
  EventLog log;
  log.add(EventKind::Info, "x", 0);
  log.add(EventKind::Warn, "x", 1);
  TEST_ASSERT_EQUAL_UINT32(2, log.size());
}

void test_collapse_only_into_newest_match() {
  EventLog log;
  log.add(EventKind::Info, "a", 0);
  log.add(EventKind::Info, "b", 1);
  log.add(EventKind::Info, "a", 2);
  TEST_ASSERT_EQUAL_UINT32(2, log.size());
  TEST_ASSERT_EQUAL_STRING("a", log.at(0).text);
  TEST_ASSERT_EQUAL_UINT16(2, log.at(0).count);
  TEST_ASSERT_EQUAL_UINT16(1, log.at(1).count);
}

void test_collapse_across_millis_wrap() {
  EventLog log;
  log.add(EventKind::Info, "x", 0xFFFFF000u);
  log.add(EventKind::Info, "x", 0x00000800u);
  TEST_ASSERT_EQUAL_UINT32(1, log.size());
  TEST_ASSERT_EQUAL_UINT16(2, log.at(0).count);
}

void test_text_cut_at_64_bytes() {
  EventLog log;
  std::string s(80, 'a');
  const Event &e = log.add(EventKind::Info, s.c_str(), 0);
  TEST_ASSERT_EQUAL_UINT32(64, strlen(e.text));
}

void test_text_cut_keeps_utf8_whole() {
  EventLog log;
  std::string s(63, 'a');
  s += "\xC2\xB0" "C";
  const Event &e = log.add(EventKind::Info, s.c_str(), 0);
  TEST_ASSERT_EQUAL_UINT32(63, strlen(e.text));
}

void test_kind_names() {
  TEST_ASSERT_EQUAL_STRING("info", eventKindName(EventKind::Info));
  TEST_ASSERT_EQUAL_STRING("action", eventKindName(EventKind::Action));
  TEST_ASSERT_EQUAL_STRING("warn", eventKindName(EventKind::Warn));
  TEST_ASSERT_EQUAL_STRING("fault", eventKindName(EventKind::Fault));
  TEST_ASSERT_EQUAL_STRING("error", eventKindName(EventKind::Error));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_event_has_seq_1);
  RUN_TEST(test_ring_keeps_last_64);
  RUN_TEST(test_repeat_within_60s_collapses);
  RUN_TEST(test_repeat_after_60s_is_new);
  RUN_TEST(test_collapse_window_moves_with_latest);
  RUN_TEST(test_same_text_other_kind_is_new);
  RUN_TEST(test_collapse_only_into_newest_match);
  RUN_TEST(test_collapse_across_millis_wrap);
  RUN_TEST(test_text_cut_at_64_bytes);
  RUN_TEST(test_text_cut_keeps_utf8_whole);
  RUN_TEST(test_kind_names);
  return UNITY_END();
}
