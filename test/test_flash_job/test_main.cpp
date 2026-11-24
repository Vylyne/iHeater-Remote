#include <string.h>
#include <unity.h>

#include <initializer_list>
#include <vector>

#include "../image_fixture.h"
#include "FakeIheater.h"
#include "FlashJob.h"

using namespace iheaterlink::policy;
using fakeiheater::FakeIheater;
using fakeiheater::Fault;
using fakeiheater::Frame;
using fakeiheater::State;

void setUp() {}
void tearDown() {}

namespace {

std::vector<uint8_t> testImage(size_t size = 3000, uint16_t buildType = kBuildTypeApp) {
  std::vector<uint8_t> img = imagefixture::makeImage(size);
  imagefixture::put16(img, kMarkerOffset + 0x08, buildType);
  imagefixture::patchImage(img, size, 0x1A2B3C4D, 0);
  return img;
}

FlashStatus run(FakeIheater &f, const std::vector<uint8_t> &img) {
  return runFlashJob(f, img.data(), img.size());
}

std::vector<FlashPhase> phaseOrder(const FakeIheater &f) {
  std::vector<FlashPhase> out;
  for (const FlashStatus &s : f.published)
    if (out.empty() || out.back() != s.phase) out.push_back(s.phase);
  return out;
}

void assertPhases(const FakeIheater &f, std::initializer_list<FlashPhase> want) {
  const std::vector<FlashPhase> got = phaseOrder(f);
  TEST_ASSERT_EQUAL(want.size(), got.size());
  size_t i = 0;
  for (FlashPhase p : want) TEST_ASSERT_EQUAL((int)p, (int)got[i++]);
}

// The job ended, published its final status, and the fake saw nothing wrong.
void assertEnded(const FakeIheater &f, const FlashStatus &st, FlashOutcome want, FlashPhase failedPhase) {
  if (f.violation) TEST_FAIL_MESSAGE(f.violationText.c_str());
  TEST_ASSERT_EQUAL_STRING(flashOutcomeName(want), flashOutcomeName(st.outcome));
  TEST_ASSERT_EQUAL_STRING(flashPhaseName(failedPhase), flashPhaseName(st.failedPhase));
  TEST_ASSERT_EQUAL((int)FlashPhase::Finished, (int)st.phase);
  TEST_ASSERT_FALSE(f.published.empty());
  TEST_ASSERT_EQUAL((int)FlashPhase::Finished, (int)f.published.back().phase);
  TEST_ASSERT_EQUAL((int)st.outcome, (int)f.published.back().outcome);
  TEST_ASSERT_FALSE(f.violation);
}

// Pages 2-29 hold the image, padded with 0xFF to a whole chunk.
void assertFlashHolds(const FakeIheater &f, const std::vector<uint8_t> &img) {
  TEST_ASSERT_EQUAL_HEX8_ARRAY(img.data(), f.flash.data(), img.size());
  const size_t padded = (img.size() + kChunkBytes - 1) / kChunkBytes * kChunkBytes;
  for (size_t i = img.size(); i < padded; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, f.flash[i]);
}

void assertIdentical(const std::vector<const FakeIheater::Logged *> &frames) {
  for (const FakeIheater::Logged *l : frames) {
    TEST_ASSERT_EQUAL(frames[0]->bytes.size(), l->bytes.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frames[0]->bytes.data(), l->bytes.data(), l->bytes.size());
  }
}

FlashStatus updated(FakeIheater &f, const std::vector<uint8_t> &img) {
  const FlashStatus st = run(f, img);
  assertEnded(f, st, FlashOutcome::Updated, FlashPhase::Idle);
  assertFlashHolds(f, img);
  TEST_ASSERT_TRUE(f.state == State::App);
  return st;
}

}  // namespace

// ---- The whole job ----

void test_updates_from_app() {
  FakeIheater f;
  const auto img = testImage();
  const FlashStatus st = updated(f, img);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.targetWrites);
  assertPhases(f, {FlashPhase::Cooling, FlashPhase::Entering, FlashPhase::Erasing, FlashPhase::Writing,
                   FlashPhase::Starting, FlashPhase::Restarting, FlashPhase::Finished});
  // Progress only grows, and reaches 1000 with the last chunk.
  uint16_t prev = 0;
  size_t lastWriting = 0;
  for (size_t i = 0; i < f.published.size(); i++) {
    TEST_ASSERT_TRUE(f.published[i].progressPermille >= prev);
    prev = f.published[i].progressPermille;
    if (f.published[i].phase == FlashPhase::Writing) lastWriting = i;
  }
  TEST_ASSERT_EQUAL(1000, f.published[lastWriting].progressPermille);
  TEST_ASSERT_EQUAL(1000, st.progressPermille);
  TEST_ASSERT_EQUAL(0, st.retries);
  TEST_ASSERT_FALSE(st.entryUnconfirmed);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(24, f.count(Frame::Chunk));  // 3000 B is 23.4 chunks
  TEST_ASSERT_EQUAL(1, f.count(Frame::Boot));
  // Every chunk is index, then 64 big-endian registers in file order.
  const auto chunks = f.frames(Frame::Chunk);
  for (size_t i = 0; i < chunks.size(); i++) {
    TEST_ASSERT_EQUAL(139, chunks[i]->bytes.size());
    TEST_ASSERT_EQUAL((int)i, chunks[i]->chunk);
  }
  TEST_ASSERT_EQUAL_HEX8(img[128], chunks[1]->bytes[9]);
  TEST_ASSERT_EQUAL_HEX8(img[129], chunks[1]->bytes[10]);
  TEST_ASSERT_TRUE(applicationIntact(st));
}

