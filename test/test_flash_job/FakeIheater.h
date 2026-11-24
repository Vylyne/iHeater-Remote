#pragma once

// Fake iHeater for the flash job tests: the app (Modbus registers, the 0xB007
// gate, input 0x16) plus the link bootloader (erase, chunk writes, boot), on
// one fake link. It is the test oracle: it checks every frame against the
// flasher contract and sets `violation` on any breach, and it answers with
// real reply bytes that the job's own parseReply judges. Header-only.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "FirmwareImage.h"
#include "FlashJob.h"
#include "UartFrames.h"
#include "UartRegisters.h"

extern "C" {
#include "modbus_rtu.h"
}

namespace fakeiheater {

using namespace iheaterlink::policy;

enum class State : uint8_t { App, Bootloader };

// Frame kinds, for fault injection and log assertions.
enum class Frame : uint8_t {
  Other,
  ReadState,    // FC04 0x00, one register
  ReadBlock,    // FC04 0x00, the 22-register input block
  ReadOther,    // any other FC04 read (0x16, 0x04, 0x05)
  WriteTarget,  // FC06 0x0100
  Entry,        // FC06 0x0102
  Erase,        // FC06 0x0300 <- 1
  Boot,         // FC06 0x0300 <- 2
  Chunk         // FC10 0x0310
};

enum class Fault : uint8_t {
  DropRequest,  // not acted on, no reply
  DropReply,    // acted on, no reply
  DamageReply   // acted on, the reply's CRC is wrong
};

class FakeIheater : public FlashLink {
 public:
  static constexpr size_t kAppBytes = kMaxImageBytes;  // pages 2-29

  struct Logged {
    std::vector<uint8_t> bytes;
    uint32_t timeoutMs;
    Frame kind;
    State stateAtSend;
    uint32_t clockAtSend;
    int chunk;  // the chunk index for Chunk frames, else -1
  };

  // ---- Knobs ----
  State state = State::App;
  uint16_t appProto = 1;               // the app's input 0x00
  uint16_t bootloaderVersion = 0x0100;  // input 0x16; 0 = old firmware
  bool refuse16 = false;               // old firmware: 0x16 answers exception 0x02
  bool refuseEntry = false;            // 0xB007 always gets exception 0x04
  uint16_t why = 2, appCheck = 0, lastError = 0;
  uint16_t eraseRefuseLastError = 0;   // non-zero: erase gets exception 4 with this last_error
  int chunkExceptionAt = -1;           // that chunk gets exception chunkExceptionCode
  uint8_t chunkExceptionCode = 2;
  uint16_t chunkExceptionLastError = 4;  // last_error set with an exception 0x04
  std::vector<int> notBlankOnce;       // each entry refuses one write of that chunk (last_error 3)
  std::vector<bool> bootOk;            // scripted boot results, used in order; then true
  uint16_t refusedAppCheck = 4;        // app_check after a scripted boot refusal
  bool appSilentAfterBoot = false;     // the new app never answers
  int resetAfterBlockReads = 0;        // non-zero: the app resets into the bootloader after this many
  std::vector<int16_t> heaterSeq;
  int16_t heaterT10 = 250, airT10 = 200, airTargetT10 = 0;
  bool heat = false, cancel = false, cancelInBootloader = false;
  uint32_t heatAtMs = 0, cancelAtMs = 0;  // clock time from which the knob turns true; 0 = never
  size_t heatAfterReads = 0;              // heat turns true once this many block reads were served

  // ---- State and records ----
  uint32_t clock = 1000;
  std::vector<uint8_t> flash = std::vector<uint8_t>(kAppBytes, 0xFF);
  std::vector<Logged> log;
  std::vector<FlashStatus> published;
  std::vector<uint32_t> publishedAt;
  bool violation = false;
  std::string violationText;
  int targetWrites = 0, blockReads = 0, appCheckReads = 0;
  int16_t lastTargetWritten = -1;
  bool versionReadNonZero = false;  // input 0x16 was served non-zero

  // Pages 2-29 start out holding some older app, never blank, so a chunk sent
  // without an erase first is refused as not blank.
  FakeIheater() {
    for (size_t i = 0; i < flash.size(); i++) flash[i] = (uint8_t)(i * 13 + 1);
  }

