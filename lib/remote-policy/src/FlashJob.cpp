#include "FlashJob.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FirmwareImage.h"

namespace iheaterlink::policy {

namespace {

constexpr uint16_t kAppProtocol = 1;                          // the app's input 0x00
constexpr uint8_t kChunkRegs = 1 + kChunkBytes / 2;           // index, then 64 data registers
static_assert(kChunkRegs == 65 && kChunkRegs <= kMaxWriteRegs, "a chunk is one 65-register FC10 write");
static_assert(kMaxImageBytes == (size_t)(kMaxChunkIndex + 1) * kChunkBytes, "the largest image ends at chunk 223");

// How one exchange, with its tries, ended.
enum class Got : uint8_t { Ok, Exception, NoReply };
// What the job does after an erase, chunk or boot step.
enum class Step : uint8_t { Next, Restart, Stop };
enum class Wait : uint8_t { Reached, TimedOut, Cancelled };

#define FLASH_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))

class Job {
 public:
  Job(FlashLink &link, const uint8_t *image, size_t len) : link_(link), image_(image), len_(len) {}

  FlashStatus run() {
    if (!imageOk()) return fail(FlashOutcome::ImageRejected);
    uint16_t state = 0;
    uint8_t exc = 0;
    switch (readInput(kInputBase, state, exc)) {
      case Got::NoReply: return failf(FlashOutcome::NoReply, "input 0x00: no reply");
      case Got::Exception:
        return failf(FlashOutcome::UnexpectedState, "input 0x00: exception 0x%02X", (unsigned)exc);
      case Got::Ok: break;
    }
    if (state == kAppProtocol) {
      if (!hasLinkBootloader() || !cool() || !enterBootloader()) return st;
    } else if (state != kBootloaderProtocol) {
      return failf(FlashOutcome::UnexpectedState, "input 0x00 reads 0x%04X", (unsigned)state);
    }
    // The last point a cancel is honoured: nothing is erased yet, and a
    // bootloader entered with 0xB007 returns to the app by itself when no
    // erase comes.
    if (link_.cancelRequested()) return fail(FlashOutcome::Cancelled);

    restarts_ = kRestarts;
    for (;;) {
      Step s = erase();
      if (s == Step::Next) s = writeChunks();
      if (s == Step::Next) s = boot();
      if (s == Step::Stop) return st;
      if (s == Step::Next) break;
    }
    return awaitApp();
  }

 private:
  FlashLink &link_;
  const uint8_t *image_;
  size_t len_;
  FlashStatus st;
  uint8_t restarts_ = 0;       // left for not-blank and boot-refused together
  bool lastAnswered_ = false;  // the last poll of input 0x00 got a value...
  uint16_t lastState_ = 0;     // ...and this was it

  // ---- Status ----

  void phase(FlashPhase p) {
    st.phase = p;
    link_.publish(st);
  }

  FlashStatus finish(FlashOutcome outcome) {
    st.outcome = outcome;
    st.phase = FlashPhase::Finished;
    link_.publish(st);
    return st;
  }

  // Ends the job at the phase it was in, with the current detail text.
  FlashStatus fail(FlashOutcome outcome) {
    st.failedPhase = st.phase;
    return finish(outcome);
  }

  FLASH_PRINTF(3, 4) FlashStatus failf(FlashOutcome outcome, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(st.detail, sizeof st.detail, fmt, args);
    va_end(args);
    return fail(outcome);
  }

  // ---- Frames ----

  // Sends the frame up to kFlashTries times while no usable reply comes back
  // (none in time, or a damaged one). True with an Ok or Exception reply.
  bool tries(const uint8_t *frame, size_t len, uint32_t timeoutMs, Reply &reply) {
    for (uint8_t i = 0; i < kFlashTries; i++) {
      if (i > 0 && st.retries < UINT16_MAX) st.retries++;
      if (link_.exchange(frame, len, reply, timeoutMs) &&
          (reply.kind == ReplyKind::Ok || reply.kind == ReplyKind::Exception))
        return true;
    }
    return false;
  }