void test_already_in_bootloader_skips_entry() {
  FakeIheater f;
  f.state = State::Bootloader;
  f.why = 2;
  updated(f, testImage());
  TEST_ASSERT_EQUAL(0, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::WriteTarget));
  TEST_ASSERT_EQUAL(0, f.count(Frame::ReadBlock));
  assertPhases(f, {FlashPhase::Erasing, FlashPhase::Writing, FlashPhase::Starting, FlashPhase::Restarting,
                   FlashPhase::Finished});
}

void test_old_firmware_refused_0x16() {
  FakeIheater f;
  f.bootloaderVersion = 0;
  f.refuse16 = true;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::NoLinkBootloader, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(0, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::WriteTarget));
  TEST_ASSERT_EQUAL(2, f.log.size());  // input 0x00, then 0x16
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_FALSE(retryAllowed(st.outcome));
  TEST_ASSERT_EQUAL_STRING("input 0x16 refused: exception 0x02", st.detail);
}

void test_old_firmware_zero_0x16() {
  FakeIheater f;
  f.bootloaderVersion = 0;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::NoLinkBootloader, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(0, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::WriteTarget));
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_EQUAL_STRING("input 0x16 reads 0", st.detail);
}

void test_unexpected_state() {
  FakeIheater f;
  f.appProto = 2;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::UnexpectedState, FlashPhase::Idle);
  TEST_ASSERT_EQUAL_STRING("input 0x00 reads 0x0002", st.detail);
  TEST_ASSERT_EQUAL(1, f.log.size());
  TEST_ASSERT_TRUE(applicationIntact(st));
}

void test_first_read_no_reply() {
  FakeIheater f;
  f.inject(Frame::ReadState, Fault::DropRequest, 1, 0);
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::NoReply, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(5, f.log.size());
  TEST_ASSERT_EQUAL(4, st.retries);
  TEST_ASSERT_EQUAL_STRING("input 0x00: no reply", st.detail);
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
}

// ---- Entering ----

void test_entry_refused_on_first_try() {
  FakeIheater f;
  f.refuseEntry = true;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::EntryRefused, FlashPhase::Entering);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL_STRING("0xB007 refused: exception 0x04", st.detail);
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_TRUE(f.state == State::App);
}

void test_entry_resend_reaches_bootloader() {
  FakeIheater f;
  f.inject(Frame::Entry, Fault::DropReply);
  const FlashStatus st = updated(f, testImage());
  const auto entries = f.frames(Frame::Entry);
  TEST_ASSERT_EQUAL(2, entries.size());
  assertIdentical(entries);
  TEST_ASSERT_TRUE(entries[1]->stateAtSend == State::Bootloader);  // answered with exception 0x02
  TEST_ASSERT_EQUAL(1, st.retries);
  TEST_ASSERT_FALSE(st.entryUnconfirmed);
}

void test_entry_request_lost_then_lands() {
  FakeIheater f;
  f.inject(Frame::Entry, Fault::DropRequest);
  const FlashStatus st = updated(f, testImage());
  TEST_ASSERT_EQUAL(2, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(1, st.retries);
  TEST_ASSERT_FALSE(st.entryUnconfirmed);
}

void test_entry_all_unanswered_but_entered() {
  FakeIheater f;
  f.inject(Frame::Entry, Fault::DropReply, 1, 0);
  const FlashStatus st = updated(f, testImage());
  const auto entries = f.frames(Frame::Entry);
  TEST_ASSERT_EQUAL(5, entries.size());
  assertIdentical(entries);
  TEST_ASSERT_TRUE(st.entryUnconfirmed);
  TEST_ASSERT_EQUAL(4, st.retries);
}

void test_entry_not_confirmed() {
  FakeIheater f;
  f.inject(Frame::Entry, Fault::DropRequest, 1, 0);
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::EntryNotConfirmed, FlashPhase::Entering);
  TEST_ASSERT_EQUAL(5, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Erase));
  TEST_ASSERT_TRUE(st.entryUnconfirmed);
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
  // Polls of input 0x00 every 100 ms from the last 0xB007 try for 3000 ms.
  std::vector<uint32_t> polls;
  const uint32_t lastEntry = f.frames(Frame::Entry).back()->clockAtSend;
  for (const auto *l : f.frames(Frame::ReadState))
    if (l->clockAtSend > lastEntry) polls.push_back(l->clockAtSend);
  TEST_ASSERT_EQUAL(31, polls.size());
  for (size_t i = 1; i < polls.size(); i++) TEST_ASSERT_EQUAL(kEntryPollMs, polls[i] - polls[i - 1]);
  TEST_ASSERT_EQUAL(kEntryWaitMs, polls.back() - polls.front());
  TEST_ASSERT_EQUAL_STRING("input 0x00 reads 0x0001 3 s after 0xB007", st.detail);
}

