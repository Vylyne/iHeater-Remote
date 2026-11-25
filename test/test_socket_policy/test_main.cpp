#include <unity.h>

#include "SocketPolicy.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

// --- SocketGate ---

void test_gate_sends_when_writable() {
  SocketGate g;
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, true, 0) == SendAction::Send);
  TEST_ASSERT_TRUE(g.decide(FrameKind::Stream, true, 0) == SendAction::Send);
}

void test_gate_skips_status_when_blocked() {
  SocketGate g;
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, 1000) == SendAction::Skip);
}

void test_gate_closes_on_blocked_stream() {
  SocketGate g;
  TEST_ASSERT_TRUE(g.decide(FrameKind::Stream, false, 1000) == SendAction::Close);
}

void test_gate_closes_after_stuck_time() {
  SocketGate g;
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, 1000) == SendAction::Skip);
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, 1000 + kSocketStuckMs - 1) ==
                   SendAction::Skip);
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, 1000 + kSocketStuckMs) ==
                   SendAction::Close);
}

void test_gate_send_clears_stuck_time() {
  SocketGate g;
  g.decide(FrameKind::Status, false, 0);
  g.decide(FrameKind::Status, true, 2000);
  g.decide(FrameKind::Status, false, 2500);
  // 2500 ms since the new stall began, not 4500 since the first.
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, 5000) == SendAction::Skip);
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, 2500 + kSocketStuckMs) ==
                   SendAction::Close);
}

void test_gate_stuck_time_across_millis_wrap() {
  SocketGate g;
  const uint32_t start = 0xFFFFFF00u;
  g.decide(FrameKind::Status, false, start);
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, start + 1000) == SendAction::Skip);
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, start + kSocketStuckMs) ==
                   SendAction::Close);
}

void test_gate_reset_forgets_stall() {
  SocketGate g;
  g.decide(FrameKind::Status, false, 0);
  g.reset();
  TEST_ASSERT_TRUE(g.decide(FrameKind::Status, false, kSocketStuckMs + 10) ==
                   SendAction::Skip);
}

// --- write budget ---

void test_write_budget_remaining() {
  TEST_ASSERT_EQUAL_UINT32(kSocketWriteBudgetMs, writeBudgetRemaining(100, 100));
  TEST_ASSERT_EQUAL_UINT32(1, writeBudgetRemaining(100, 100 + kSocketWriteBudgetMs - 1));
  TEST_ASSERT_EQUAL_UINT32(0, writeBudgetRemaining(100, 100 + kSocketWriteBudgetMs));
  TEST_ASSERT_EQUAL_UINT32(0, writeBudgetRemaining(100, 100 + 10 * kSocketWriteBudgetMs));
}

void test_write_budget_across_millis_wrap() {
  const uint32_t start = 0xFFFFFFF0u;
  TEST_ASSERT_EQUAL_UINT32(kSocketWriteBudgetMs - 32, writeBudgetRemaining(start, start + 32));
}

// --- slotToClose ---

SocketSlot anon(uint32_t order) { return SocketSlot{true, false, false, order}; }
SocketSlot user(uint32_t order) { return SocketSlot{true, true, false, order}; }
SocketSlot freeSlot() { return SocketSlot{false, false, false, 0}; }

void test_slot_free_slot_left_closes_none() {
  SocketSlot s[5] = {anon(1), anon(2), anon(3), freeSlot(), freeSlot()};
  s[3] = anon(4);  // the newcomer; slot 4 is still free
  TEST_ASSERT_EQUAL_INT(kNoSlot, slotToClose(s, 5, 3));
}

void test_slot_logged_out_newcomer_filling_last_is_closed() {
  SocketSlot s[5] = {anon(1), anon(2), anon(3), anon(4), anon(5)};
  TEST_ASSERT_EQUAL_INT(4, slotToClose(s, 5, 4));
}

void test_slot_logged_in_newcomer_closes_oldest_logged_out() {
  SocketSlot s[5] = {anon(7), anon(3), user(2), anon(5), user(9)};
  TEST_ASSERT_EQUAL_INT(1, slotToClose(s, 5, 4));
}

void test_slot_all_logged_in_closes_none() {
  SocketSlot s[5] = {user(1), user(2), user(3), user(4), user(5)};
  TEST_ASSERT_EQUAL_INT(kNoSlot, slotToClose(s, 5, 4));
}

void test_slot_closing_counts_as_free() {
  SocketSlot s[5] = {anon(1), anon(2), anon(3), anon(4), anon(5)};
  s[0].closing = true;
  TEST_ASSERT_EQUAL_INT(kNoSlot, slotToClose(s, 5, 4));
}

void test_slot_handshaking_is_never_closed() {
  SocketSlot s[5] = {anon(1), anon(2), user(3), user(4), user(5)};
  s[0].pending = true;  // still in its handshake, login not known yet
  TEST_ASSERT_EQUAL_INT(1, slotToClose(s, 5, 4));
  s[1].pending = true;
  TEST_ASSERT_EQUAL_INT(kNoSlot, slotToClose(s, 5, 4));
}

void test_slot_oldest_across_order_wrap() {
  // order wraps: 0xFFFFFFF0 was given before 0x00000002.
  SocketSlot s[5] = {anon(2), anon(0xFFFFFFF0u), user(5), user(6), user(7)};
  TEST_ASSERT_EQUAL_INT(1, slotToClose(s, 5, 4));
}

void test_slot_newcomer_out_of_range() {
  SocketSlot s[2] = {anon(1), anon(2)};
  TEST_ASSERT_EQUAL_INT(kNoSlot, slotToClose(s, 2, 2));
  TEST_ASSERT_EQUAL_INT(kNoSlot, slotToClose(nullptr, 0, 0));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_gate_sends_when_writable);
  RUN_TEST(test_gate_skips_status_when_blocked);
  RUN_TEST(test_gate_closes_on_blocked_stream);
  RUN_TEST(test_gate_closes_after_stuck_time);
  RUN_TEST(test_gate_send_clears_stuck_time);
  RUN_TEST(test_gate_stuck_time_across_millis_wrap);
  RUN_TEST(test_gate_reset_forgets_stall);
  RUN_TEST(test_write_budget_remaining);
  RUN_TEST(test_write_budget_across_millis_wrap);
  RUN_TEST(test_slot_free_slot_left_closes_none);
  RUN_TEST(test_slot_logged_out_newcomer_filling_last_is_closed);
  RUN_TEST(test_slot_logged_in_newcomer_closes_oldest_logged_out);
  RUN_TEST(test_slot_all_logged_in_closes_none);
  RUN_TEST(test_slot_closing_counts_as_free);
  RUN_TEST(test_slot_handshaking_is_never_closed);
  RUN_TEST(test_slot_oldest_across_order_wrap);
  RUN_TEST(test_slot_newcomer_out_of_range);
  return UNITY_END();
}