  Got judge(bool got, const Reply &reply, uint8_t &exc) {
    if (!got) return Got::NoReply;
    if (reply.kind == ReplyKind::Exception) {
      exc = reply.exception;
      return Got::Exception;
    }
    return Got::Ok;
  }

  Got readInput(uint16_t addr, uint16_t &value, uint8_t &exc) {
    uint8_t frame[kRequestLen];
    buildReadRequest(kFcReadInput, addr, 1, frame);
    Reply reply;
    const Got got = judge(tries(frame, sizeof frame, kJobReplyMs, reply), reply, exc);
    if (got == Got::Ok) value = reply.regs[0];
    return got;
  }

  Got writeRegister(uint16_t addr, uint16_t value, uint32_t timeoutMs, uint8_t &exc) {
    uint8_t frame[kRequestLen];
    buildWriteSingle(addr, value, frame);
    Reply reply;
    return judge(tries(frame, sizeof frame, timeoutMs, reply), reply, exc);
  }

  // A bootloader input read for a failure's detail (last_error, app_check).
  bool readDiagnostic(uint16_t addr, uint16_t &value) {
    uint8_t exc = 0;
    return readInput(addr, value, exc) == Got::Ok;
  }

  // Polls input 0x00, one read every kEntryPollMs and no resends, until it
  // reads `want` or waitMs has passed.
  Wait waitForState(uint16_t want, uint32_t waitMs, bool cancellable) {
    uint8_t frame[kRequestLen];
    buildReadRequest(kFcReadInput, kInputBase, 1, frame);
    const uint32_t start = link_.nowMs();
    for (;;) {
      if (cancellable && link_.cancelRequested()) return Wait::Cancelled;
      Reply reply;
      lastAnswered_ = link_.exchange(frame, sizeof frame, reply, kJobReplyMs) && reply.kind == ReplyKind::Ok;
      if (lastAnswered_) {
        lastState_ = reply.regs[0];
        if (lastState_ == want) return Wait::Reached;
      }
      if ((uint32_t)(link_.nowMs() - start) >= waitMs) return Wait::TimedOut;
      link_.sleepMs(kEntryPollMs);
    }
  }

  // ---- Steps ----

  bool imageOk() {
    if (image_ == nullptr) {
      snprintf(st.detail, sizeof st.detail, "no image");
      return false;
    }
    const ImageInfo info = checkImage(image_, len_);
    if (info.result == ImageCheck::Ok && info.buildType == kBuildTypeApp && len_ <= kMaxImageBytes) return true;
    const char *text = "image check failed";
    switch (info.result) {
      case ImageCheck::TooSmall: text = "image too small"; break;
      case ImageCheck::TooLarge: text = "image over 28672 B"; break;
      case ImageCheck::BadStackPointer: text = "image: bad stack pointer"; break;
      case ImageCheck::BadResetVector: text = "image: bad reset vector"; break;
      case ImageCheck::NoRecord: text = "image has no build record"; break;
      case ImageCheck::BadRecord: text = "image: bad build record"; break;
      case ImageCheck::LengthMismatch: text = "image length doesn't match its record"; break;
      case ImageCheck::BadChecksum: text = "image checksum is wrong"; break;
      case ImageCheck::UnsupportedProtocol: text = "image: unsupported link protocol"; break;
      case ImageCheck::WrongBuildType:
        snprintf(st.detail, sizeof st.detail, "image is build_type %u, not %u", (unsigned)info.buildType,
                 (unsigned)kBuildTypeApp);
        return false;
      case ImageCheck::Ok: break;
    }
    snprintf(st.detail, sizeof st.detail, "%s", text);
    return false;
  }