void test_cancel_after_entry_before_erase() {
  FakeIheater f;
  f.cancelInBootloader = true;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::Cancelled, FlashPhase::Entering);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Chunk));
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_FALSE(retryAllowed(st.outcome));
}

void test_cancel_while_polling_for_bootloader() {
  FakeIheater f;
  f.inject(Frame::Entry, Fault::DropRequest, 1, 0);
  f.cancelAtMs = f.clock + 5 * kJobReplyMs + 1000;  // a second into the polls
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::Cancelled, FlashPhase::Entering);
  TEST_ASSERT_TRUE(f.count(Frame::ReadState) < 1 + 31);
  TEST_ASSERT_TRUE(applicationIntact(st));
}

void test_cancel_in_bootloader_before_erase() {
  FakeIheater f;
  f.state = State::Bootloader;
  f.cancel = true;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::Cancelled, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(0, f.count(Frame::Erase));
}

void test_cancel_ignored_after_erase() {
  FakeIheater f;
  f.state = State::Bootloader;
  f.cancelAtMs = f.clock + 1;
  f.inject(Frame::Erase, Fault::DropRequest);  // the lost try moves the clock on
  updated(f, testImage());
}

// ---- Timeouts per frame ----

void assertTimeouts(const FakeIheater &f) {
  for (const auto &l : f.log) {
    const bool longWait = l.kind == Frame::Erase || l.kind == Frame::Boot;
    TEST_ASSERT_EQUAL(longWait ? kLongReplyMs : kJobReplyMs, l.timeoutMs);
  }
}

void test_erase_waits_2s() {
  FakeIheater f;
  updated(f, testImage());
  TEST_ASSERT_EQUAL(2000, f.frames(Frame::Erase)[0]->timeoutMs);
  assertTimeouts(f);
}

void test_boot_waits_2s() {
  FakeIheater f;
  f.inject(Frame::Boot, Fault::DropReply, 1, 0);
  updated(f, testImage());
  for (const auto *l : f.frames(Frame::Boot)) TEST_ASSERT_EQUAL(2000, l->timeoutMs);
  assertTimeouts(f);
}

// ---- Erasing and writing ----

void test_erase_reply_lost_resent() {
  FakeIheater f;
  f.inject(Frame::Erase, Fault::DropReply);
  const FlashStatus st = updated(f, testImage());
  TEST_ASSERT_EQUAL(2, f.count(Frame::Erase));
  assertIdentical(f.frames(Frame::Erase));
  TEST_ASSERT_EQUAL(1, st.retries);
}

void test_erase_refused_reports_last_error() {
  FakeIheater f;
  f.eraseRefuseLastError = 2;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::EraseRefused, FlashPhase::Erasing);
  TEST_ASSERT_EQUAL(2, st.lastError);
  TEST_ASSERT_EQUAL_STRING("erase refused: last_error 2", st.detail);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Erase));  // an exception is never resent
  TEST_ASSERT_EQUAL(0, f.count(Frame::Chunk));
  TEST_ASSERT_FALSE(applicationIntact(st));
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
}

void test_chunk_reply_lost_resent() {
  FakeIheater f;
  f.inject(Frame::Chunk, Fault::DropReply, 1, 1, 5);
  const FlashStatus st = updated(f, testImage());
  const auto sent = f.frames(Frame::Chunk, 5);
  TEST_ASSERT_EQUAL(2, sent.size());
  assertIdentical(sent);
  TEST_ASSERT_EQUAL(1, st.retries);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Erase));
}

void test_chunk_reply_damaged_resent() {
  FakeIheater f;
  f.inject(Frame::Chunk, Fault::DamageReply, 1, 1, 5);
  const FlashStatus st = updated(f, testImage());
  const auto sent = f.frames(Frame::Chunk, 5);
  TEST_ASSERT_EQUAL(2, sent.size());
  assertIdentical(sent);
  TEST_ASSERT_EQUAL(1, st.retries);
}

void test_every_step_survives_a_damaged_reply() {
  const Frame kinds[] = {Frame::ReadState, Frame::ReadOther, Frame::ReadBlock, Frame::Erase, Frame::Chunk};
  for (Frame kind : kinds) {
    FakeIheater f;
    f.inject(kind, Fault::DamageReply);
    const FlashStatus st = updated(f, testImage());
    TEST_ASSERT_EQUAL(1, st.retries);
  }
}

