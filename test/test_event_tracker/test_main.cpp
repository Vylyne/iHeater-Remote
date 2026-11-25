#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "EventTracker.h"
#include "UartRegisters.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

namespace {

// Runs `from` then `to` and returns the events of the second call.
size_t step(const TrackedState &from, const TrackedState &to, TrackedEvent *out,
            size_t max = 8) {
  EventTracker t;
  TrackedEvent scratch[8];
  t.update(from, scratch, 8);
  return t.update(to, out, max);
}

void expectOne(const TrackedState &from, const TrackedState &to, EventKind kind,
               const char *text) {
  TrackedEvent ev[8];
  TEST_ASSERT_EQUAL_UINT(1, step(from, to, ev));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kind), static_cast<uint8_t>(ev[0].kind));
  TEST_ASSERT_EQUAL_STRING(text, ev[0].text);
}

TrackedState uartState() {
  TrackedState s;
  s.uart = true;
  return s;
}

}  // namespace

void test_first_update_from_defaults() {
  EventTracker t;
  TrackedState s;
  s.wifi = WifiState::Joined;
  TrackedEvent ev[4];
  TEST_ASSERT_EQUAL_UINT(1, t.update(s, ev, 4));
  TEST_ASSERT_EQUAL_STRING("Wi-Fi connected", ev[0].text);
}

void test_no_change_no_events() {
  EventTracker t;
  TrackedState s;
  s.wifi = WifiState::Joined;
  TrackedEvent ev[4];
  t.update(s, ev, 4);
  TEST_ASSERT_EQUAL_UINT(0, t.update(s, ev, 4));
}

void test_wifi_lost() {
  TrackedState a, b;
  a.wifi = WifiState::Joined;
  b.wifi = WifiState::Lost;
  expectOne(a, b, EventKind::Warn, "Wi-Fi lost");
}

void test_wifi_ap() {
  TrackedState a, b;
  b.wifi = WifiState::Ap;
  expectOne(a, b, EventKind::Warn, "Hotspot started");
}

void test_moon_ready() {
  TrackedState a, b;
  a.moon = MoonState::Connecting;
  b.moon = MoonState::Ready;
  expectOne(a, b, EventKind::Info, "Moonraker connected, Klipper ready");
}

void test_moon_not_ready() {
  TrackedState a, b;
  a.moon = MoonState::Ready;
  b.moon = MoonState::NotReady;
  expectOne(a, b, EventKind::Warn, "Klipper not ready");
}

void test_moon_not_ready_to_ready() {
  TrackedState a, b;
  a.moon = MoonState::NotReady;
  b.moon = MoonState::Ready;
  expectOne(a, b, EventKind::Info, "Moonraker connected, Klipper ready");
}

void test_moon_disconnected() {
  TrackedState a, b;
  a.moon = MoonState::Ready;
  b.moon = MoonState::Disconnected;
  expectOne(a, b, EventKind::Warn, "Moonraker disconnected");
  a.moon = MoonState::NotReady;
  b.moon = MoonState::Connecting;
  expectOne(a, b, EventKind::Warn, "Moonraker disconnected");
}

void test_moon_other_transitions_silent() {
  TrackedState a, b;
  TrackedEvent ev[4];
  a.moon = MoonState::Disconnected;
  b.moon = MoonState::Connecting;
  TEST_ASSERT_EQUAL_UINT(0, step(a, b, ev));
  a.moon = MoonState::Ready;
  b.moon = MoonState::Off;
  TEST_ASSERT_EQUAL_UINT(0, step(a, b, ev));
}

void test_paired() {
  TrackedState a, b;
  b.paired = true;
  expectOne(a, b, EventKind::Info, "Paired with Klipper");
}

void test_link_up() {
  TrackedState a = uartState(), b = uartState();
  b.link = LinkStatus::Up;
  expectOne(a, b, EventKind::Info, "iHeater link up");
}

void test_link_no_reply() {
  TrackedState a = uartState(), b = uartState();
  a.link = LinkStatus::Up;
  b.link = LinkStatus::NoReply;
  expectOne(a, b, EventKind::Error, "iHeater not replying");
}

void test_link_updating() {
  TrackedState a = uartState(), b = uartState();
  b.link = LinkStatus::Updating;
  expectOne(a, b, EventKind::Info, "iHeater in its bootloader");
}