  // Input 0x16 names the link bootloader. Firmware without one refuses the
  // read or returns 0, and there 0xB007 would wipe the app: never send it.
  bool hasLinkBootloader() {
    uint16_t version = 0;
    uint8_t exc = 0;
    switch (readInput(kRegBootloaderVersion, version, exc)) {
      case Got::NoReply: failf(FlashOutcome::NoReply, "input 0x16: no reply"); return false;
      case Got::Exception:
        failf(FlashOutcome::NoLinkBootloader, "input 0x16 refused: exception 0x%02X", (unsigned)exc);
        return false;
      case Got::Ok: break;
    }
    if (version != 0) return true;
    failf(FlashOutcome::NoLinkBootloader, "input 0x16 reads 0");
    return false;
  }

  // Waits for the heater to cool, with the air target at 0. True when it is
  // safe to enter the bootloader; otherwise the job has ended.
  bool cool() {
    phase(FlashPhase::Cooling);
    uint8_t frame[kRequestLen];
    buildReadRequest(kFcReadInput, kInputBase, kInputCount, frame);
    const uint32_t start = link_.nowMs();
    uint8_t misses = 0;
    for (;;) {
      if (link_.cancelRequested()) return stop(FlashOutcome::Cancelled);
      if (link_.heatWanted()) return stop(FlashOutcome::HeatRequested);
      Reply reply;
      IheaterSnapshot snap;
      const bool ok = tries(frame, sizeof frame, kJobReplyMs, reply) && reply.kind == ReplyKind::Ok &&
                      decodeInputs(reply.regs, reply.count, snap);
      if (!ok) {
        if (++misses >= kCoolMissLimit) return stop(FlashOutcome::NoReplyCooling);
      } else {
        misses = 0;
        // Anything but the app here (the bootloader reads 0 at the
        // temperatures) must not pass for a cool heater.
        if (snap.proto != kAppProtocol) {
          failf(FlashOutcome::UnexpectedState, "input 0x00 reads 0x%04X", (unsigned)snap.proto);
          return false;
        }
        st.heaterT10 = snap.heaterT10;
        link_.publish(st);
        if (snap.heaterT10 == kNoReading || snap.airT10 == kNoReading) return stop(FlashOutcome::BadReading);
        if (snap.airTargetT10 != 0) {
          uint8_t exc = 0;  // a lost or refused write shows up as a non-zero target on the next read
          writeRegister(kRegTarget, 0, kJobReplyMs, exc);
        } else if (snap.heaterT10 <= kSafeHeaterT10) {
          return true;
        }
      }
      if ((uint32_t)(link_.nowMs() - start) >= kCoolTimeoutMs) return stop(FlashOutcome::CoolTimeout);
      link_.sleepMs(kCoolPollMs);
    }
  }

  bool stop(FlashOutcome outcome) {
    fail(outcome);
    return false;
  }

  // Writes 0xB007 as linkboot_flash.py's _enter does, then waits for input
  // 0x00 to read 0x8001. Once a try has gone unanswered the board may have
  // taken it and reset, so an exception to a resend (the bootloader answers
  // 0x0102 with 0x02) means it arrived; only an exception to the first try is
  // a refusal.
  bool enterBootloader() {
    phase(FlashPhase::Entering);
    if (link_.cancelRequested()) return stop(FlashOutcome::Cancelled);
    if (link_.heatWanted()) return stop(FlashOutcome::HeatRequested);
    uint8_t frame[kRequestLen];
    buildWriteSingle(kRegBootloader, kBootloaderMagic, frame);
    bool unanswered = false, answered = false;
    for (uint8_t i = 0; i < kFlashTries && !answered; i++) {
      if (i > 0 && st.retries < UINT16_MAX) st.retries++;
      Reply reply;
      if (!link_.exchange(frame, sizeof frame, reply, kJobReplyMs) ||
          (reply.kind != ReplyKind::Ok && reply.kind != ReplyKind::Exception)) {
        unanswered = true;
        continue;
      }
      answered = true;
      if (reply.kind == ReplyKind::Exception && !unanswered) {
        failf(FlashOutcome::EntryRefused, "0xB007 refused: exception 0x%02X", (unsigned)reply.exception);
        return false;
      }
    }
    st.entryUnconfirmed = !answered;
    link_.publish(st);
    switch (waitForState(kBootloaderProtocol, kEntryWaitMs, true)) {
      case Wait::Reached: return true;
      case Wait::Cancelled: return stop(FlashOutcome::Cancelled);
      case Wait::TimedOut: break;
    }
    if (lastAnswered_)
      failf(FlashOutcome::EntryNotConfirmed, "input 0x00 reads 0x%04X 3 s after 0xB007", (unsigned)lastState_);
    else
      failf(FlashOutcome::EntryNotConfirmed, "no reply 3 s after 0xB007");
    return false;
  }