void test_not_blank_restarts_once() {
  FakeIheater f;
  f.notBlankOnce = {3};
  const FlashStatus st = updated(f, testImage());
  TEST_ASSERT_EQUAL(2, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(3, st.lastError);
  // The restart wrote chunks 0-3 again, then the rest.
  TEST_ASSERT_EQUAL(4 + 24, f.count(Frame::Chunk));
}

void test_not_blank_twice_stops() {
  FakeIheater f;
  f.notBlankOnce = {3, 3};
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::WriteRefused, FlashPhase::Writing);
  TEST_ASSERT_EQUAL(3, st.lastError);
  TEST_ASSERT_EQUAL(2, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Boot));
  TEST_ASSERT_EQUAL_STRING("chunk 3: not blank (last_error 3)", st.detail);
}

void test_chunk_other_exception_stops() {
  FakeIheater f;
  f.chunkExceptionAt = 3;
  f.chunkExceptionCode = 2;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::WriteRefused, FlashPhase::Writing);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(1, f.count(Frame::Chunk, 3));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Boot));
  TEST_ASSERT_EQUAL_STRING("chunk 3 refused: exception 0x02", st.detail);
}

void test_chunk_device_failure_other_error_stops() {
  FakeIheater f;
  f.chunkExceptionAt = 3;
  f.chunkExceptionCode = 4;  // with last_error 4: program or verify failed
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::WriteRefused, FlashPhase::Writing);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(4, st.lastError);
  TEST_ASSERT_EQUAL_STRING("chunk 3 refused: last_error 4", st.detail);
}

// ---- Starting and restarting ----

void test_boot_refused_restarts_once() {
  FakeIheater f;
  f.bootOk = {false, true};
  const FlashStatus st = updated(f, testImage());
  TEST_ASSERT_EQUAL(2, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(2, f.count(Frame::Boot));
  TEST_ASSERT_EQUAL(1, f.appCheckReads);
  TEST_ASSERT_EQUAL(48, f.count(Frame::Chunk));
  TEST_ASSERT_EQUAL(4, st.appCheck);
}

void test_boot_refused_twice_stops() {
  FakeIheater f;
  f.bootOk = {false, false};
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::BootRefused, FlashPhase::Starting);
  TEST_ASSERT_EQUAL(4, st.appCheck);
  TEST_ASSERT_EQUAL(2, f.count(Frame::Erase));
  TEST_ASSERT_EQUAL(2, f.appCheckReads);
  TEST_ASSERT_EQUAL_STRING("boot refused: app_check 4", st.detail);
  TEST_ASSERT_FALSE(applicationIntact(st));
}

void test_restart_shared() {
  FakeIheater f;
  f.notBlankOnce = {3};
  f.bootOk = {false};
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::BootRefused, FlashPhase::Starting);
  TEST_ASSERT_EQUAL(2, f.count(Frame::Erase));
}

void test_boot_reply_lost_polls_for_app() {
  FakeIheater f;
  f.inject(Frame::Boot, Fault::DropReply, 1, 0);
  const FlashStatus st = updated(f, testImage());
  TEST_ASSERT_EQUAL(5, f.count(Frame::Boot));
  TEST_ASSERT_EQUAL(4, st.retries);
}

void test_boot_resend_lands_in_app() {
  FakeIheater f;
  f.appBootMs = 1000;  // an app up before the 2 s resend, so the resend lands in it
  f.inject(Frame::Boot, Fault::DropReply);
  const FlashStatus st = updated(f, testImage());
  const auto boots = f.frames(Frame::Boot);
  TEST_ASSERT_EQUAL(2, boots.size());
  assertIdentical(boots);
  TEST_ASSERT_TRUE(boots[1]->stateAtSend == State::App);  // the app answers exception 0x02
  TEST_ASSERT_EQUAL(1, st.retries);
}

void test_app_never_answers() {
  FakeIheater f;
  f.appSilentAfterBoot = true;
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::NoAppAfterUpdate, FlashPhase::Restarting);
  size_t restarting = 0;
  while (f.published[restarting].phase != FlashPhase::Restarting) restarting++;
  const uint32_t waited = f.clock - f.publishedAt[restarting];
  TEST_ASSERT_TRUE(waited >= kAppWaitMs);
  TEST_ASSERT_TRUE(waited < kAppWaitMs + kJobReplyMs + kEntryPollMs);
  TEST_ASSERT_FALSE(applicationIntact(st));
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
  TEST_ASSERT_EQUAL_STRING("no reply 15 s after boot", st.detail);
}

// The real app is silent for about 5 s after the boot command (reset_LEDs(10),
// init, then a flush of the link), so the job must keep polling past that.
void test_app_silent_while_booting_still_updates() {
  FakeIheater f;
  f.appBootMs = 5500;
  updated(f, testImage());
  const auto boots = f.frames(Frame::Boot);
  TEST_ASSERT_EQUAL(1, boots.size());
  const uint32_t bootAt = boots[0]->clockAtSend;
  size_t unanswered = 0;
  for (const auto *l : f.frames(Frame::ReadState))
    if (l->clockAtSend >= bootAt && l->clockAtSend < bootAt + f.appBootMs) unanswered++;
  TEST_ASSERT_TRUE(unanswered >= 9);  // one poll every 600 ms through the silence
  TEST_ASSERT_TRUE(f.frames(Frame::ReadState).back()->clockAtSend >= bootAt + f.appBootMs);
}