  // Fault injection: frames of `kind` (and chunk index `chunk`, or any when
  // -1) numbered first..last (1-based, last 0 = every one from first on).
  void inject(Frame kind, Fault fault, int first = 1, int last = -1, int chunk = -1) {
    rules_.push_back({kind, fault, first, last < 0 ? first : last, chunk, 0});
  }

  // What the bootloader's erase does to pages 2-29.
  void setBlank() { std::fill(flash.begin(), flash.end(), 0xFF); }

  size_t count(Frame kind, int chunk = -1) const {
    size_t n = 0;
    for (const Logged &l : log)
      if (l.kind == kind && (chunk < 0 || l.chunk == chunk)) n++;
    return n;
  }
  std::vector<const Logged *> frames(Frame kind, int chunk = -1) const {
    std::vector<const Logged *> out;
    for (const Logged &l : log)
      if (l.kind == kind && (chunk < 0 || l.chunk == chunk)) out.push_back(&l);
    return out;
  }

  // ---- FlashLink ----
  bool exchange(const uint8_t *frame, size_t len, Reply &reply, uint32_t timeoutMs) override {
    Logged entry{std::vector<uint8_t>(frame, frame + len), timeoutMs, Frame::Other, state, clock, -1};
    if (!checkFrame(frame, len)) {
      log.push_back(entry);
      clock += timeoutMs;
      return false;
    }
    entry.kind = classify(frame, len, entry.chunk);
    log.push_back(entry);

    Fault fault;
    const bool faulted = matchFault(entry.kind, entry.chunk, fault);
    if (faulted && fault == Fault::DropRequest) {
      clock += timeoutMs;
      return false;
    }
    out_.clear();
    handle(frame, len);
    if (out_.empty() || (faulted && fault == Fault::DropReply)) {
      clock += timeoutMs;
      return false;
    }
    if (faulted && fault == Fault::DamageReply) out_.back() ^= 0x5A;
    reply = parseReply(frame, out_.data(), out_.size());
    return true;
  }
  void sleepMs(uint32_t ms) override { clock += ms; }
  uint32_t nowMs() override { return clock; }
  bool heatWanted() override {
    return heat || (heatAtMs != 0 && clock >= heatAtMs) ||
           (heatAfterReads != 0 && (size_t)blockReads >= heatAfterReads);
  }
  bool cancelRequested() override {
    return cancel || (cancelAtMs != 0 && clock >= cancelAtMs) ||
           (cancelInBootloader && state == State::Bootloader);
  }
  void publish(const FlashStatus &s) override {
    published.push_back(s);
    publishedAt.push_back(clock);
  }

 private:
  struct Rule {
    Frame kind;
    Fault fault;
    int first, last, chunk, seen;
  };
  std::vector<Rule> rules_;
  std::vector<uint8_t> out_;
  size_t heaterIdx_ = 0;
  int16_t lastHeaterServed_ = INT16_MIN;
  size_t bootIdx_ = 0;
  uint16_t chunksWritten_ = 0;

  void breach(const char *what) {
    if (!violation) violationText = what;
    violation = true;
  }

  static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

  // Slave 1, a whole frame with a good CRC, FC 03/04/06/10, and FC10 only as
  // the contract's chunk write.
  bool checkFrame(const uint8_t *f, size_t len) {
    if (len < 8) {
      breach("short frame");
      return false;
    }
    if (f[0] != kSlaveAddr) {
      breach("not slave 1");
      return false;
    }
    const uint16_t crc = modbus_crc16(f, len - 2);
    if (f[len - 2] != (uint8_t)(crc & 0xFF) || f[len - 1] != (uint8_t)(crc >> 8)) {
      breach("bad CRC");
      return false;
    }
    switch (f[1]) {
      case kFcReadHolding:
      case kFcReadInput:
      case kFcWriteSingle:
        if (len != 8) {
          breach("wrong length for FC 03/04/06");
          return false;
        }
        return true;
      case kFcWriteMultiple: {
        const uint16_t count = get16(f + 4);
        if (len != 9 + 2 * (size_t)count || f[6] != 2 * count) {
          breach("FC10 length or byte count wrong");
          return false;
        }
        if (get16(f + 2) != kRegChunk || count != 65) {
          breach("FC10 not a 65-register write to 0x0310");
          return false;
        }
        if (get16(f + 7) > kMaxChunkIndex) {
          breach("chunk index above 223");
          return false;
        }
        return true;
      }
      default:
        breach("function code not 03/04/06/10");
        return false;
    }
  }