void test_link_restarting_silent() {
  TrackedState a = uartState(), b = uartState();
  TrackedEvent ev[4];
  a.link = LinkStatus::Up;
  b.link = LinkStatus::Restarting;
  TEST_ASSERT_EQUAL_UINT(0, step(a, b, ev));
}

void test_link_fields_ignored_without_uart() {
  EventTracker t;
  TrackedState s;
  s.link = LinkStatus::NoReply;
  s.hasSnapshot = true;
  s.fault = 3;
  s.clear = ClearPhase::Blocked;
  TrackedEvent ev[4];
  TEST_ASSERT_EQUAL_UINT(0, t.update(s, ev, 4));
}

void test_protocol_not_supported() {
  TrackedState a = uartState(), b = uartState();
  b.hasSnapshot = true;
  b.protocolSupported = false;
  b.proto = 7;
  expectOne(a, b, EventKind::Error, "iHeater link protocol 7 not supported");
}

void test_fault_raised() {
  TrackedState a = uartState(), b = uartState();
  a.hasSnapshot = b.hasSnapshot = true;
  b.fault = 3;
  char want[kEventTextMax + 1];
  snprintf(want, sizeof want, "iHeater fault: %s", faultName(3));
  expectOne(a, b, EventKind::Fault, want);
}

void test_fault_changed() {
  TrackedState a = uartState(), b = uartState();
  a.hasSnapshot = b.hasSnapshot = true;
  a.fault = 3;
  b.fault = 5;
  char want[kEventTextMax + 1];
  snprintf(want, sizeof want, "iHeater fault: %s", faultName(5));
  expectOne(a, b, EventKind::Fault, want);
}

void test_fault_cleared() {
  TrackedState a = uartState(), b = uartState();
  a.hasSnapshot = b.hasSnapshot = true;
  a.fault = 3;
  expectOne(a, b, EventKind::Info, "iHeater fault cleared");
}

void test_fault_without_snapshot_ignored() {
  TrackedState a = uartState(), b = uartState();
  b.fault = 3;
  TrackedEvent ev[4];
  TEST_ASSERT_EQUAL_UINT(0, step(a, b, ev));
}

void test_restart_on_uptime_backwards() {
  TrackedState a = uartState(), b = uartState();
  a.hasSnapshot = b.hasSnapshot = true;
  a.ihUptimeS = 500;
  b.ihUptimeS = 2;
  b.resetCause = 4;
  expectOne(a, b, EventKind::Warn, "iHeater restarted (reset cause 0x0004)");
}

void test_restart_needs_snapshot_both_times() {
  EventTracker t;
  TrackedEvent ev[4];
  TrackedState s = uartState();
  s.hasSnapshot = true;
  s.ihUptimeS = 500;
  t.update(s, ev, 4);
  s.hasSnapshot = false;
  TEST_ASSERT_EQUAL_UINT(0, t.update(s, ev, 4));
  s.hasSnapshot = true;
  s.ihUptimeS = 2;
  TEST_ASSERT_EQUAL_UINT(0, t.update(s, ev, 4));
}

void test_clear_sent() {
  TrackedState a = uartState(), b = uartState();
  b.clear = ClearPhase::AwaitRestart;
  expectOne(a, b, EventKind::Action, "Fault clear sent");
}

void test_clear_worked() {
  TrackedState a = uartState(), b = uartState();
  a.clear = ClearPhase::AwaitRestart;
  b.lastClearOk = true;
  expectOne(a, b, EventKind::Info, "Fault clear worked");
}

void test_clear_did_not_work() {
  TrackedState a = uartState(), b = uartState();
  a.clear = ClearPhase::AwaitRestart;
  expectOne(a, b, EventKind::Warn, "Fault clear did not work");
}

void test_clear_blocked() {
  TrackedState a = uartState(), b = uartState();
  b.clear = ClearPhase::Blocked;
  expectOne(a, b, EventKind::Error, "iHeater could not clear the fault: power-cycle it");
}

void test_max_limits_output() {
  TrackedState a, b;
  b.wifi = WifiState::Joined;
  b.moon = MoonState::Ready;
  b.paired = true;
  TrackedEvent ev[8];
  TEST_ASSERT_EQUAL_UINT(2, step(a, b, ev, 2));
  TEST_ASSERT_EQUAL_STRING("Wi-Fi connected", ev[0].text);
  TEST_ASSERT_EQUAL_STRING("Moonraker connected, Klipper ready", ev[1].text);
}

