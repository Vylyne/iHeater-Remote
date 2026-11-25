#include <unity.h>

#include "FeedCursor.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

constexpr uint32_t kStep = 1000;

// --- histWindow ---

void test_window_from_the_middle() {
  // 10 samples at 1000..10000; the cursor wants 7000 onwards.
  const HistWindow w = histWindow(7000, 10000, 10, kStep, 120);
  TEST_ASSERT_FALSE(w.done);
  TEST_ASSERT_EQUAL_UINT32(6, w.from);
  TEST_ASSERT_EQUAL_UINT32(4, w.n);
  TEST_ASSERT_EQUAL_UINT32(7000, w.firstMs);
  TEST_ASSERT_EQUAL_UINT32(10000, w.lastMs);
}

void test_window_caught_up_is_done() {
  // Past the newest sample: nothing to send, and never the whole ring again.
  TEST_ASSERT_TRUE(histWindow(11000, 10000, 10, kStep, 120).done);
  TEST_ASSERT_TRUE(histWindow(25000, 10000, 10, kStep, 120).done);
}

void test_window_newest_only() {
  const HistWindow w = histWindow(10000, 10000, 10, kStep, 120);
  TEST_ASSERT_FALSE(w.done);
  TEST_ASSERT_EQUAL_UINT32(9, w.from);
  TEST_ASSERT_EQUAL_UINT32(1, w.n);
  TEST_ASSERT_EQUAL_UINT32(10000, w.lastMs);
}

void test_window_scrolled_out_restarts_at_oldest() {
  const HistWindow w = histWindow(0, 10000, 10, kStep, 120);
  TEST_ASSERT_FALSE(w.done);
  TEST_ASSERT_EQUAL_UINT32(0, w.from);
  TEST_ASSERT_EQUAL_UINT32(10, w.n);
  TEST_ASSERT_EQUAL_UINT32(1000, w.firstMs);
  TEST_ASSERT_EQUAL_UINT32(10000, w.lastMs);
}

void test_window_capped_per_frame() {
  const HistWindow w = histWindow(1000, 10000, 10, kStep, 3);
  TEST_ASSERT_EQUAL_UINT32(0, w.from);
  TEST_ASSERT_EQUAL_UINT32(3, w.n);
  TEST_ASSERT_EQUAL_UINT32(3000, w.lastMs);
}

void test_window_across_millis_wrap() {
  // Newest just after the wrap; the cursor is 4 steps before it.
  const uint32_t newest = 0x00000200u;
  const uint32_t next = newest - 4 * 0x100u;  // 0xFFFFFE00
  const HistWindow w = histWindow(next, newest, 10, 0x100u, 120);
  TEST_ASSERT_FALSE(w.done);
  TEST_ASSERT_EQUAL_UINT32(5, w.from);
  TEST_ASSERT_EQUAL_UINT32(5, w.n);
  TEST_ASSERT_EQUAL_UINT32(next, w.firstMs);
  TEST_ASSERT_EQUAL_UINT32(newest, w.lastMs);
}

// --- resync cursors ---

void test_missed_event_cursor_resends_from_that_event() {
  const FeedCursor c = cursorAfterMissedEvent(7, 5, 9000, kStep);
  TEST_ASSERT_FALSE(c.histStage);
  TEST_ASSERT_EQUAL_UINT32(6, c.evSeq);
  // The page keeps what it has: no "first" frames.
  TEST_ASSERT_TRUE(c.evFirstSent);
  TEST_ASSERT_TRUE(c.histFirstSent);
  // It already has every sample: the history stage has nothing to add.
  TEST_ASSERT_TRUE(c.histStarted);
  TEST_ASSERT_EQUAL_UINT32(10000, c.histNextMs);
  TEST_ASSERT_TRUE(histWindow(c.histNextMs, 9000, 5, kStep, 120).done);
}

void test_missed_event_cursor_with_no_history() {
  const FeedCursor c = cursorAfterMissedEvent(1, 0, 0, kStep);
  TEST_ASSERT_EQUAL_UINT32(0, c.evSeq);
  TEST_ASSERT_FALSE(c.histStarted);  // starts at the oldest sample once there is one
}

void test_missed_hist_cursor_resends_those_samples() {
  const FeedCursor c = cursorAfterMissedHist(12, 5000);
  TEST_ASSERT_TRUE(c.histStage);  // it has every event
  TEST_ASSERT_EQUAL_UINT32(12, c.evSeq);
  TEST_ASSERT_TRUE(c.evFirstSent);
  TEST_ASSERT_TRUE(c.histFirstSent);
  TEST_ASSERT_TRUE(c.histStarted);
  TEST_ASSERT_EQUAL_UINT32(5000, c.histNextMs);
}

void test_rewind_sends_events_again_before_more_history() {
  FeedCursor c;
  c.histStage = true;
  c.evFirstSent = true;
  c.evSeq = 9;
  c.histStarted = true;
  c.histNextMs = 4000;
  rewindForEvent(c, 10);
  TEST_ASSERT_FALSE(c.histStage);
  TEST_ASSERT_EQUAL_UINT32(9, c.evSeq);
  TEST_ASSERT_EQUAL_UINT32(4000, c.histNextMs);  // history resumes where it was
}

void test_rewind_for_a_bumped_event_resends_it() {
  // A repeat bumps an event the backlog already sent (it keeps its seq).
  FeedCursor c;
  c.evFirstSent = true;
  c.evSeq = 9;
  rewindForEvent(c, 4);
  TEST_ASSERT_EQUAL_UINT32(3, c.evSeq);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_window_from_the_middle);
  RUN_TEST(test_window_caught_up_is_done);
  RUN_TEST(test_window_newest_only);
  RUN_TEST(test_window_scrolled_out_restarts_at_oldest);
  RUN_TEST(test_window_capped_per_frame);
  RUN_TEST(test_window_across_millis_wrap);
  RUN_TEST(test_missed_event_cursor_resends_from_that_event);
  RUN_TEST(test_missed_event_cursor_with_no_history);
  RUN_TEST(test_missed_hist_cursor_resends_those_samples);
  RUN_TEST(test_rewind_sends_events_again_before_more_history);
  RUN_TEST(test_rewind_for_a_bumped_event_resends_it);
  return UNITY_END();
}