  static Frame classify(const uint8_t *f, size_t, int &chunk) {
    const uint16_t addr = get16(f + 2), value = get16(f + 4);
    switch (f[1]) {
      case kFcReadInput:
        if (addr == 0 && value == 1) return Frame::ReadState;
        if (addr == 0 && value == kInputCount) return Frame::ReadBlock;
        return Frame::ReadOther;
      case kFcWriteSingle:
        if (addr == kRegTarget) return Frame::WriteTarget;
        if (addr == kRegBootloader) return Frame::Entry;
        if (addr == kRegCommand && value == kCmdErase) return Frame::Erase;
        if (addr == kRegCommand && value == kCmdBoot) return Frame::Boot;
        return Frame::Other;
      case kFcWriteMultiple:
        chunk = get16(f + 7);
        return Frame::Chunk;
      default: return Frame::Other;
    }
  }

  bool matchFault(Frame kind, int chunk, Fault &fault) {
    bool hit = false;
    for (Rule &r : rules_) {
      if (r.kind != kind || (r.chunk >= 0 && r.chunk != chunk)) continue;
      r.seen++;
      if (!hit && r.seen >= r.first && (r.last == 0 || r.seen <= r.last)) {
        hit = true;
        fault = r.fault;
      }
    }
    return hit;
  }

  // ---- Replies ----
  void finish() {
    const uint16_t crc = modbus_crc16(out_.data(), out_.size());
    out_.push_back((uint8_t)(crc & 0xFF));
    out_.push_back((uint8_t)(crc >> 8));
  }
  void exception(const uint8_t *f, uint8_t code) {
    out_ = {kSlaveAddr, (uint8_t)(f[1] | 0x80), code};
    finish();
  }
  void echo(const uint8_t *f) {  // FC06: the whole request; FC10: its first 6 bytes
    if (f[1] == kFcWriteSingle) {
      out_.assign(f, f + 8);
      return;
    }
    out_.assign(f, f + 6);
    finish();
  }
  void registers(const uint8_t *f, const uint16_t *regs, uint16_t count) {
    out_ = {kSlaveAddr, f[1], (uint8_t)(2 * count)};
    for (uint16_t i = 0; i < count; i++) {
      out_.push_back((uint8_t)(regs[i] >> 8));
      out_.push_back((uint8_t)(regs[i] & 0xFF));
    }
    finish();
  }

  int16_t currentHeaterT10() const { return lastHeaterServed_ == INT16_MIN ? heaterT10 : lastHeaterServed_; }

  void handle(const uint8_t *f, size_t len) {
    if (state == State::App) {
      if (appSilent_) return;
      handleApp(f);
    } else {
      handleBootloader(f, len);
    }
  }

  bool appSilent_ = false;

  void handleApp(const uint8_t *f) {
    const uint16_t addr = get16(f + 2), value = get16(f + 4);
    if (f[1] == kFcReadInput) {
      if (addr == kRegBootloaderVersion && value == 1) {
        if (refuse16) return exception(f, 0x02);
        if (bootloaderVersion != 0) versionReadNonZero = true;
        return registers(f, &bootloaderVersion, 1);
      }
      if ((uint32_t)addr + value > kInputCount) return exception(f, 0x02);
      uint16_t regs[kInputCount] = {};
      regs[0] = appProto;
      if (addr == 0 && value == kInputCount) {
        blockReads++;
        int16_t h = heaterT10;
        if (!heaterSeq.empty()) {
          h = heaterSeq[heaterIdx_ < heaterSeq.size() ? heaterIdx_ : heaterSeq.size() - 1];
          heaterIdx_++;
        }
        lastHeaterServed_ = h;
        regs[0x09] = (uint16_t)airT10;
        regs[0x0A] = (uint16_t)h;
        regs[0x0C] = (uint16_t)airTargetT10;
        if (resetAfterBlockReads != 0 && blockReads == resetAfterBlockReads) {
          registers(f, regs + addr, value);
          state = State::Bootloader;
          why = 1;
          return;
        }
      }
      return registers(f, regs + addr, value);
    }
    if (f[1] != kFcWriteSingle) return exception(f, 0x01);
    if (addr == kRegTarget) {
      targetWrites++;
      lastTargetWritten = (int16_t)value;
      airTargetT10 = (int16_t)value;
      return echo(f);
    }
    if (addr == kRegBootloader && value == kBootloaderMagic) {
      if (!versionReadNonZero) breach("0xB007 sent before input 0x16 read non-zero");
      if (bootloaderVersion == 0) breach("0xB007 sent to firmware without the link bootloader");
      // The iHeater's own gate: air target 0 and heater at or below 50.0 C.
      if (refuseEntry || airTargetT10 != 0 || currentHeaterT10() > kSafeHeaterT10) return exception(f, 0x04);
      echo(f);  // the reply goes out, then the board resets into the bootloader
      state = State::Bootloader;
      why = 1;
      lastError = 0;
      return;
    }
    exception(f, 0x02);
  }