void test_no_reply_gives_up_after_5() {
  FakeIheater f;
  f.inject(Frame::Chunk, Fault::DropRequest, 1, 0, 3);
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::NoReply, FlashPhase::Writing);
  TEST_ASSERT_EQUAL(5, f.count(Frame::Chunk, 3));
  assertIdentical(f.frames(Frame::Chunk, 3));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Chunk, 4));
  TEST_ASSERT_EQUAL(4, st.retries);
  TEST_ASSERT_EQUAL_STRING("chunk 3: no reply", st.detail);
  TEST_ASSERT_TRUE(retryAllowed(st.outcome));
}

// ---- Image limits ----

void test_largest_image_ends_at_chunk_223() {
  FakeIheater f;
  const auto img = testImage(kMaxImageBytes);
  updated(f, img);
  const auto chunks = f.frames(Frame::Chunk);
  TEST_ASSERT_EQUAL(224, chunks.size());
  TEST_ASSERT_EQUAL(223, chunks.back()->chunk);
  for (const auto *c : chunks) TEST_ASSERT_TRUE(c->chunk <= 223);
}

void test_oversize_sends_nothing() {
  FakeIheater f;
  const auto img = testImage(kMaxImageBytes + 1);
  const FlashStatus st = run(f, img);
  assertEnded(f, st, FlashOutcome::ImageRejected, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(0, f.log.size());
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_FALSE(retryAllowed(st.outcome));
  TEST_ASSERT_EQUAL_STRING("image over 28672 B", st.detail);
  const FlashStatus none = runFlashJob(f, nullptr, 0);
  TEST_ASSERT_EQUAL((int)FlashOutcome::ImageRejected, (int)none.outcome);
  TEST_ASSERT_EQUAL(0, f.log.size());
}

void test_rom_build_sends_nothing() {
  FakeIheater f;
  const FlashStatus st = run(f, testImage(3000, 1));
  assertEnded(f, st, FlashOutcome::ImageRejected, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(0, f.log.size());
  TEST_ASSERT_EQUAL_STRING("image is build_type 1, not 2", st.detail);
}

void test_bad_checksum_sends_nothing() {
  FakeIheater f;
  auto img = testImage();
  img[2000] ^= 1;
  const FlashStatus st = run(f, img);
  assertEnded(f, st, FlashOutcome::ImageRejected, FlashPhase::Idle);
  TEST_ASSERT_EQUAL(0, f.log.size());
}

// ---- Cooling ----

namespace {

void assertStoppedInApp(const FakeIheater &f, const FlashStatus &st, FlashOutcome want, FlashPhase phase) {
  assertEnded(f, st, want, phase);
  TEST_ASSERT_EQUAL(0, f.count(Frame::Entry));
  TEST_ASSERT_TRUE(applicationIntact(st));
  TEST_ASSERT_TRUE(f.state == State::App);
}

}  // namespace

void test_cooling_writes_target_only_when_nonzero() {
  FakeIheater f;
  f.airTargetT10 = 600;
  updated(f, testImage());
  TEST_ASSERT_EQUAL(1, f.targetWrites);
  TEST_ASSERT_EQUAL(0, f.lastTargetWritten);
  TEST_ASSERT_EQUAL(1, f.count(Frame::Entry));
}

void test_cooling_waits_until_50C() {
  FakeIheater f;
  f.heaterSeq = {800, 650, 501, 500};
  const uint32_t start = f.clock;
  const FlashStatus st = updated(f, testImage());
  TEST_ASSERT_EQUAL(1, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(500, st.heaterT10);
  std::vector<int16_t> seen;
  for (const FlashStatus &s : f.published)
    if (s.phase == FlashPhase::Cooling && s.heaterT10 != kNoReading && (seen.empty() || seen.back() != s.heaterT10))
      seen.push_back(s.heaterT10);
  const int16_t wantSeen[] = {800, 650, 501, 500};
  TEST_ASSERT_EQUAL(4, seen.size());
  for (size_t i = 0; i < 4; i++) TEST_ASSERT_EQUAL(wantSeen[i], seen[i]);
  // Three 5 s sleeps sit between the four reads; 0xB007 follows the last.
  TEST_ASSERT_EQUAL(start + 3 * kCoolPollMs, f.frames(Frame::Entry)[0]->clockAtSend);
}

void test_cooling_times_out_after_30_min() {
  FakeIheater f;
  f.heaterT10 = 600;
  const uint32_t start = f.clock;
  const FlashStatus st = run(f, testImage());
  assertStoppedInApp(f, st, FlashOutcome::CoolTimeout, FlashPhase::Cooling);
  TEST_ASSERT_TRUE(f.clock - start >= kCoolTimeoutMs);
  TEST_ASSERT_TRUE(f.clock - start < kCoolTimeoutMs + 2 * kCoolPollMs);
}

void test_cancel_during_cooling() {
  FakeIheater f;
  f.heaterT10 = 600;
  f.cancelAtMs = f.clock + 10000;
  assertStoppedInApp(f, run(f, testImage()), FlashOutcome::Cancelled, FlashPhase::Cooling);
}

void test_heat_requested_during_cooling() {
  FakeIheater f;
  f.heaterT10 = 600;
  f.heatAtMs = f.clock + 10000;
  const FlashStatus st = run(f, testImage());
  assertStoppedInApp(f, st, FlashOutcome::HeatRequested, FlashPhase::Cooling);
  TEST_ASSERT_FALSE(retryAllowed(st.outcome));
}

void test_heat_requested_just_before_entry() {
  FakeIheater f;
  f.heaterSeq = {800, 500};
  f.heatAfterReads = 2;  // true once the 500 reading has been served
  assertStoppedInApp(f, run(f, testImage()), FlashOutcome::HeatRequested, FlashPhase::Entering);
}

void test_invalid_heater_reading_stops() {
  FakeIheater f;
  f.heaterT10 = kNoReading;
  assertStoppedInApp(f, run(f, testImage()), FlashOutcome::BadReading, FlashPhase::Cooling);
  FakeIheater g;
  g.airT10 = kNoReading;
  assertStoppedInApp(g, run(g, testImage()), FlashOutcome::BadReading, FlashPhase::Cooling);
}

void test_cooling_stops_if_the_app_is_gone() {
  FakeIheater f;
  f.heaterT10 = 600;
  f.resetAfterBlockReads = 1;  // the next block read finds the bootloader, temperatures 0
  const FlashStatus st = run(f, testImage());
  assertEnded(f, st, FlashOutcome::UnexpectedState, FlashPhase::Cooling);
  TEST_ASSERT_EQUAL_STRING("input 0x00 reads 0x8001", st.detail);
  TEST_ASSERT_EQUAL(0, f.count(Frame::Entry));
  TEST_ASSERT_EQUAL(0, f.count(Frame::Erase));
}

void test_cooling_stops_after_three_missed_reads() {
  FakeIheater f;
  f.heaterT10 = 600;
  f.inject(Frame::ReadBlock, Fault::DropRequest, 2, 0);
  const FlashStatus st = run(f, testImage());
  assertStoppedInApp(f, st, FlashOutcome::NoReplyCooling, FlashPhase::Cooling);
  // One good read, then three reads of five tries each.
  TEST_ASSERT_EQUAL(1 + 3 * kFlashTries, f.count(Frame::ReadBlock));
  TEST_ASSERT_EQUAL(1, f.blockReads);
}

// ---- Tables and start rules ----

void test_outcome_tables() {
  const char *names[] = {"none", "updated", "image-rejected", "no-reply", "unexpected-state",
                         "no-link-bootloader", "cancelled", "heat-requested", "cool-timeout", "bad-reading",
                         "no-reply-cooling", "entry-refused", "entry-not-confirmed", "erase-refused",
                         "write-refused", "boot-refused", "no-app-after-update"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    const FlashOutcome o = (FlashOutcome)i;
    TEST_ASSERT_EQUAL_STRING(names[i], flashOutcomeName(o));
    if (o == FlashOutcome::None)
      TEST_ASSERT_EQUAL_STRING("", flashOutcomeMessage(o));
    else
      TEST_ASSERT_TRUE(strlen(flashOutcomeMessage(o)) > 0);
  }
  const char *phases[] = {"idle", "cooling", "entering", "erasing", "writing", "starting", "restarting", "finished"};
  for (size_t i = 0; i < 8; i++) TEST_ASSERT_EQUAL_STRING(phases[i], flashPhaseName((FlashPhase)i));
  TEST_ASSERT_EQUAL_STRING("Stopped: heat requested.", flashOutcomeMessage(FlashOutcome::HeatRequested));
}

void test_heat_wanted_rule() {
  TEST_ASSERT_FALSE(heatWanted(true, 0.0f));
  TEST_ASSERT_TRUE(heatWanted(true, 0.5f));
  TEST_ASSERT_FALSE(heatWanted(false, 60.0f));
}

void test_decide_flash_start() {
  FlashStartInputs in;
  in.uartOutput = true;
  in.imageOk = true;
  in.link = LinkStatus::Up;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::None);

  // In the bootloader: no valid app (why 2) may be flashed; asked (why 1) is leaving.
  in.link = LinkStatus::Updating;
  in.inBootloader = true;
  in.bootloaderWhy = 2;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::None);
  in.bootloaderWhy = 1;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::BootloaderLeaving);
  in.bootloaderWhy = 0;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::LinkDown);
  in.inBootloader = false;
  in.link = LinkStatus::NoReply;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::LinkDown);

  // Refusal order: (RestartPending first), NotUart, JobRunning, NoImage, HeatWanted, then the link.
  FlashStartInputs all;
  all.jobRunning = true;
  all.heatWanted = true;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::NotUart);
  all.uartOutput = true;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::JobRunning);
  all.jobRunning = false;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::NoImage);
  all.imageOk = true;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::HeatWanted);
  all.inBootloader = true;
  all.bootloaderWhy = 2;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::HeatWanted);
  all.heatWanted = false;
  all.inBootloader = false;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::LinkDown);

  // A retry ignores the link; NothingToRetry takes LinkDown's place.
  in.lastOutcome = FlashOutcome::WriteRefused;
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::None);
  in.lastOutcome = FlashOutcome::Updated;
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::NothingToRetry);
  in.lastOutcome = FlashOutcome::WriteRefused;
  in.heatWanted = true;
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::HeatWanted);
  in.heatWanted = false;
  in.uartOutput = false;
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::NotUart);
  in.uartOutput = true;
  in.imageOk = false;
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::NoImage);
}

