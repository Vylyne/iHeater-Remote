#pragma once

// Byte-accurate fake iHeater for the flash job tests. It is the test oracle:
// it follows AN3155 and NACKs malformed frames instead of accepting whatever
// the sequencer sends. Header-only test utility.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <deque>
#include <vector>

#include "FlashJob.h"
#include "RomProtocol.h"

namespace fakeiheater {

using namespace iheaterlink::policy;

enum class Mode : uint8_t { App, Rom, Silent };

class FakeIheater : public FlashLink {
 public:
  struct Write {
    uint32_t addr;
    size_t len;
  };

  // Knobs.
  bool extended = true;
  uint16_t pid = 0x445;
  bool alreadySynced = false;  // the first 0x7F gets a NACK
  bool romSilent = false;
  bool noiseBeforeSyncAck = false;  // one 0x00 byte answers the first 0x7F
  uint32_t nackWriteAt = 0;
  uint32_t corruptReadAt = 0;
  Mode modeAfterGo = Mode::App;
  std::vector<int16_t> heaterSeq;
  int16_t heaterT10 = 250, airT10 = 200, airTargetT10 = 0;
  bool heat = false, cancel = false;
  uint32_t heatAtMs = 0, cancelAtMs = 0;  // clock time from which the knob turns true; 0 = never
  size_t heatAfterReads = 0;              // heat turns true once this many heater reads were served
  int appSilentAfter = -1;                // input reads after the Nth get no reply; -1 = never
  uint8_t b007Exception = 0;              // answer 0xB007 with this exception and stay in App
  bool dropB007Reply = false;             // enter the ROM bootloader but send no reply
  bool b007StaysApp = false;              // reply Ok but stay in App

  // State and records.
  Mode mode = Mode::Rom;
  LinkParity parity = LinkParity::None;
  uint32_t clock = 1000;
  std::vector<uint8_t> flash = std::vector<uint8_t>(32768, 0xFF);
  std::vector<int> erasedPages;
  std::vector<Write> writes;  // accepted writes, in order
  std::vector<uint32_t> syncTimes;
  std::vector<FlashStatus> published;
  bool goSeen = false;
  uint32_t goAddress = 0;
  bool massEraseAttempted = false;
  bool lastEraseExtended = false;
  size_t sentBytes = 0;
  int modbusCalls = 0, modbusBeforeRestarting = 0;
  size_t heaterIdx = 0;
  int inputReads = 0, targetWrites = 0, bootloaderWrites = 0;
  int16_t lastTargetWritten = -1;

  // FlashLink
  void setParity(LinkParity p) override {
    parity = p;
    out_.clear();
    rx_.clear();
  }
  void send(const uint8_t *data, size_t len) override {
    sentBytes += len;
    for (size_t i = 0; i < len; i++) onByte(data[i]);
  }
  size_t receive(uint8_t *buf, size_t len, uint32_t timeoutMs) override {
    size_t n = 0;
    while (n < len && !out_.empty()) {
      buf[n++] = out_.front();
      out_.pop_front();
    }
    if (n < len) clock += timeoutMs;
    return n;
  }
  void flushInput() override { out_.clear(); }
  bool modbus(const PollRequest &req, Reply &reply) override {
    modbusCalls++;
    if (published.empty() || published.back().phase != FlashPhase::Restarting) modbusBeforeRestarting++;
    if (parity != LinkParity::None || mode != Mode::App) return false;
    if (req.kind == PollKind::WriteTarget) {
      targetWrites++;
      lastTargetWritten = (int16_t)req.value;
      airTargetT10 = (int16_t)req.value;
      reply = Reply{};
      reply.kind = ReplyKind::Ok;
      return true;
    }
    if (req.kind == PollKind::WriteBootloader) {
      bootloaderWrites++;
      reply = Reply{};
      if (b007Exception != 0) {
        reply.kind = ReplyKind::Exception;
        reply.exception = b007Exception;
        return true;
      }
      // The iHeater's own rule: air target 0 and heater at or below 50.0 C.
      if (airTargetT10 != 0 || currentHeaterT10() > 500) {
        reply.kind = ReplyKind::Exception;
        reply.exception = 2;
        return true;
      }
      if (!b007StaysApp) mode = Mode::Rom;
      if (dropB007Reply) return false;
      reply.kind = ReplyKind::Ok;
      return true;
    }
    inputReads++;
    if (appSilentAfter >= 0 && inputReads > appSilentAfter) return false;
    int16_t h = heaterT10;
    if (!heaterSeq.empty()) {
      h = heaterSeq[heaterIdx < heaterSeq.size() ? heaterIdx : heaterSeq.size() - 1];
      heaterIdx++;
    }
    lastHeaterServed_ = h;
    reply = Reply{};
    reply.kind = ReplyKind::Ok;
    reply.count = kInputCount;
    reply.regs[0] = 1;
    reply.regs[0x09] = (uint16_t)airT10;
    reply.regs[0x0A] = (uint16_t)h;
    reply.regs[0x0C] = (uint16_t)airTargetT10;
    return true;
  }
  void sleepMs(uint32_t ms) override { clock += ms; }
  uint32_t nowMs() override { return clock; }
  bool heatWanted() override {
    return heat || (heatAtMs != 0 && clock >= heatAtMs) || (heatAfterReads != 0 && heaterIdx >= heatAfterReads);
  }
  bool cancelRequested() override { return cancel || (cancelAtMs != 0 && clock >= cancelAtMs); }
  void publish(const FlashStatus &s) override { published.push_back(s); }