  void handleBootloader(const uint8_t *f, size_t len) {
    const uint16_t addr = get16(f + 2), value = get16(f + 4);
    if (f[1] == kFcReadInput) {
      if (value == 0 || (uint32_t)addr + value > 0x20) return exception(f, 0x02);
      uint16_t regs[0x20] = {};
      regs[0] = kBootloaderProtocol;
      regs[1] = 0x0100;
      regs[2] = why == 2 ? 2 : 1;
      regs[kBlWhy] = why;
      regs[kBlAppCheck] = appCheck;
      regs[kBlLastError] = lastError;
      regs[kBlTimeoutS] = why == 1 ? 30 : 0;
      regs[7] = chunksWritten_;
      if (addr <= kBlAppCheck && kBlAppCheck < addr + value) appCheckReads++;
      return registers(f, regs + addr, value);
    }
    if (f[1] == kFcWriteSingle) {
      if (addr != kRegCommand) return exception(f, 0x02);
      if (value == kCmdErase) {
        if (eraseRefuseLastError != 0) {
          lastError = eraseRefuseLastError;
          return exception(f, 0x04);
        }
        setBlank();
        chunksWritten_ = 0;
        lastError = 0;
        return echo(f);
      }
      if (value == kCmdBoot) {
        const bool scripted = bootIdx_ < bootOk.size() ? bootOk[bootIdx_] : true;
        bootIdx_++;
        appCheck = scripted ? checkApp() : refusedAppCheck;
        if (appCheck != 0) {
          lastError = 5;
          return exception(f, 0x04);
        }
        echo(f);  // reply, then reset into the app
        state = State::App;
        appSilent_ = appSilentAfterBoot;
        return;
      }
      return exception(f, 0x03);
    }
    if (f[1] == kFcWriteMultiple) {
      const int index = get16(f + 7);
      if (index == chunkExceptionAt) {
        if (chunkExceptionCode == kExcDeviceFailure) lastError = chunkExceptionLastError;
        return exception(f, chunkExceptionCode);
      }
      for (size_t i = 0; i < notBlankOnce.size(); i++) {
        if (notBlankOnce[i] == index) {
          notBlankOnce.erase(notBlankOnce.begin() + (long)i);
          lastError = kLbErrNotBlank;
          return exception(f, 0x04);
        }
      }
      const uint8_t *data = f + 9;
      uint8_t *at = &flash[(size_t)index * kChunkBytes];
      (void)len;
      for (size_t i = 0; i < kChunkBytes; i += 2) {
        const bool same = at[i] == data[i] && at[i + 1] == data[i + 1];
        const bool blank = at[i] == 0xFF && at[i + 1] == 0xFF;
        if (!same && !blank) {
          lastError = kLbErrNotBlank;
          return exception(f, 0x04);
        }
      }
      memcpy(at, data, kChunkBytes);
      chunksWritten_++;
      lastError = 0;
      return echo(f);
    }
    exception(f, 0x01);
  }

  // The bootloader's app check: 0 valid, else the contract's app_check code.
  uint16_t checkApp() const {
    const uint8_t *m = flash.data() + kMarkerOffset;
    const uint32_t size = (uint32_t)m[0x10] | ((uint32_t)m[0x11] << 8) | ((uint32_t)m[0x12] << 16) |
                          ((uint32_t)m[0x13] << 24);
    if (size > kAppBytes) return 3;
    const ImageCheck r = checkImage(flash.data(), size).result;
    if (r == ImageCheck::Ok) return 0;
    if (r == ImageCheck::BadStackPointer || r == ImageCheck::BadResetVector) return 1;
    if (r == ImageCheck::BadChecksum) return 4;
    if (r == ImageCheck::LengthMismatch || r == ImageCheck::TooSmall) return 3;
    return 2;
  }
};

}  // namespace fakeiheater
