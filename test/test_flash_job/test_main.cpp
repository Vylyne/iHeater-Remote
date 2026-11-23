#include <string.h>
#include <unity.h>

#include <initializer_list>

#include "../image_fixture.h"
#include "FakeIheater.h"
#include "FlashJob.h"
#include "RomProtocol.h"

using namespace iheaterlink::policy;
using fakeiheater::FakeIheater;
using fakeiheater::Mode;

void setUp() {}
void tearDown() {}

void test_command_frame() {
  uint8_t out[2];
  buildRomCommand(kRomWrite, out);
  TEST_ASSERT_EQUAL_HEX8(0x31, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xCE, out[1]);
  buildRomCommand(kRomGet, out);
  TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[1]);
}

void test_address_frame() {
  uint8_t out[5];
  buildRomAddress(0x08000100, out);
  const uint8_t want[5] = {0x08, 0x00, 0x01, 0x00, 0x09};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

void test_extended_erase_body() {
  uint8_t out[64];
  TEST_ASSERT_EQUAL(9, buildEraseBody(3, true, out, sizeof out));
  const uint8_t want[9] = {0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x01};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 9);
}

void test_legacy_erase_body() {
  uint8_t out[64];
  TEST_ASSERT_EQUAL(5, buildEraseBody(3, false, out, sizeof out));
  const uint8_t want[5] = {0x02, 0x00, 0x01, 0x02, 0x01};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

void test_erase_refuses_page_30() {
  uint8_t out[128];
  TEST_ASSERT_EQUAL(63, buildEraseBody(30, true, out, sizeof out));
  TEST_ASSERT_EQUAL(32, buildEraseBody(30, false, out, sizeof out));
  for (bool ext : {true, false}) {
    TEST_ASSERT_EQUAL(0, buildEraseBody(31, ext, out, sizeof out));
    TEST_ASSERT_EQUAL(0, buildEraseBody(0, ext, out, sizeof out));
  }
  TEST_ASSERT_EQUAL(0, buildEraseBody(3, true, out, 8));
  TEST_ASSERT_EQUAL(0, buildEraseBody(3, false, out, 4));
}

void test_write_body() {
  uint8_t out[300];
  const uint8_t data[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL(6, buildWriteBody(data, 4, out, sizeof out));
  const uint8_t want[6] = {0x03, 0x01, 0x02, 0x03, 0x04, 0x07};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 6);

  uint8_t big[260] = {0};
  TEST_ASSERT_EQUAL(0, buildWriteBody(big, 5, out, sizeof out));
  TEST_ASSERT_EQUAL(0, buildWriteBody(big, 0, out, sizeof out));
  TEST_ASSERT_EQUAL(0, buildWriteBody(big, 260, out, sizeof out));
  TEST_ASSERT_EQUAL(258, buildWriteBody(big, 256, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
  TEST_ASSERT_EQUAL(0, buildWriteBody(data, 4, out, 5));
}

void test_read_count() {
  uint8_t out[2];
  buildReadCount(256, out);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
  buildReadCount(4, out);
  TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFC, out[1]);
}

namespace {

std::vector<uint8_t> testImage(size_t size = 3000) {
  std::vector<uint8_t> img = imagefixture::makeImage(size);
  imagefixture::patchImage(img, size, 0x1A2B3C4D, 0);
  return img;
}

FlashStatus run(FakeIheater &f, const std::vector<uint8_t> &img, FlashStart start = FlashStart::Retry) {
  return runFlashJob(f, img.data(), img.size(), start);
}

std::vector<FlashPhase> phaseOrder(const FakeIheater &f) {
  std::vector<FlashPhase> out;
  for (const FlashStatus &s : f.published)
    if (out.empty() || out.back() != s.phase) out.push_back(s.phase);
  return out;
}

void assertFinished(const FakeIheater &f, const FlashStatus &st) {
  TEST_ASSERT_EQUAL(FlashPhase::Finished, st.phase);
  TEST_ASSERT_TRUE(f.parity == LinkParity::None);
  TEST_ASSERT_FALSE(f.published.empty());
  TEST_ASSERT_EQUAL(FlashPhase::Finished, f.published.back().phase);
  TEST_ASSERT_EQUAL(st.outcome, f.published.back().outcome);
}

void assertFlashMatches(const FakeIheater &f, const std::vector<uint8_t> &img) {
  TEST_ASSERT_EQUAL_HEX8_ARRAY(img.data(), f.flash.data(), img.size());
}

void assertSequence(const FakeIheater &f) {
  const FlashPhase want[] = {FlashPhase::Syncing, FlashPhase::Erasing, FlashPhase::Writing,
                             FlashPhase::Starting, FlashPhase::Restarting, FlashPhase::Finished};
  const std::vector<FlashPhase> got = phaseOrder(f);
  TEST_ASSERT_EQUAL(6, got.size());
  for (size_t i = 0; i < 6; i++) TEST_ASSERT_TRUE(want[i] == got[i]);
  TEST_ASSERT_EQUAL(0, f.modbusBeforeRestarting);
}

}  // namespace

void test_retry_starts_at_sync() {
  FakeIheater f;
  const auto img = testImage();
  const FlashStatus st = run(f, img);
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  assertFinished(f, st);
  assertSequence(f);
  assertFlashMatches(f, img);
  TEST_ASSERT_EQUAL(3, f.erasedPages.size());
  for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL(i, f.erasedPages[i]);
  TEST_ASSERT_FALSE(f.massEraseAttempted);
  TEST_ASSERT_TRUE(st.extendedErase);
  TEST_ASSERT_EQUAL_HEX16(0x445, st.pid);
  TEST_ASSERT_FALSE(st.unexpectedPid);
}

void test_recovery_skips_cooling_and_entry() {
  FakeIheater f;
  const auto img = testImage();
  const FlashStatus st = run(f, img, FlashStart::Recovery);
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  assertFinished(f, st);
  assertSequence(f);
  assertFlashMatches(f, img);
  TEST_ASSERT_EQUAL(3, f.erasedPages.size());
}

void test_write_order_word0_last() {
  FakeIheater f;
  const auto img = testImage();
  run(f, img);
  const uint32_t addrs[] = {0x08000100, 0x08000200, 0x08000300, 0x08000400, 0x08000500, 0x08000600,
                            0x08000700, 0x08000800, 0x08000900, 0x08000A00, 0x08000B00,
                            0x08000004, 0x08000000};
  TEST_ASSERT_EQUAL(13, f.writes.size());
  for (size_t i = 0; i < 13; i++) TEST_ASSERT_EQUAL_HEX32(addrs[i], f.writes[i].addr);
  TEST_ASSERT_EQUAL(184, f.writes[10].len);
  TEST_ASSERT_EQUAL(252, f.writes[11].len);
  TEST_ASSERT_EQUAL(4, f.writes[12].len);
}

void test_padding_to_word() {
  FakeIheater f;
  const auto img = testImage(3001);
  const FlashStatus st = run(f, img);
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  TEST_ASSERT_EQUAL_HEX32(0x08000B00, f.writes[10].addr);
  TEST_ASSERT_EQUAL(188, f.writes[10].len);
  assertFlashMatches(f, img);
  for (size_t i = 3001; i < 3004; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, f.flash[i]);
}

void test_sync_nack_already_synced() {
  FakeIheater f;
  f.alreadySynced = true;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  assertFinished(f, st);
}

void test_sync_ignores_noise_byte() {
  FakeIheater f;
  f.noiseBeforeSyncAck = true;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  TEST_ASSERT_TRUE(f.syncTimes.size() >= 2);
  TEST_ASSERT_TRUE(f.syncTimes[1] - f.syncTimes[0] >= kSyncGapMs);
}

void test_sync_timeout() {
  FakeIheater f;
  f.romSilent = true;
  const uint32_t t0 = f.clock;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::SyncFailed, st.outcome);
  TEST_ASSERT_EQUAL(FlashPhase::Syncing, st.failedPhase);
  TEST_ASSERT_EQUAL_STRING("sync: no reply after 20 tries", st.detail);
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
  TEST_ASSERT_FALSE(applicationIntact(st.outcome));
  const uint32_t dt = f.clock - t0;
  TEST_ASSERT_TRUE(dt >= 2000 && dt <= 5000);
  assertFinished(f, st);
}

void test_legacy_erase() {
  FakeIheater f;
  f.extended = false;
  const auto img = testImage();
  const FlashStatus st = run(f, img);
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  TEST_ASSERT_FALSE(st.extendedErase);
  TEST_ASSERT_FALSE(f.lastEraseExtended);
  TEST_ASSERT_EQUAL(3, f.erasedPages.size());
  assertFlashMatches(f, img);
}

void test_unexpected_pid_warns() {
  FakeIheater f;
  f.pid = 0x440;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::Updated, st.outcome);
  TEST_ASSERT_TRUE(st.unexpectedPid);
  TEST_ASSERT_EQUAL_HEX16(0x440, st.pid);
}