 private:
  enum class State : uint8_t { Unsynced, Idle, Addr, WriteBody, ReadCount, EraseExt, EraseLegacy };
  enum class Op : uint8_t { Write, Read, Go };

  static constexpr uint32_t kBase = 0x08000000;
  static constexpr size_t kSize = 32768;

  bool started_ = false;
  int16_t lastHeaterServed_ = INT16_MIN;

  // The heater reading the iHeater itself would hold now.
  int16_t currentHeaterT10() const { return lastHeaterServed_ == INT16_MIN ? heaterT10 : lastHeaterServed_; }
  State state_ = State::Unsynced;
  Op op_ = Op::Write;
  uint32_t addr_ = 0;
  bool noiseDone_ = false;
  bool erased_[32] = {};
  std::vector<uint8_t> rx_;
  std::deque<uint8_t> out_;

  void reply(uint8_t b) { out_.push_back(b); }
  void ack() { reply(kRomAck); }
  void nack() {
    reply(kRomNack);
    state_ = State::Idle;
    rx_.clear();
  }

  // Total frame length the current state is waiting for, given rx_ (non-empty).
  size_t needed() const {
    switch (state_) {
      case State::Idle:
      case State::ReadCount: return 2;
      case State::Addr: return 5;
      case State::WriteBody: return (size_t)rx_[0] + 3;
      case State::EraseExt: {
        if (rx_.size() < 2) return 2;
        const uint32_t c = ((uint32_t)rx_[0] << 8) | rx_[1];
        if (c >= 0xFFF0) return 3;
        return 2 + 2 * ((size_t)c + 1) + 1;
      }
      case State::EraseLegacy: return rx_[0] == 0xFF ? 2 : (size_t)rx_[0] + 3;
      default: return 1;
    }
  }

  void onByte(uint8_t b) {
    if (!started_) {
      started_ = true;
      state_ = alreadySynced ? State::Idle : State::Unsynced;
    }
    if (mode != Mode::Rom || parity != LinkParity::Even || romSilent) return;
    if (b == kRomSync) syncTimes.push_back(clock);
    if (state_ == State::Unsynced) {
      if (b != kRomSync) return;
      if (noiseBeforeSyncAck && !noiseDone_) {
        noiseDone_ = true;
        reply(0x00);
        return;
      }
      state_ = State::Idle;
      ack();
      return;
    }
    if (state_ == State::Idle && rx_.empty() && b == kRomSync) {
      reply(kRomNack);  // a lone 0x7F once synced
      return;
    }
    rx_.push_back(b);
    if (rx_.size() < needed()) return;
    handle();
  }

  bool addrOk(uint32_t a, size_t n) const { return a >= kBase && (a - kBase) + n <= kSize; }

  bool pagesErased(uint32_t a, size_t n) const {
    for (uint32_t p = (a - kBase) / 1024; p <= (a - kBase + n - 1) / 1024; p++)
      if (!erased_[p]) return false;
    return true;
  }

  void erasePage(int p) {
    memset(&flash[(size_t)p * 1024], 0xFF, 1024);
    erased_[p] = true;
    erasedPages.push_back(p);
  }