void test_restart_pending_refuses_every_start() {
  FlashStartInputs in;
  in.uartOutput = true;
  in.imageOk = true;
  in.link = LinkStatus::Up;
  in.lastOutcome = FlashOutcome::NoReply;
  in.restartPending = true;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::RestartPending);
  in.link = LinkStatus::Updating;
  in.inBootloader = true;
  in.bootloaderWhy = 2;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::RestartPending);
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::RestartPending);
  in.restartPending = false;
  TEST_ASSERT_TRUE(decideFlashStart(in, false) == FlashRefusal::None);
  TEST_ASSERT_TRUE(decideFlashStart(in, true) == FlashRefusal::None);
  FlashStartInputs all;
  all.jobRunning = true;
  all.heatWanted = true;
  all.restartPending = true;
  TEST_ASSERT_TRUE(decideFlashStart(all, false) == FlashRefusal::RestartPending);
  TEST_ASSERT_TRUE(decideFlashStart(all, true) == FlashRefusal::RestartPending);
}

void test_refusal_messages() {
  TEST_ASSERT_EQUAL_STRING("", flashRefusalMessage(FlashRefusal::None));
  TEST_ASSERT_EQUAL_STRING("Switch the output to UART first.", flashRefusalMessage(FlashRefusal::NotUart));
  TEST_ASSERT_EQUAL_STRING("Upload a firmware file that passes the check first.",
                           flashRefusalMessage(FlashRefusal::NoImage));
  TEST_ASSERT_EQUAL_STRING("Turn the heat off first: Manual Off, or a Klipper target of 0.",
                           flashRefusalMessage(FlashRefusal::HeatWanted));
  TEST_ASSERT_EQUAL_STRING("An iHeater update is already running.", flashRefusalMessage(FlashRefusal::JobRunning));
  TEST_ASSERT_EQUAL_STRING("There's no failed update to retry.", flashRefusalMessage(FlashRefusal::NothingToRetry));
  TEST_ASSERT_EQUAL_STRING("The Remote is about to restart. Try again in a few seconds.",
                           flashRefusalMessage(FlashRefusal::RestartPending));
  TEST_ASSERT_EQUAL_STRING(
      "The iHeater is in its bootloader and returns to its firmware by itself within 30 s. Try again then.",
      flashRefusalMessage(FlashRefusal::BootloaderLeaving));
  TEST_ASSERT_TRUE(strlen(flashRefusalMessage(FlashRefusal::LinkDown)) > 0);
}