void test_write_nack_reports_step() {
  FakeIheater f;
  f.nackWriteAt = 0x08000200;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::RomError, st.outcome);
  TEST_ASSERT_EQUAL_STRING("write at 0x08000200: NACK", st.detail);
  TEST_ASSERT_EQUAL(FlashPhase::Writing, st.failedPhase);
  TEST_ASSERT_FALSE(f.goSeen);
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
  assertFinished(f, st);
}

void test_verify_mismatch_stops() {
  FakeIheater f;
  f.corruptReadAt = 0x08000300;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::VerifyFailed, st.outcome);
  TEST_ASSERT_EQUAL_STRING("verify at 0x08000300: data mismatch", st.detail);
  TEST_ASSERT_EQUAL(FlashPhase::Writing, st.failedPhase);
  TEST_ASSERT_FALSE(f.goSeen);
  for (const auto &w : f.writes) TEST_ASSERT_TRUE(w.addr != 0x08000000);
  assertFinished(f, st);
}

void test_go_to_flash_base() {
  FakeIheater f;
  run(f, testImage());
  TEST_ASSERT_TRUE(f.goSeen);
  TEST_ASSERT_EQUAL_HEX32(0x08000000, f.goAddress);
}

void test_restart_rom_answers_power_cycle() {
  FakeIheater f;
  f.modeAfterGo = Mode::Rom;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::PowerCycleNeeded, st.outcome);
  TEST_ASSERT_FALSE(retryAllowed(st.outcome));
  TEST_ASSERT_FALSE(applicationIntact(st.outcome));
  assertFinished(f, st);
}