  FLASH_PRINTF(3, 4) Step stopStep(FlashOutcome outcome, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(st.detail, sizeof st.detail, fmt, args);
    va_end(args);
    fail(outcome);
    return Step::Stop;
  }

  Step erase() {
    st.progressPermille = 0;
    phase(FlashPhase::Erasing);
    uint8_t exc = 0;
    switch (writeRegister(kRegCommand, kCmdErase, kLongReplyMs, exc)) {
      case Got::Ok: return Step::Next;
      case Got::NoReply: return stopStep(FlashOutcome::NoReply, "erase: no reply");
      case Got::Exception: break;
    }
    uint16_t lastError = 0;
    if (!readDiagnostic(kBlLastError, lastError))
      return stopStep(FlashOutcome::EraseRefused, "erase refused: exception 0x%02X", (unsigned)exc);
    st.lastError = lastError;
    return stopStep(FlashOutcome::EraseRefused, "erase refused: last_error %u", (unsigned)lastError);
  }

  // One FC10 write per 128-byte chunk: the index, then the bytes in file
  // order, two per register, first byte high; the last chunk padded with 0xFF.
  Step writeChunks() {
    phase(FlashPhase::Writing);
    const size_t total = (len_ + kChunkBytes - 1) / kChunkBytes;
    uint16_t regs[kChunkRegs];
    uint8_t frame[kMaxFrameLen];
    for (size_t index = 0; index < total; index++) {
      if (index > kMaxChunkIndex)  // the image check makes this unreachable
        return stopStep(FlashOutcome::WriteRefused, "chunk %u: over index 223", (unsigned)index);
      regs[0] = (uint16_t)index;
      for (size_t i = 0; i < kChunkBytes / 2; i++) {
        const size_t at = index * kChunkBytes + 2 * i;
        const uint8_t hi = at < len_ ? image_[at] : 0xFF, lo = at + 1 < len_ ? image_[at + 1] : 0xFF;
        regs[1 + i] = (uint16_t)((hi << 8) | lo);
      }
      const size_t n = buildWriteMultiple(kRegChunk, regs, kChunkRegs, frame, sizeof frame);
      Reply reply;
      if (n == 0 || !tries(frame, n, kJobReplyMs, reply))
        return stopStep(FlashOutcome::NoReply, "chunk %u: no reply", (unsigned)index);
      if (reply.kind == ReplyKind::Exception) {
        const unsigned exc = reply.exception;
        if (exc != kExcDeviceFailure)
          return stopStep(FlashOutcome::WriteRefused, "chunk %u refused: exception 0x%02X", (unsigned)index, exc);
        uint16_t lastError = 0;
        if (!readDiagnostic(kBlLastError, lastError))
          return stopStep(FlashOutcome::WriteRefused, "chunk %u refused: exception 0x%02X", (unsigned)index, exc);
        st.lastError = lastError;
        if (lastError == kLbErrNotBlank && restarts_ > 0) {
          restarts_--;
          return Step::Restart;
        }
        if (lastError == kLbErrNotBlank)
          return stopStep(FlashOutcome::WriteRefused, "chunk %u: not blank (last_error 3)", (unsigned)index);
        return stopStep(FlashOutcome::WriteRefused, "chunk %u refused: last_error %u", (unsigned)index,
                        (unsigned)lastError);
      }
      st.progressPermille = (uint16_t)(((index + 1) * 1000u) / total);
      link_.publish(st);
    }
    return Step::Next;
  }

