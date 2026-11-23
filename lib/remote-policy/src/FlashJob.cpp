#include "FlashJob.h"

#include <stdio.h>
#include <string.h>

#include "FirmwareImage.h"
#include "RomProtocol.h"

namespace iheaterlink::policy {

namespace {

constexpr uint32_t kSyncReplyMs = 100;

enum class Wait : uint8_t { Ack, Nack, Timeout, Bad };

const char *waitText(Wait w) {
  switch (w) {
    case Wait::Nack: return "NACK";
    case Wait::Timeout: return "timeout";
    default: return "bad reply";
  }
}

class Job {
 public:
  explicit Job(FlashLink &link) : link_(link) {}

  FlashStatus st;

  void enter(FlashPhase phase) {
    st.phase = phase;
    link_.publish(st);
  }

  FlashStatus finish(FlashOutcome outcome) {
    link_.setParity(LinkParity::None);
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

  FlashStatus failWith(FlashOutcome outcome, const char *text) {
    snprintf(st.detail, sizeof st.detail, "%s", text);
    return fail(outcome);
  }

  // Step label for failure details, e.g. "write at 0x08000100".
  void step(const char *name) { snprintf(label_, sizeof label_, "%s", name); }
  void stepAt(const char *name, uint32_t addr) {
    snprintf(label_, sizeof label_, "%s at 0x%08X", name, (unsigned)addr);
  }

  bool ack(uint32_t timeoutMs) {
    uint8_t b = 0;
    Wait w = Wait::Ack;
    if (link_.receive(&b, 1, timeoutMs) != 1) {
      w = Wait::Timeout;
    } else if (b == kRomNack) {
      w = Wait::Nack;
    } else if (b != kRomAck) {
      w = Wait::Bad;
    }
    if (w == Wait::Ack) return true;
    snprintf(st.detail, sizeof st.detail, "%s: %s", label_, waitText(w));
    return false;
  }

  bool receiveAll(uint8_t *buf, size_t len, uint32_t timeoutMs) {
    if (link_.receive(buf, len, timeoutMs) == len) return true;
    snprintf(st.detail, sizeof st.detail, "%s: timeout", label_);
    return false;
  }

  bool command(uint8_t cmd) {
    uint8_t f[2];
    buildRomCommand(cmd, f);
    link_.send(f, sizeof f);
    return ack(kRomReplyMs);
  }

  bool address(uint32_t addr) {
    uint8_t f[5];
    buildRomAddress(addr, f);
    link_.send(f, sizeof f);
    return ack(kRomReplyMs);
  }

  bool sync() {
    link_.setParity(LinkParity::Even);
    link_.sleepMs(kSyncSettleMs);
    for (uint8_t i = 0; i < kSyncTries; i++) {
      link_.flushInput();
      const uint8_t sync = kRomSync;
      link_.send(&sync, 1);
      uint8_t b = 0;
      if (link_.receive(&b, 1, kSyncReplyMs) == 1 && (b == kRomAck || b == kRomNack)) {
        link_.sleepMs(50);
        link_.flushInput();
        return true;
      }
      link_.sleepMs(kSyncGapMs);
    }
    return false;
  }

  // Get: the version and command list tell which erase command exists.
  bool get(bool &extended, bool &legacy) {
    step("get");
    if (!command(kRomGet)) return false;
    uint8_t n = 0;
    if (!receiveAll(&n, 1, kRomReplyMs)) return false;
    uint8_t body[256];
    if (!receiveAll(body, (size_t)n + 1, kRomReplyMs)) return false;
    if (!ack(kRomReplyMs)) return false;
    extended = legacy = false;
    for (size_t i = 1; i <= n; i++) {  // body[0] is the bootloader version
      if (body[i] == kRomExtErase) extended = true;
      if (body[i] == kRomErase) legacy = true;
    }
    return true;
  }

  bool getId() {
    step("get id");
    if (!command(kRomGetId)) return false;
    uint8_t n = 0;
    if (!receiveAll(&n, 1, kRomReplyMs)) return false;
    uint8_t body[256];
    if (!receiveAll(body, (size_t)n + 1, kRomReplyMs)) return false;
    if (!ack(kRomReplyMs)) return false;
    if (n < 1) {
      snprintf(st.detail, sizeof st.detail, "get id: short reply");
      return false;
    }
    st.pid = (uint16_t)((body[0] << 8) | body[1]);
    st.unexpectedPid = st.pid != kExpectedPid;
    return true;
  }

  bool erase(uint16_t pages, bool extended) {
    step("erase");
    uint8_t body[64];
    const size_t n = buildEraseBody(pages, extended, body, sizeof body);
    if (n == 0) {
      snprintf(st.detail, sizeof st.detail, "erase: bad page count");
      return false;
    }
    if (!command(extended ? kRomExtErase : kRomErase)) return false;
    link_.send(body, n);
    return ack(kRomEraseMs);
  }

  bool writeChunk(uint32_t addr, const uint8_t *data, size_t len) {
    stepAt("write", addr);
    uint8_t body[kRomChunkBytes + 2];
    const size_t n = buildWriteBody(data, len, body, sizeof body);
    if (n == 0) {
      snprintf(st.detail, sizeof st.detail, "%s: bad chunk", label_);
      return false;
    }
    if (!command(kRomWrite) || !address(addr)) return false;
    link_.send(body, n);
    return ack(kRomReplyMs);
  }

  bool readChunk(uint32_t addr, uint8_t *out, size_t len) {
    stepAt("verify", addr);
    uint8_t count[2];
    buildReadCount(len, count);
    if (!command(kRomRead) || !address(addr)) return false;
    link_.send(count, sizeof count);
    if (!ack(kRomReplyMs)) return false;
    return receiveAll(out, len, kRomReplyMs);
  }

  bool go(uint32_t addr) {
    step("go");
    return command(kRomGo) && address(addr);
  }

  bool stillInApplication() {
    link_.setParity(LinkParity::None);
    link_.sleepMs(50);
    Reply reply;
    return link_.modbus({PollKind::ReadInputs, kInputBase, kInputCount}, reply) && reply.kind == ReplyKind::Ok;
  }

  // Waits for the heater to cool, with the air target at 0. Returns true when
  // it is safe to enter the bootloader; otherwise the job has ended in `done`.
  bool cool(FlashStatus &done) {
    enter(FlashPhase::Cooling);
    const uint32_t start = link_.nowMs();
    uint8_t misses = 0;
    for (;;) {
      if (link_.cancelRequested()) return stop(FlashOutcome::Cancelled, done);
      if (link_.heatWanted()) return stop(FlashOutcome::HeatRequested, done);
      Reply reply;
      IheaterSnapshot snap;
      const bool ok = link_.modbus({PollKind::ReadInputs, kInputBase, kInputCount}, reply) &&
                      reply.kind == ReplyKind::Ok && decodeInputs(reply.regs, reply.count, snap);
      if (!ok) {
        if (++misses >= kCoolMissLimit) return stop(FlashOutcome::NoReplyCooling, done);
      } else {
        misses = 0;
        st.heaterT10 = snap.heaterT10;
        link_.publish(st);
        if (snap.heaterT10 == kNoReading || snap.airT10 == kNoReading)
          return stop(FlashOutcome::BadReading, done);
        if (snap.airTargetT10 != 0) {
          Reply ignored;  // a lost write shows up as a non-zero target on the next read
          link_.modbus({PollKind::WriteTarget, kRegTarget, 0}, ignored);
        } else if (snap.heaterT10 <= kSafeHeaterT10) {
          return true;
        }
      }
      if ((uint32_t)(link_.nowMs() - start) >= kCoolTimeoutMs) return stop(FlashOutcome::CoolTimeout, done);
      link_.sleepMs(kCoolPollMs);
    }
  }

  // Writes 0xB007 once. True: carry on to the ROM phases (entry may be
  // unconfirmed); false: the job has ended in `done`.
  bool enterBootloader(FlashStatus &done) {
    enter(FlashPhase::Entering);
    if (link_.cancelRequested()) return stop(FlashOutcome::Cancelled, done);
    if (link_.heatWanted()) return stop(FlashOutcome::HeatRequested, done);
    Reply reply;
    if (link_.modbus({PollKind::WriteBootloader, kRegBootloader, kBootloaderMagic}, reply)) {
      if (reply.kind == ReplyKind::Exception) {
        snprintf(st.detail, sizeof st.detail, "exception 0x%02X", (unsigned)reply.exception);
        return stop(FlashOutcome::EntryRefused, done);
      }
      if (reply.kind == ReplyKind::Ok) return true;
    }
    st.entryUnconfirmed = true;
    return true;
  }

  bool restartAnswers() {
    const PollRequest req{PollKind::ReadInputs, kInputBase, kInputCount};
    const uint32_t start = link_.nowMs();
    while ((uint32_t)(link_.nowMs() - start) < kRestartWaitMs) {
      Reply reply;
      if (link_.modbus(req, reply) && reply.kind == ReplyKind::Ok) return true;
      link_.sleepMs(kPollPeriodMs);
    }
    return false;
  }

 private:
  bool stop(FlashOutcome outcome, FlashStatus &done) {
    done = fail(outcome);
    return false;
  }

  FlashLink &link_;
  char label_[32] = "";
};

// Image byte at index i of the word-padded image.
void fillChunk(const uint8_t *image, size_t len, size_t at, size_t n, uint8_t *out) {
  for (size_t i = 0; i < n; i++) out[i] = at + i < len ? image[at + i] : 0xFF;
}

// The ROM phases: sync, erase, write and verify, go, restart.
FlashStatus romPhases(Job &job, FlashLink &link, const uint8_t *image, size_t len) {
  const size_t padded = (len + 3) & ~(size_t)3;
  const uint16_t pages = (uint16_t)((len + kFlashPageBytes - 1) / kFlashPageBytes);

  job.enter(FlashPhase::Syncing);
  if (!job.sync()) {
    // After an unconfirmed 0xB007, a Modbus answer means the iHeater never left its application.
    if (job.st.entryUnconfirmed && job.stillInApplication())
      return job.failWith(FlashOutcome::EntryNotConfirmed, "still answering Modbus after 0xB007");
    return job.failWith(FlashOutcome::SyncFailed, "sync: no reply after 20 tries");
  }

  bool extended = false, legacy = false;
  if (!job.get(extended, legacy)) return job.fail(FlashOutcome::RomError);
  if (!extended && !legacy)
    return job.failWith(FlashOutcome::RomError, "get: neither Erase nor Extended Erase offered");
  job.st.extendedErase = extended;
  if (!job.getId()) return job.fail(FlashOutcome::RomError);

  job.enter(FlashPhase::Erasing);
  if (!job.erase(pages, extended)) return job.fail(FlashOutcome::RomError);

  // Everything after the first 256 bytes, then bytes 4..255, then word 0 last,
  // so an interrupted job never leaves a valid-looking vector table.
  job.enter(FlashPhase::Writing);
  size_t verified = 0;
  uint8_t chunk[kRomChunkBytes], back[kRomChunkBytes];
  struct Span {
    size_t at, n;
  };
  const size_t first = padded < kRomChunkBytes ? padded : kRomChunkBytes;
  size_t next = kRomChunkBytes;
  bool bodyDone = false;
  for (;;) {
    Span s;
    if (next < padded) {
      s = {next, padded - next < kRomChunkBytes ? padded - next : kRomChunkBytes};
      next += kRomChunkBytes;
    } else if (!bodyDone) {
      bodyDone = true;
      s = {4, first - 4};
    } else {
      s = {0, 4};
    }
    const uint32_t addr = kFlashBase + (uint32_t)s.at;
    fillChunk(image, len, s.at, s.n, chunk);
    if (!job.writeChunk(addr, chunk, s.n)) return job.fail(FlashOutcome::RomError);
    if (!job.readChunk(addr, back, s.n)) return job.fail(FlashOutcome::RomError);
    if (memcmp(chunk, back, s.n) != 0) {
      snprintf(job.st.detail, sizeof job.st.detail, "verify at 0x%08X: data mismatch", (unsigned)addr);
      return job.fail(FlashOutcome::VerifyFailed);
    }
    verified += s.n;
    job.st.progressPermille = (uint16_t)((verified * 1000u) / padded);
    link.publish(job.st);
    if (s.at == 0) break;
  }

  job.enter(FlashPhase::Starting);
  if (!job.go(kFlashBase)) return job.fail(FlashOutcome::RomError);

  job.enter(FlashPhase::Restarting);
  link.setParity(LinkParity::None);
  if (job.restartAnswers()) return job.finish(FlashOutcome::Updated);
  job.st.failedPhase = FlashPhase::Restarting;
  return job.finish(probeRom(link) ? FlashOutcome::PowerCycleNeeded : FlashOutcome::NoReplyAfterUpdate);
}

}  // namespace

FlashStatus runFlashJob(FlashLink &link, const uint8_t *image, size_t len, FlashStart start) {
  Job job(link);
  link.setParity(LinkParity::None);
  if (image == nullptr || len < kMinImageBytes || len > kMaxImageBytes)
    return job.failWith(FlashOutcome::ImageRejected, "image length out of range");
  if (start == FlashStart::Normal) {
    FlashStatus done;
    if (!job.cool(done) || !job.enterBootloader(done)) return done;
  }
  return romPhases(job, link, image, len);
}

bool probeRom(FlashLink &link) {
  link.setParity(LinkParity::Even);
  link.flushInput();
  const uint8_t sync = kRomSync;
  link.send(&sync, 1);
  uint8_t b = 0;
  const bool answered = link.receive(&b, 1, kRomProbeReplyMs) == 1 && (b == kRomAck || b == kRomNack);
  link.setParity(LinkParity::None);
  return answered;
}

bool romProbeDue(uint32_t nowMs, uint32_t lastOkMs, uint32_t lastProbeMs) {
  if ((uint32_t)(nowMs - lastOkMs) < kRomProbeAfterMs) return false;
  return lastProbeMs == 0 || (uint32_t)(nowMs - lastProbeMs) >= kRomProbePeriodMs;
}

bool heatWanted(bool targetMode, float targetC) { return targetMode && targetC > 0.0f; }

FlashStartDecision decideFlashStart(const FlashStartInputs &in, bool retry) {
  if (!in.uartOutput) return {FlashRefusal::NotUart, FlashStart::Normal};
  if (in.jobRunning) return {FlashRefusal::JobRunning, FlashStart::Normal};
  if (!in.imageOk) return {FlashRefusal::NoImage, FlashStart::Normal};
  if (in.heatWanted) return {FlashRefusal::HeatWanted, FlashStart::Normal};
  if (retry) {
    if (!retryAllowed(in.lastOutcome)) return {FlashRefusal::NothingToRetry, FlashStart::Retry};
    return {FlashRefusal::None, FlashStart::Retry};
  }
  if (in.link == LinkStatus::Up) return {FlashRefusal::None, FlashStart::Normal};
  if (in.romDetected) return {FlashRefusal::None, FlashStart::Recovery};
  return {FlashRefusal::LinkDown, FlashStart::Normal};
}

const char *flashRefusalMessage(FlashRefusal r) {
  switch (r) {
    case FlashRefusal::None: return "";
    case FlashRefusal::NotUart: return "Switch the output to UART first.";
    case FlashRefusal::NoImage: return "Upload a firmware file that passes the check first.";
    case FlashRefusal::HeatWanted: return "Turn the heat off first: Manual Off, or a Klipper target of 0.";
    case FlashRefusal::JobRunning: return "An iHeater update is already running.";
    case FlashRefusal::LinkDown: return "The iHeater isn't answering, and its bootloader wasn't detected.";
    case FlashRefusal::NothingToRetry: return "There's no failed update to retry.";
  }
  return "";
}

bool retryAllowed(FlashOutcome o) {
  return o == FlashOutcome::SyncFailed || o == FlashOutcome::RomError ||
         o == FlashOutcome::VerifyFailed || o == FlashOutcome::NoReplyAfterUpdate;
}

bool applicationIntact(FlashOutcome o) {
  switch (o) {
    case FlashOutcome::Cancelled:
    case FlashOutcome::HeatRequested:
    case FlashOutcome::CoolTimeout:
    case FlashOutcome::BadReading:
    case FlashOutcome::NoReplyCooling:
    case FlashOutcome::EntryRefused:
    case FlashOutcome::EntryNotConfirmed:
    case FlashOutcome::ImageRejected: return true;
    default: return false;
  }
}

const char *flashPhaseName(FlashPhase p) {
  switch (p) {
    case FlashPhase::Idle: return "idle";
    case FlashPhase::Cooling: return "cooling";
    case FlashPhase::Entering: return "entering";
    case FlashPhase::Syncing: return "syncing";
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
    case FlashOutcome::PowerCycleNeeded: return "power-cycle";
    case FlashOutcome::Cancelled: return "cancelled";
    case FlashOutcome::HeatRequested: return "heat-requested";
    case FlashOutcome::CoolTimeout: return "cool-timeout";
    case FlashOutcome::BadReading: return "bad-reading";
    case FlashOutcome::NoReplyCooling: return "no-reply-cooling";
    case FlashOutcome::EntryRefused: return "entry-refused";
    case FlashOutcome::EntryNotConfirmed: return "entry-not-confirmed";
    case FlashOutcome::ImageRejected: return "image-rejected";
    case FlashOutcome::SyncFailed: return "sync-failed";
    case FlashOutcome::RomError: return "rom-error";
    case FlashOutcome::VerifyFailed: return "verify-failed";
    case FlashOutcome::NoReplyAfterUpdate: return "no-reply-after-update";
  }
  return "none";
}

const char *flashOutcomeMessage(FlashOutcome o) {
  switch (o) {
    case FlashOutcome::None: return "";
    case FlashOutcome::Updated: return "The iHeater was updated and is running the new firmware.";
    case FlashOutcome::PowerCycleNeeded:
      return "The iHeater was updated but is still in its bootloader. Power-cycle the iHeater; don't flash it again.";
    case FlashOutcome::Cancelled: return "Stopped: cancelled. The iHeater is still running its firmware.";
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
      return "Entry not confirmed; the iHeater is still running its firmware.";
    case FlashOutcome::ImageRejected: return "The image was rejected. The iHeater is still running its firmware.";
    case FlashOutcome::SyncFailed:
      return "The iHeater's bootloader did not answer. Check the wiring, then retry or use recovery.";
    case FlashOutcome::RomError:
      return "The iHeater's bootloader reported an error. The firmware may be incomplete; retry the update.";
    case FlashOutcome::VerifyFailed:
      return "The written firmware did not match the image. It may be incomplete; retry the update.";
    case FlashOutcome::NoReplyAfterUpdate:
      return "The iHeater did not answer after the update. Check it, then retry the update.";
  }
  return "";
}

}  // namespace iheaterlink::policy