void test_restart_silent() {
  FakeIheater f;
  f.modeAfterGo = Mode::Silent;
  const FlashStatus st = run(f, testImage());
  TEST_ASSERT_EQUAL(FlashOutcome::NoReplyAfterUpdate, st.outcome);
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
  assertFinished(f, st);
  // Each failed poll sleeps one poll period; 15 s is about 75 of them.
  TEST_ASSERT_TRUE(f.modbusCalls >= 70 && f.modbusCalls <= 80);
}

void test_progress_reaches_1000_last() {
  FakeIheater f;
  run(f, testImage());
  uint16_t prev = 0;
  int at1000 = 0;
  size_t lastWriting = 0, at1000Index = 0;
  for (size_t i = 0; i < f.published.size(); i++) {
    const FlashStatus &s = f.published[i];
    TEST_ASSERT_TRUE(s.progressPermille >= prev);
    prev = s.progressPermille;
    if (s.phase == FlashPhase::Writing) {
      lastWriting = i;
      if (s.progressPermille == 1000) {
        at1000++;
        at1000Index = i;
      }
    }
  }
  TEST_ASSERT_EQUAL(1, at1000);
  TEST_ASSERT_EQUAL(lastWriting, at1000Index);
}

void test_oversize_image_rejected() {
  FakeIheater f;
  std::vector<uint8_t> img(30721, 0xAA);
  const FlashStatus st = runFlashJob(f, img.data(), img.size(), FlashStart::Retry);
  TEST_ASSERT_EQUAL(FlashOutcome::ImageRejected, st.outcome);
  TEST_ASSERT_EQUAL(0, f.sentBytes);
  TEST_ASSERT_TRUE(applicationIntact(st.outcome));
  assertFinished(f, st);
  const FlashStatus tiny = runFlashJob(f, img.data(), 7, FlashStart::Retry);
  TEST_ASSERT_EQUAL(FlashOutcome::ImageRejected, tiny.outcome);
  TEST_ASSERT_EQUAL(0, f.sentBytes);
}