void test_target_text_manual_press() {
  char out[kEventTextMax + 1];
  TEST_ASSERT_TRUE(targetEventText("web", 65, out));
  TEST_ASSERT_EQUAL_STRING("Target 65 \xC2\xB0" "C (manual press)", out);
  TEST_ASSERT_TRUE(targetEventText("button:long", 65, out));
  TEST_ASSERT_EQUAL_STRING("Target 65 \xC2\xB0" "C (manual press)", out);
}

void test_target_text_half_degree() {
  char out[kEventTextMax + 1];
  TEST_ASSERT_TRUE(targetEventText("klipper", 62.5f, out));
  TEST_ASSERT_EQUAL_STRING("Target 62.5 \xC2\xB0" "C (Klipper)", out);
}

void test_target_text_off_timer() {
  char out[kEventTextMax + 1];
  TEST_ASSERT_TRUE(targetEventText("timer", 0, out));
  TEST_ASSERT_EQUAL_STRING("Heat off (timer ended)", out);
  TEST_ASSERT_TRUE(targetEventText("timer: back to klipper", 0, out));
  TEST_ASSERT_EQUAL_STRING("Heat off (timer ended, back to Klipper)", out);
  TEST_ASSERT_TRUE(targetEventText("back to klipper", 50, out));
  TEST_ASSERT_EQUAL_STRING("Target 50 \xC2\xB0" "C (back to Klipper)", out);
}

void test_target_text_unknown_source() {
  char out[kEventTextMax + 1];
  TEST_ASSERT_TRUE(targetEventText("printer link lost 10 min", 0, out));
  TEST_ASSERT_EQUAL_STRING("Heat off (printer link lost 10 min)", out);
}

void test_target_text_boot_skipped() {
  char out[kEventTextMax + 1] = "x";
  TEST_ASSERT_FALSE(targetEventText("boot", 0, out));
}

void test_unpair_reason_not_paired() {
  const char *r = unpairReasonText(
      "not paired: add the token from the Setup page as remote_token in your "
      "[heater_iheater_remote] section");
  TEST_ASSERT_EQUAL_STRING("Not paired: no heater has this Remote's token", r);
}

void test_unpair_reason_other() {
  TEST_ASSERT_EQUAL_STRING("Unpaired from Klipper", unpairReasonText("no reply"));
  TEST_ASSERT_EQUAL_STRING("Pairing token used by more than one heater",
                           unpairReasonText("token found on more than one heater"));
  TEST_ASSERT_EQUAL_STRING("No heater_iheater_remote in Klipper",
                           unpairReasonText("no heater_iheater_remote found"));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_first_update_from_defaults);
  RUN_TEST(test_no_change_no_events);
  RUN_TEST(test_wifi_lost);
  RUN_TEST(test_wifi_ap);
  RUN_TEST(test_moon_ready);
  RUN_TEST(test_moon_not_ready);
  RUN_TEST(test_moon_not_ready_to_ready);
  RUN_TEST(test_moon_disconnected);
  RUN_TEST(test_moon_other_transitions_silent);
  RUN_TEST(test_paired);
  RUN_TEST(test_link_up);
  RUN_TEST(test_link_no_reply);
  RUN_TEST(test_link_updating);
  RUN_TEST(test_link_restarting_silent);
  RUN_TEST(test_link_fields_ignored_without_uart);
  RUN_TEST(test_protocol_not_supported);
  RUN_TEST(test_fault_raised);
  RUN_TEST(test_fault_changed);
  RUN_TEST(test_fault_cleared);
  RUN_TEST(test_fault_without_snapshot_ignored);
  RUN_TEST(test_restart_on_uptime_backwards);
  RUN_TEST(test_restart_needs_snapshot_both_times);
  RUN_TEST(test_clear_sent);
  RUN_TEST(test_clear_worked);
  RUN_TEST(test_clear_did_not_work);
  RUN_TEST(test_clear_blocked);
  RUN_TEST(test_max_limits_output);
  RUN_TEST(test_target_text_manual_press);
  RUN_TEST(test_target_text_half_degree);
  RUN_TEST(test_target_text_off_timer);
  RUN_TEST(test_target_text_unknown_source);
  RUN_TEST(test_target_text_boot_skipped);
  RUN_TEST(test_unpair_reason_not_paired);
  RUN_TEST(test_unpair_reason_other);
  return UNITY_END();
}