  static uint8_t xorOf(const uint8_t *d, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= d[i];
    return x;
  }

  void handle() {
    std::vector<uint8_t> f;
    f.swap(rx_);
    switch (state_) {
      case State::Idle: {
        if ((uint8_t)(f[0] ^ f[1]) != 0xFF) return nack();
        const uint8_t cmd = f[0];
        if (cmd == kRomGet) {
          ack();
          const uint8_t list[] = {kRomGet, 0x01, kRomGetId, kRomRead, kRomGo,
                                  extended ? kRomExtErase : kRomErase, 0x63, 0x73, 0x82, 0x92};
          reply(sizeof list);  // N: the version byte plus N commands follow
          reply(0x31);
          for (uint8_t c : list) reply(c);
          ack();
        } else if (cmd == kRomGetId) {
          ack();
          reply(1);
          reply(pid >> 8);
          reply(pid & 0xFF);
          ack();
        } else if (cmd == kRomWrite || cmd == kRomRead || cmd == kRomGo) {
          ack();
          op_ = cmd == kRomWrite ? Op::Write : cmd == kRomRead ? Op::Read : Op::Go;
          state_ = State::Addr;
        } else if (cmd == (extended ? kRomExtErase : kRomErase)) {
          ack();
          lastEraseExtended = extended;
          state_ = extended ? State::EraseExt : State::EraseLegacy;
        } else {
          nack();
        }
        return;
      }
      case State::Addr: {
        if (xorOf(f.data(), 4) != f[4]) return nack();
        addr_ = ((uint32_t)f[0] << 24) | ((uint32_t)f[1] << 16) | ((uint32_t)f[2] << 8) | f[3];
        if (!addrOk(addr_, 1) || (op_ == Op::Write && (addr_ & 3))) return nack();
        ack();
        if (op_ == Op::Write) {
          state_ = State::WriteBody;
        } else if (op_ == Op::Read) {
          state_ = State::ReadCount;
        } else {
          goSeen = true;
          goAddress = addr_;
          mode = modeAfterGo;
          state_ = State::Unsynced;
        }
        return;
      }
      case State::WriteBody: {
        const size_t n = (size_t)f[0] + 1;
        if (xorOf(f.data(), n + 1) != f[n + 1] || n % 4 || !addrOk(addr_, n) || !pagesErased(addr_, n))
          return nack();
        if (addr_ == nackWriteAt) return nack();
        memcpy(&flash[addr_ - kBase], &f[1], n);
        writes.push_back({addr_, n});
        ack();
        state_ = State::Idle;
        return;
      }
      case State::ReadCount: {
        if ((uint8_t)(f[0] ^ f[1]) != 0xFF) return nack();
        const size_t n = (size_t)f[0] + 1;
        if (!addrOk(addr_, n)) return nack();
        ack();
        for (size_t i = 0; i < n; i++) {
          uint8_t v = flash[addr_ - kBase + i];
          if (corruptReadAt && addr_ + i == corruptReadAt) v ^= 0x01;
          reply(v);
        }
        state_ = State::Idle;
        return;
      }
      case State::EraseExt: {
        const uint32_t c = ((uint32_t)f[0] << 8) | f[1];
        if (c >= 0xFFF0) {
          massEraseAttempted = true;
          return nack();
        }
        if (xorOf(f.data(), f.size() - 1) != f.back()) return nack();
        for (size_t i = 0; i <= c; i++)
          if ((((int)f[2 + 2 * i] << 8) | f[3 + 2 * i]) > 29) return nack();
        for (size_t i = 0; i <= c; i++) erasePage(((int)f[2 + 2 * i] << 8) | f[3 + 2 * i]);
        ack();
        state_ = State::Idle;
        return;
      }
      case State::EraseLegacy: {
        if (f[0] == 0xFF) {
          massEraseAttempted = true;
          return nack();
        }
        if (xorOf(f.data(), f.size() - 1) != f.back()) return nack();
        const size_t n = (size_t)f[0] + 1;
        for (size_t i = 0; i < n; i++)
          if (f[1 + i] > 29) return nack();
        for (size_t i = 0; i < n; i++) erasePage(f[1 + i]);
        ack();
        state_ = State::Idle;
        return;
      }
      default: return;
    }
  }
};

}  // namespace fakeiheater