void test_probe_rom() {
  {
    FakeIheater f;  // fresh ROM: ACK
    TEST_ASSERT_TRUE(probeRom(f));
    TEST_ASSERT_TRUE(f.parity == LinkParity::None);
  }
  {
    FakeIheater f;  // synced ROM: NACK
    f.alreadySynced = true;
    TEST_ASSERT_TRUE(probeRom(f));
    TEST_ASSERT_TRUE(f.parity == LinkParity::None);
  }
  {
    FakeIheater f;
    f.romSilent = true;
    TEST_ASSERT_FALSE(probeRom(f));
    TEST_ASSERT_TRUE(f.parity == LinkParity::None);
  }
}

void test_rom_probe_due() {
  TEST_ASSERT_FALSE(romProbeDue(9999, 0, 0));
  TEST_ASSERT_TRUE(romProbeDue(10000, 0, 0));
  TEST_ASSERT_FALSE(romProbeDue(50000, 45000, 0));
  TEST_ASSERT_FALSE(romProbeDue(50000, 1000, 30000));
  TEST_ASSERT_TRUE(romProbeDue(60000, 1000, 30000));
}

void test_outcome_tables() {
  TEST_ASSERT_EQUAL_STRING("writing", flashPhaseName(FlashPhase::Writing));
  TEST_ASSERT_EQUAL_STRING("power-cycle", flashOutcomeName(FlashOutcome::PowerCycleNeeded));
  TEST_ASSERT_EQUAL_STRING("no-reply-after-update", flashOutcomeName(FlashOutcome::NoReplyAfterUpdate));
  TEST_ASSERT_EQUAL_STRING("Stopped: heat requested.", flashOutcomeMessage(FlashOutcome::HeatRequested));
  TEST_ASSERT_TRUE(retryAllowed(FlashOutcome::VerifyFailed));
  TEST_ASSERT_FALSE(retryAllowed(FlashOutcome::Updated));
  TEST_ASSERT_TRUE(applicationIntact(FlashOutcome::Cancelled));
  TEST_ASSERT_FALSE(applicationIntact(FlashOutcome::RomError));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_command_frame);
  RUN_TEST(test_address_frame);
  RUN_TEST(test_extended_erase_body);
  RUN_TEST(test_legacy_erase_body);
  RUN_TEST(test_erase_refuses_page_30);
  RUN_TEST(test_write_body);
  RUN_TEST(test_read_count);
  RUN_TEST(test_retry_starts_at_sync);
  RUN_TEST(test_recovery_skips_cooling_and_entry);
  RUN_TEST(test_write_order_word0_last);
  RUN_TEST(test_padding_to_word);
  RUN_TEST(test_sync_nack_already_synced);
  RUN_TEST(test_sync_ignores_noise_byte);
  RUN_TEST(test_sync_timeout);
  RUN_TEST(test_legacy_erase);
  RUN_TEST(test_unexpected_pid_warns);
  RUN_TEST(test_write_nack_reports_step);
  RUN_TEST(test_verify_mismatch_stops);
  RUN_TEST(test_go_to_flash_base);
  RUN_TEST(test_restart_rom_answers_power_cycle);
  RUN_TEST(test_restart_silent);
  RUN_TEST(test_progress_reaches_1000_last);
  RUN_TEST(test_oversize_image_rejected);
  RUN_TEST(test_probe_rom);
  RUN_TEST(test_rom_probe_due);
  RUN_TEST(test_outcome_tables);
  return UNITY_END();
}