void test_retry_allowed() {
  const FlashOutcome no[] = {FlashOutcome::None, FlashOutcome::Updated, FlashOutcome::Cancelled,
                             FlashOutcome::HeatRequested, FlashOutcome::ImageRejected,
                             FlashOutcome::NoLinkBootloader};
  for (FlashOutcome o : no) TEST_ASSERT_FALSE_MESSAGE(retryAllowed(o), flashOutcomeName(o));
  const FlashOutcome yes[] = {FlashOutcome::NoReply,        FlashOutcome::UnexpectedState,
                              FlashOutcome::CoolTimeout,    FlashOutcome::BadReading,
                              FlashOutcome::NoReplyCooling, FlashOutcome::EntryRefused,
                              FlashOutcome::EntryNotConfirmed, FlashOutcome::EraseRefused,
                              FlashOutcome::WriteRefused,   FlashOutcome::BootRefused,
                              FlashOutcome::NoAppAfterUpdate};
  for (FlashOutcome o : yes) TEST_ASSERT_TRUE_MESSAGE(retryAllowed(o), flashOutcomeName(o));
}

void test_application_intact() {
  FlashStatus s;
  s.phase = FlashPhase::Finished;
  s.outcome = FlashOutcome::Updated;
  s.failedPhase = FlashPhase::Idle;
  TEST_ASSERT_TRUE(applicationIntact(s));
  s.outcome = FlashOutcome::NoReply;
  const FlashPhase intact[] = {FlashPhase::Idle, FlashPhase::Cooling, FlashPhase::Entering};
  for (FlashPhase p : intact) {
    s.failedPhase = p;
    TEST_ASSERT_TRUE(applicationIntact(s));
  }
  const FlashPhase gone[] = {FlashPhase::Erasing, FlashPhase::Writing, FlashPhase::Starting, FlashPhase::Restarting};
  for (FlashPhase p : gone) {
    s.failedPhase = p;
    TEST_ASSERT_FALSE(applicationIntact(s));
  }
}