  // An exception 0x04 is the bootloader refusing the image. No usable reply,
  // or another exception (a resend that reached the app already running),
  // leaves it to the poll for the app.
  Step boot() {
    phase(FlashPhase::Starting);
    uint8_t exc = 0;
    if (writeRegister(kRegCommand, kCmdBoot, kLongReplyMs, exc) != Got::Exception || exc != kExcDeviceFailure)
      return Step::Next;
    uint16_t check = 0;
    if (!readDiagnostic(kBlAppCheck, check))
      return stopStep(FlashOutcome::BootRefused, "boot refused: exception 0x%02X", (unsigned)exc);
    st.appCheck = check;
    if (restarts_ > 0) {
      restarts_--;
      return Step::Restart;
    }
    return stopStep(FlashOutcome::BootRefused, "boot refused: app_check %u", (unsigned)check);
  }

  FlashStatus awaitApp() {
    phase(FlashPhase::Restarting);
    if (waitForState(kAppProtocol, kAppWaitMs, false) == Wait::Reached) return finish(FlashOutcome::Updated);
    const unsigned waitS = (unsigned)(kAppWaitMs / 1000u);
    if (lastAnswered_)
      return failf(FlashOutcome::NoAppAfterUpdate, "input 0x00 reads 0x%04X %u s after boot", (unsigned)lastState_,
                   waitS);
    return failf(FlashOutcome::NoAppAfterUpdate, "no reply %u s after boot", waitS);
  }
};

#undef FLASH_PRINTF

}  // namespace

FlashStatus runFlashJob(FlashLink &link, const uint8_t *image, size_t len) {
  Job job(link, image, len);
  return job.run();
}

bool heatWanted(bool targetMode, float targetC) { return targetMode && targetC > 0.0f; }

FlashRefusal decideFlashStart(const FlashStartInputs &in, bool retry) {
  if (in.restartPending) return FlashRefusal::RestartPending;
  if (!in.uartOutput) return FlashRefusal::NotUart;
  if (in.jobRunning) return FlashRefusal::JobRunning;
  if (!in.imageOk) return FlashRefusal::NoImage;
  if (in.heatWanted) return FlashRefusal::HeatWanted;
  if (retry) return retryAllowed(in.lastOutcome) ? FlashRefusal::None : FlashRefusal::NothingToRetry;
  if (in.link == LinkStatus::Up) return FlashRefusal::None;
  if (in.inBootloader && in.bootloaderWhy == 2) return FlashRefusal::None;
  if (in.inBootloader && in.bootloaderWhy == 1) return FlashRefusal::BootloaderLeaving;
  return FlashRefusal::LinkDown;
}

const char *flashRefusalMessage(FlashRefusal r) {
  switch (r) {
    case FlashRefusal::None: return "";
    case FlashRefusal::NotUart: return "Switch the output to UART first.";
    case FlashRefusal::NoImage: return "Upload a firmware file that passes the check first.";
    case FlashRefusal::HeatWanted: return "Turn the heat off first: Manual Off, or a Klipper target of 0.";
    case FlashRefusal::JobRunning: return "An iHeater update is already running.";
    case FlashRefusal::LinkDown: return "The iHeater isn't answering, and its bootloader wasn't detected.";
    case FlashRefusal::BootloaderLeaving:
      return "The iHeater is in its bootloader and returns to its firmware by itself within 30 s. Try again then.";
    case FlashRefusal::NothingToRetry: return "There's no failed update to retry.";
    case FlashRefusal::RestartPending: return "The Remote is about to restart. Try again in a few seconds.";
  }
  return "";
}

bool retryAllowed(FlashOutcome o) {
  switch (o) {
    case FlashOutcome::None:
    case FlashOutcome::Updated:
    case FlashOutcome::Cancelled:
    case FlashOutcome::HeatRequested:
    case FlashOutcome::ImageRejected:
    case FlashOutcome::NoLinkBootloader: return false;
    default: return true;
  }
}

bool applicationIntact(const FlashStatus &s) {
  if (s.outcome == FlashOutcome::Updated) return true;
  return s.failedPhase == FlashPhase::Idle || s.failedPhase == FlashPhase::Cooling ||
         s.failedPhase == FlashPhase::Entering;
}

const char *flashPhaseName(FlashPhase p) {
  switch (p) {
    case FlashPhase::Idle: return "idle";
    case FlashPhase::Cooling: return "cooling";
    case FlashPhase::Entering: return "entering";
    case FlashPhase::Erasing: return "erasing";
    case FlashPhase::Writing: return "writing";
    case FlashPhase::Starting: return "starting";
    case FlashPhase::Restarting: return "restarting";
    case FlashPhase::Finished: return "finished";
  }
  return "idle";
}

const char *flashOutcomeName(FlashOutcome o) {
  switch (o) {
    case FlashOutcome::None: return "none";
    case FlashOutcome::Updated: return "updated";
    case FlashOutcome::ImageRejected: return "image-rejected";
    case FlashOutcome::NoReply: return "no-reply";
    case FlashOutcome::UnexpectedState: return "unexpected-state";
    case FlashOutcome::NoLinkBootloader: return "no-link-bootloader";
    case FlashOutcome::Cancelled: return "cancelled";
    case FlashOutcome::HeatRequested: return "heat-requested";
    case FlashOutcome::CoolTimeout: return "cool-timeout";
    case FlashOutcome::BadReading: return "bad-reading";
    case FlashOutcome::NoReplyCooling: return "no-reply-cooling";
    case FlashOutcome::EntryRefused: return "entry-refused";
    case FlashOutcome::EntryNotConfirmed: return "entry-not-confirmed";
    case FlashOutcome::EraseRefused: return "erase-refused";
    case FlashOutcome::WriteRefused: return "write-refused";
    case FlashOutcome::BootRefused: return "boot-refused";
    case FlashOutcome::NoAppAfterUpdate: return "no-app-after-update";
  }
  return "none";
}

const char *flashOutcomeMessage(FlashOutcome o) {
  switch (o) {
    case FlashOutcome::None: return "";
    case FlashOutcome::Updated: return "The iHeater was updated and is running the new firmware.";
    case FlashOutcome::ImageRejected: return "The image was rejected, so nothing was sent to the iHeater.";
    case FlashOutcome::NoReply: return "The iHeater stopped answering during the update. Check the wiring, then retry.";
    case FlashOutcome::UnexpectedState:
      return "The iHeater answered as neither its firmware nor its bootloader, so the update stopped.";
    case FlashOutcome::NoLinkBootloader:
      return "This iHeater has no link bootloader, so the Remote can't update it. Install the full image from a "
             "PC first. Nothing was changed.";
    case FlashOutcome::Cancelled: return "Stopped: cancelled before the erase. The iHeater keeps its firmware.";
    case FlashOutcome::HeatRequested: return "Stopped: heat requested.";
    case FlashOutcome::CoolTimeout:
      return "Stopped: the heater did not cool down in time. The iHeater is still running its firmware.";
    case FlashOutcome::BadReading:
      return "Stopped: the heater temperature could not be read. The iHeater is still running its firmware.";
    case FlashOutcome::NoReplyCooling:
      return "Stopped: the iHeater stopped answering while cooling down. It is still running its firmware.";
    case FlashOutcome::EntryRefused:
      return "The iHeater refused to enter its bootloader and is still running its firmware.";
    case FlashOutcome::EntryNotConfirmed:
      return "The iHeater didn't show its bootloader. Nothing was erased; it keeps its firmware.";
    case FlashOutcome::EraseRefused:
      return "The iHeater's bootloader couldn't erase the old firmware. Retry the update.";
    case FlashOutcome::WriteRefused:
      return "The iHeater's bootloader refused part of the new firmware. Retry the update.";
    case FlashOutcome::BootRefused:
      return "The iHeater's bootloader found the written firmware invalid. Retry the update.";
    case FlashOutcome::NoAppAfterUpdate:
      return "The iHeater didn't answer as its firmware after the update. Check it, then retry the update.";
  }
  return "";
}

}  // namespace iheaterlink::policy