// ---- The fake itself ----

void test_fake_flags_bad_frames() {
  FakeIheater f;
  uint8_t frame[kRequestLen];
  buildReadRequest(kFcReadInput, 0, 1, frame);
  Reply reply;
  TEST_ASSERT_TRUE(f.exchange(frame, sizeof frame, reply, kJobReplyMs));
  TEST_ASSERT_FALSE(f.violation);
  frame[7] ^= 1;  // bad CRC
  TEST_ASSERT_FALSE(f.exchange(frame, sizeof frame, reply, kJobReplyMs));
  TEST_ASSERT_TRUE(f.violation);

  FakeIheater g;
  uint16_t values[66] = {224};
  uint8_t big[160];
  const size_t n = buildWriteMultiple(kRegChunk, values, 65, big, sizeof big);
  g.exchange(big, n, reply, kJobReplyMs);
  TEST_ASSERT_TRUE(g.violation);  // index above 223

  FakeIheater h;
  const size_t m = buildWriteMultiple(kRegChunk, values + 1, 64, big, sizeof big);
  h.exchange(big, m, reply, kJobReplyMs);
  TEST_ASSERT_TRUE(h.violation);  // 64 registers, not 65

  FakeIheater k;  // 0xB007 without a non-zero read of 0x16 first
  buildWriteSingle(kRegBootloader, kBootloaderMagic, frame);
  k.exchange(frame, sizeof frame, reply, kJobReplyMs);
  TEST_ASSERT_TRUE(k.violation);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_updates_from_app);
  RUN_TEST(test_already_in_bootloader_skips_entry);
  RUN_TEST(test_old_firmware_refused_0x16);
  RUN_TEST(test_old_firmware_zero_0x16);
  RUN_TEST(test_unexpected_state);
  RUN_TEST(test_first_read_no_reply);
  RUN_TEST(test_entry_refused_on_first_try);
  RUN_TEST(test_entry_resend_reaches_bootloader);
  RUN_TEST(test_entry_request_lost_then_lands);
  RUN_TEST(test_entry_all_unanswered_but_entered);
  RUN_TEST(test_entry_not_confirmed);
  RUN_TEST(test_cancel_after_entry_before_erase);
  RUN_TEST(test_cancel_while_polling_for_bootloader);
  RUN_TEST(test_cancel_in_bootloader_before_erase);
  RUN_TEST(test_cancel_ignored_after_erase);
  RUN_TEST(test_erase_waits_2s);
  RUN_TEST(test_boot_waits_2s);
  RUN_TEST(test_erase_reply_lost_resent);
  RUN_TEST(test_erase_refused_reports_last_error);
  RUN_TEST(test_chunk_reply_lost_resent);
  RUN_TEST(test_chunk_reply_damaged_resent);
  RUN_TEST(test_every_step_survives_a_damaged_reply);
  RUN_TEST(test_not_blank_restarts_once);
  RUN_TEST(test_not_blank_twice_stops);
  RUN_TEST(test_chunk_other_exception_stops);
  RUN_TEST(test_chunk_device_failure_other_error_stops);
  RUN_TEST(test_boot_refused_restarts_once);
  RUN_TEST(test_boot_refused_twice_stops);
  RUN_TEST(test_restart_shared);
  RUN_TEST(test_boot_reply_lost_polls_for_app);
  RUN_TEST(test_boot_resend_lands_in_app);
  RUN_TEST(test_app_never_answers);
  RUN_TEST(test_app_silent_while_booting_still_updates);
  RUN_TEST(test_no_reply_gives_up_after_5);
  RUN_TEST(test_largest_image_ends_at_chunk_223);
  RUN_TEST(test_oversize_sends_nothing);
  RUN_TEST(test_rom_build_sends_nothing);
  RUN_TEST(test_bad_checksum_sends_nothing);
  RUN_TEST(test_cooling_writes_target_only_when_nonzero);
  RUN_TEST(test_cooling_waits_until_50C);
  RUN_TEST(test_cooling_times_out_after_30_min);
  RUN_TEST(test_cancel_during_cooling);
  RUN_TEST(test_heat_requested_during_cooling);
  RUN_TEST(test_heat_requested_just_before_entry);
  RUN_TEST(test_invalid_heater_reading_stops);
  RUN_TEST(test_cooling_stops_if_the_app_is_gone);
  RUN_TEST(test_cooling_stops_after_three_missed_reads);
  RUN_TEST(test_outcome_tables);
  RUN_TEST(test_heat_wanted_rule);
  RUN_TEST(test_decide_flash_start);
  RUN_TEST(test_restart_pending_refuses_every_start);
  RUN_TEST(test_refusal_messages);
  RUN_TEST(test_retry_allowed);
  RUN_TEST(test_application_intact);
  RUN_TEST(test_fake_flags_bad_frames);
  return UNITY_END();
}
