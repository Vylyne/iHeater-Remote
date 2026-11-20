# iHeater Remote UART client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Remote the Modbus RTU master for the iHeater's UART build, selectable in Setup next to the existing pulse output.

**Architecture:** All decisions (frames, register decode, range, poll state machine, resume record, setup-save rule) are pure C++ in `lib/remote-policy/`, tested on the PC against a copy of the iHeater's own Modbus slave core. A thin FreeRTOS task (`UartLink`) moves bytes on UART1 and feeds the poll policy; the app talks to it through the same output interface as the pulse adapter.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17 (ESP32-C3), ESP-IDF UART driver, FreeRTOS, Unity on `platform = native` with MinGW.

**Spec:** `docs/design/2026-11-20-remote-uart-client-design.md` (approved). Contract: `C:/git/github/iHeater-Standalone-Firmware/docs/design/2026-11-19-uart-link-design.md` and that repo's `docs/en/README.md`, "Working with iHeater Remote (UART build)".

## Global Constraints

- Modbus RTU, slave address 1, 115200 8N1. Function codes used by the Remote: 0x03, 0x04, 0x06. Clear only with FC 06.
- Input block 0x0000–0x0015 (22 registers); settings block 0x0200–0x0209 (10 registers, temperatures in signed tenths, `link_hold_s` in seconds); `air_target_cmd` 0x0100; `clear_fault` 0x0101.
- Temperatures are signed 16-bit tenths of °C; `0x8000` means no reading.
- Poll cycle 200 ms; reply timeout 200 ms; never back off.
- UART1, TX GPIO4 (`IHEATER_UART_TX_PIN`), RX GPIO5 (`IHEATER_UART_RX_PIN`). Pulse stays on GPIO3.
- Nothing is written to the iHeater until the app has set a desired target. Until the settings block has been read, only target 0 is written.
- Never heat above the user's Setup maximum: an empty effective range means target 0.
- `proto_version` other than 1: read and display only, never write.
- Policy files in `lib/remote-policy/` and test files use CRLF; keep each existing file's line endings. Edit CRLF files with a script that writes `\r\n`, never `sed -i`.
- Vendored `lib/modbus-rtu/` files are byte-identical copies of iHeater-Standalone-Firmware `Lib/ModbusRtu/` at commit `d1da01a` (LF endings).
- Canadian English in comments and docs.
- Never flash or upload to a device without asking the user first.
- Commit messages end with a blank line, then `Co-Authored-By: <model that wrote the change> <noreply@anthropic.com>`.
- Gates (after Task 1):
  - `pio run -e esp32c3-super-mini-remote`
  - `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native`
  - `python -m unittest discover -s klipper -p "test_*.py"`
  Redirect long `pio` output to a file and read its tail.

## Review Focus

1. **A reply split across UART reads, or trailing garbage after a good frame** (iHeater boot noise, a half frame at power-up). The parser must return Incomplete for a short prefix and Invalid, not Ok, for extra bytes. Pinned by `test_parse_incomplete_prefix` and `test_parse_trailing_byte_invalid` (Task 2).
2. **The off command while the settings block cannot be read.** Turning heat off must never wait on limits. Pinned by `test_off_written_before_limits` (Task 4).
3. **`millis()` wrap during link-status timing.** `link()` must use wrap-safe differences. Pinned by `test_link_status_wraparound` (Task 4).
4. **A clear whose reply is lost, then the iHeater does not restart.** It must count as an erase failure and not be retried. Pinned by `test_lost_clear_reply_without_restart_blocks` (Task 5).
5. **RTC resume record holding garbage after a power-on reset.** It must never resume. Pinned by `test_resume_rejects_power_on` and `test_resume_rejects_zeroed` (Task 6).

---

### Task 1: PC test environment and vendored Modbus core

**Files:**
- Create: `lib/modbus-rtu/library.json`, `lib/modbus-rtu/src/modbus_rtu.c`, `lib/modbus-rtu/src/modbus_rtu.h`, `lib/modbus-rtu/README.md`
- Modify: `platformio.ini` (add `[env:native]`)
- Modify: `test/test_remote_policy/test_main.cpp` (CRLF)
- Modify: `lib/remote-policy/library.json`
- Modify: `AGENTS.md`

**Interfaces:**
- Produces: a `native` env that runs every `test/test_*` directory on the PC; `modbus_rtu.h` (C, no `extern "C"` guard: C++ includes it inside `extern "C" { }`).

- [ ] **Step 1: Copy the core.** Copy `C:/git/github/iHeater-Standalone-Firmware/Lib/ModbusRtu/modbus_rtu.{c,h}` byte for byte into `lib/modbus-rtu/src/`. `library.json`: `{"name": "modbus-rtu", "version": "1.0.0"}`. `README.md`: two or three lines naming the source repo, path and commit `d1da01a`, and "change it there first, then copy again".
- [ ] **Step 2: Add the native env.**

```ini
; Host-side unit tests: pio test -e native (needs MinGW gcc on PATH). Not a default env.
[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17
```

Add `"dependencies": [{"name": "modbus-rtu"}]` to `lib/remote-policy/library.json` only if Task 2's LDF scan does not find it by include; otherwise leave it.
- [ ] **Step 3: Make `test_remote_policy` host-runnable.** Drop `#include <Arduino.h>`; replace `setup()`/`loop()` with `int main()` that runs the same `RUN_TEST` list between `UNITY_BEGIN()` and `return UNITY_END();`. Add one test:

```cpp
extern "C" {
#include "modbus_rtu.h"
}
void test_vendored_crc_vector() {
  const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
  TEST_ASSERT_EQUAL_HEX16(0xCDC5, modbus_crc16(frame, sizeof frame));  // wire bytes C5 CD
}
```

- [ ] **Step 4: Run.** `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native > $WS/t1.log 2>&1; tail -20 $WS/t1.log`
  Expected: `test_remote_policy` PASSED, 32 test cases, 0 failures.
- [ ] **Step 5: Firmware still builds.** `pio run -e esp32c3-super-mini-remote` → SUCCESS.
- [ ] **Step 6: AGENTS.md.** Replace the build-only `pio test -e esp32c3-super-mini-remote …` gate with the native command (with the `PATH` export), remove "There is no host C++ compiler…" paragraph, replace it with one line: policy tests run on the PC with MinGW from `~/scoop/apps/mingw`. Add `lib/modbus-rtu/` to Layout ("copy of the iHeater's Modbus slave core; do not edit here").
- [ ] **Step 7: Commit** `build: run policy tests on the PC; vendor the iHeater Modbus core`.

---

### Task 2: Modbus master frames

**Files:**
- Create: `lib/remote-policy/src/UartFrames.h`, `lib/remote-policy/src/UartFrames.cpp` (CRLF)
- Create: `test/test_uart_link/test_main.cpp` (CRLF, `int main()`)

**Interfaces:**
- Consumes: `modbus_crc16`, `modbus_handle_frame`, `ModbusHandlers` (tests only for the latter two).
- Produces (namespace `iheaterlink::policy`):

```cpp
constexpr uint8_t kSlaveAddr = 1;
constexpr uint8_t kFcReadHolding = 0x03;
constexpr uint8_t kFcReadInput = 0x04;
constexpr uint8_t kFcWriteSingle = 0x06;
constexpr size_t kRequestLen = 8;
constexpr uint8_t kMaxReadRegs = 32;

// 8 on success; 0 when fc is not 0x03/0x04 or count is 0 or > kMaxReadRegs.
size_t buildReadRequest(uint8_t fc, uint16_t addr, uint16_t count, uint8_t out[kRequestLen]);
size_t buildWriteSingle(uint16_t addr, uint16_t value, uint8_t out[kRequestLen]);  // always 8

enum class ReplyKind : uint8_t { Incomplete, Ok, Exception, Invalid };
struct Reply {
  ReplyKind kind = ReplyKind::Incomplete;
  uint8_t exception = 0;          // set for Exception
  uint8_t count = 0;              // registers in regs for an Ok read; 0 for an Ok write
  uint16_t regs[kMaxReadRegs] = {};
};
// Judges buf (everything received so far) against the request that was sent.
Reply parseReply(const uint8_t req[kRequestLen], const uint8_t *buf, size_t len);
```

Length rule: if `len >= 2` and `buf[1] == (req[1] | 0x80)`, the reply is 5 bytes; else a read reply is `5 + 2*count` bytes and a write reply is 8 bytes (exact echo of the request). Fewer bytes than that → Incomplete. More bytes, wrong address, bad CRC, an unexpected function code, a byte count that does not equal `2*count`, or a write echo that differs → Invalid.

- [ ] **Step 1: Write the failing tests** in `test/test_uart_link/test_main.cpp`. A fake slave for loopback: input register `a` reads `0x100 + a` for `a <= 0x15`, else exception 0x02; holding 0x0100 stores its write; other writes return 0x02.

```cpp
void test_build_read_vector() {  // classic Modbus example
  uint8_t f[kRequestLen];
  TEST_ASSERT_EQUAL(8, buildReadRequest(kFcReadHolding, 0x0000, 10, f));
  const uint8_t want[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, 8);
}
void test_build_read_rejects() {
  uint8_t f[kRequestLen];
  TEST_ASSERT_EQUAL(0, buildReadRequest(0x06, 0, 1, f));
  TEST_ASSERT_EQUAL(0, buildReadRequest(kFcReadInput, 0, 0, f));
  TEST_ASSERT_EQUAL(0, buildReadRequest(kFcReadInput, 0, 33, f));
}
void test_loopback_read_inputs();      // request 0x0000 x22 through modbus_handle_frame → Ok, count 22, regs[i] == 0x100+i
void test_loopback_exception();        // read input 0x0030 x1 → Exception, exception == 0x02
void test_loopback_write_target();     // buildWriteSingle(0x0100, 600) → Ok, count 0, fake holds 600
void test_parse_incomplete_prefix();   // first 4 bytes of the 49-byte inputs reply → Incomplete; first 1 byte → Incomplete
void test_parse_trailing_byte_invalid(); // good reply + 1 extra byte → Invalid
void test_parse_bad_crc_invalid();     // flip one CRC bit → Invalid
void test_parse_wrong_address_invalid(); // reply byte 0 = 0x02 (CRC recomputed) → Invalid
void test_parse_byte_count_mismatch(); // read x22 answered with byte count 42 (CRC recomputed) → Invalid
void test_parse_write_echo_mismatch(); // echo with value 601 for a 600 write (CRC recomputed) → Invalid
```

- [ ] **Step 2: Run** `pio test -e native -f test_uart_link` → FAIL (UartFrames.h not found).
- [ ] **Step 3: Implement** `UartFrames.cpp`, CRC via `modbus_crc16` (include inside `extern "C"`), big-endian register words, CRC low byte first.
- [ ] **Step 4: Run** `pio test -e native` → both suites PASS.
- [ ] **Step 5: Commit** `feat(policy): Modbus master frames for the iHeater link`.

---

### Task 3: Register decode and UART target range

**Files:**
- Create: `lib/remote-policy/src/UartRegisters.h`, `lib/remote-policy/src/UartRegisters.cpp` (CRLF)
- Test: `test/test_uart_link/test_main.cpp`

**Interfaces:**
- Consumes: `TargetRange` from `RemotePolicy.h`.
- Produces (namespace `iheaterlink::policy`):

```cpp
constexpr uint16_t kInputBase = 0x0000;
constexpr uint8_t kInputCount = 22;
constexpr uint16_t kSettingsBase = 0x0200;
constexpr uint8_t kSettingsCount = 10;
constexpr uint16_t kRegTarget = 0x0100;
constexpr uint16_t kRegClear = 0x0101;
constexpr int16_t kNoReading = INT16_MIN;  // wire 0x8000

enum class IheaterState : uint8_t { Booting = 0, Idle = 1, Heating = 2, Hold = 3, Fault = 4 };

struct IheaterSnapshot {
  uint16_t proto, fwVersion, fwPatch;
  IheaterState state;
  uint8_t activeFault;
  bool clearableNow;
  uint16_t clearAllowMask;
  uint8_t clearsLeft, lastClearResult;
  int16_t airT10, heaterT10, auxT10, airTargetT10, heaterSetT10;
  uint16_t pwmPermille;
  bool fanOn;
  uint32_t uptimeS;
  uint16_t linkLossCount, holdRemainingS, resetCause;
  uint8_t lastFault;
};
bool decodeInputs(const uint16_t *regs, uint8_t count, IheaterSnapshot &out);  // false unless count == 22

struct IheaterLimits {
  int16_t maxHeaterT10, maxAirT10, minTargetT10, maxAirTargetT10;
  uint16_t linkHoldS;
  int16_t deltaT10, fanOnT10, fanOffT10, fanOnActiveT10, clearMarginT10;
};
bool decodeSettings(const uint16_t *regs, uint8_t count, IheaterLimits &out);  // false unless count == 10

// Text for the UI. Codes 0x01–0x08 and 0xFF per the contract's fault table;
// anything else "unknown fault".
const char *faultName(uint8_t code);

struct RangeT10 { int16_t minT10; int16_t maxT10; };
// Setup range (whole °C) intersected with the iHeater's [min, max]. False when empty.
bool effectiveRangeT10(TargetRange setup, int16_t ihMinT10, int16_t ihMaxT10, RangeT10 &out);
// <= 0 → 0. Else round(c*10) clamped into the effective range; empty range → 0.
uint16_t uartTargetT10(float requestedC, TargetRange setup, int16_t ihMinT10, int16_t ihMaxT10);
```

Fault names: 0x01 "air did not reach target in time", 0x02 "heater not responding", 0x03 "manual shutdown", 0x04 "air thermistor open", 0x05 "heater thermistor open", 0x06 "heater over-temperature", 0x07 "air over-temperature", 0x08 "link lost", 0xFF "unknown".

- [ ] **Step 1: Write the failing tests.**

```cpp
void test_decode_inputs_fields();   // regs with proto 1, fw 0x0102, state 2, air 524, heater 1182, pwm 430, fan 1,
                                    // uptime lo 0x0005 hi 0x0001 → uptimeS == 65541, state == Heating, fanOn
void test_decode_negative_and_invalid();  // air 0xFF9C → -100; heater 0x8000 → kNoReading
void test_decode_wrong_count();     // count 21 → false
void test_decode_settings();        // min 450, max 850, hold 60, margin 100
void test_fault_names();            // 0x07 → "air over-temperature"; 0x42 → "unknown fault"
void test_uart_target_inside();     // {45,85}, 450..850: 60 → 600; 62.36 → 624
void test_uart_target_clamps();     // 40 → 450; 90 → 850; {50,70}: 85 → 700
void test_uart_target_off();        // 0 → 0; -3 → 0
void test_uart_range_empty();       // {11,40} with 450..850: effectiveRangeT10 false; uartTargetT10(30) == 0
```

- [ ] **Step 2: Run** `pio test -e native -f test_uart_link` → FAIL (missing header).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `pio test -e native` → PASS.
- [ ] **Step 5: Commit** `feat(policy): decode iHeater registers and clamp UART targets`.

---

### Task 4: Poll policy — reads, settings, target writes, link status

**Files:**
- Create: `lib/remote-policy/src/UartPollPolicy.h`, `lib/remote-policy/src/UartPollPolicy.cpp` (CRLF)
- Test: `test/test_uart_link/test_main.cpp`

**Interfaces:**
- Consumes: Tasks 2 and 3 (`Reply`, `IheaterSnapshot`, `IheaterLimits`, `uartTargetT10`, `effectiveRangeT10`), `intervalElapsed` from `RemotePolicy.h`.
- Produces (namespace `iheaterlink::policy`):

```cpp
constexpr uint32_t kPollPeriodMs = 200;
constexpr uint32_t kReplyTimeoutMs = 200;
constexpr uint32_t kNoReplyAfterMs = 1000;
constexpr uint32_t kRestartWindowMs = 10000;

enum class LinkStatus : uint8_t { Waiting, Up, Restarting, NoReply };
enum class ClearPhase : uint8_t { Idle, AwaitRestart, Blocked };
enum class PollKind : uint8_t { None, ReadInputs, ReadSettings, WriteTarget, WriteClear };
struct PollRequest { PollKind kind; uint16_t addr; uint16_t value; };  // value = count for reads

class UartPollPolicy {
 public:
  void setDesired(float targetC, TargetRange setup);  // first call makes the desired target known
  PollRequest cycleRead() const;                      // always {ReadInputs, 0x0000, 22}
  PollRequest followUp(uint32_t nowMs) const;         // at most one more request this cycle, or None
  void onReply(const PollRequest &req, const Reply &reply, uint32_t nowMs);
  void onNoReply(const PollRequest &req, uint32_t nowMs);
  LinkStatus link(uint32_t nowMs) const;
  bool hasSnapshot() const;  const IheaterSnapshot &snapshot() const;
  bool hasLimits() const;    const IheaterLimits &limits() const;
  bool protocolSupported() const;  // snapshot proto == 1
  bool desiredKnown() const; uint16_t desiredT10() const; bool rangeEmpty() const;
  ClearPhase clearPhase() const; uint8_t blockedFault() const; bool lastClearOk() const;  // Task 5
};
```

`followUp` order (first match wins; returns None if there is no snapshot, the protocol is unsupported, or the last inputs read failed):
1. Desired known, desired is 0, `airTargetT10 != 0`, state is not Fault → WriteTarget 0 (even without limits).
2. No limits → ReadSettings `{0x0200, 10}`.
3. Auto-clear (Task 5).
4. Desired known, state not Fault, `desiredT10 != airTargetT10`, not the refused value for the current state → WriteTarget.
5. None.

Desired °C is re-converted to tenths whenever limits change. A target write answered with an exception records `(refusedT10, state)`; rule 4 skips while both still match. An iHeater restart (snapshot uptime lower than the previous one) or a reply after the link was NoReply or Restarting drops the limits and the refusal. `link()`: Waiting before any Ok reply; Up when the last Ok reply is within `kNoReplyAfterMs`; Restarting when the clear phase is AwaitRestart and the clear was sent within `kRestartWindowMs`; else NoReply. All time comparisons wrap-safe.

- [ ] **Step 1: Write the failing tests.** A helper builds an Ok inputs `Reply` from an `IheaterSnapshot`-like set of values (proto 1, state Idle, uptime 100, airTarget 0 unless set) and an Ok settings `Reply` (min 450, max 850).

```cpp
void test_cycle_read_is_input_block();     // cycleRead() == {ReadInputs, 0x0000, 22}
void test_no_followup_before_snapshot();   // fresh policy: followUp(0).kind == None
void test_reads_settings_after_first_snapshot();  // after inputs Ok: ReadSettings, addr 0x0200, value 10
void test_no_write_before_desired();       // inputs + settings Ok, desired never set → None
void test_writes_target_when_different();  // setDesired(60,{45,85}) → WriteTarget 0x0100 600; snapshot airTarget 600 → None
void test_off_written_before_limits();     // snapshot airTarget 600, no settings read, setDesired(0,…) → WriteTarget 0
void test_nonzero_waits_for_limits();      // no settings, setDesired(60,…) → ReadSettings
void test_no_target_write_while_faulted(); // state Fault, clearable 0, desired 60 → None
void test_target_refusal_not_retried();    // WriteTarget 600 answered Exception 0x03 → None; setDesired(65) → WriteTarget 650
void test_restart_rereads_settings();      // uptime 100 then 3 → ReadSettings
void test_proto_mismatch_never_writes();   // proto 2, desired 60 → None (also no ReadSettings)
void test_empty_range_desired_zero();      // setup {11,40}, limits 450..850, setDesired(30) → desiredT10 0, rangeEmpty true
void test_link_status_sequence();          // Waiting at 0; Up after reply at 100; still Up at 1000 after misses; NoReply at 1200; Up on next reply
void test_link_status_wraparound();        // last reply at 0xFFFFFF00, now 0x00000100 (512 ms later) → Up
```

- [ ] **Step 2: Run** → FAIL (missing header).
- [ ] **Step 3: Implement** `UartPollPolicy` (clear rules land in Task 5; leave rule 3 returning nothing for now).
- [ ] **Step 4: Run** `pio test -e native` → PASS.
- [ ] **Step 5: Commit** `feat(policy): UART poll policy for reads, settings and target writes`.

---

### Task 5: Poll policy — auto-clear and its verification

**Files:**
- Modify: `lib/remote-policy/src/UartPollPolicy.{h,cpp}`
- Test: `test/test_uart_link/test_main.cpp`

**Interfaces:**
- Consumes: Task 4's class.
- Produces: rule 3 of `followUp`, and `clearPhase()`, `blockedFault()`, `lastClearOk()` behaviour.

Rules:
- **Send:** state Fault, `activeFault != 0`, `clearableNow`, `clearsLeft > 0`, phase Idle, and not (phase Blocked with `blockedFault == activeFault`) → WriteClear `{0x0101, activeFault}`. On sending (reply or not), remember the code, `clearsLeft`, `uptimeS` and the time.
- **Reply Ok, or no reply:** phase AwaitRestart.
- **Exception 0x03:** phase Idle (the next cycle decides again from fresh registers).
- **Exception 0x04:** phase Blocked, `blockedFault` = the code.
- **In AwaitRestart, on each Ok inputs snapshot:**
  - uptime lower than remembered → restart: `lastClearOk = (lastClearResult == 1)`, phase Idle.
  - no restart, `clearsLeft` lower than remembered, state Fault with the same code → phase Blocked (erase failure, reply lost).
  - no restart, `clearsLeft` unchanged, more than `kRestartWindowMs` since sending → phase Idle.
- **Blocked** lifts when `activeFault` differs from `blockedFault`.

- [ ] **Step 1: Write the failing tests.**

```cpp
void test_clear_when_clearable();               // Fault 0x07, clearable 1, clearsLeft 3 → WriteClear 0x0101 value 7
void test_no_clear_when_not_clearable();        // clearable 0 → None
void test_no_clear_without_budget();            // clearsLeft 0 → None
void test_clear_refused_rereads();              // Exception 0x03 → phase Idle; next snapshot clearable → WriteClear again
void test_clear_erase_failure_blocks();         // Exception 0x04 → Blocked, blockedFault 7, None while fault 7;
                                                // fault becomes 0x01 clearable → WriteClear value 1
void test_clear_success_verified();             // Ok → AwaitRestart; link Restarting at +1000 and +5000 with misses;
                                                // snapshot uptime 2, lastClearResult 1, clearsLeft 2, state Idle →
                                                // phase Idle, lastClearOk true, followUp ReadSettings
void test_lost_clear_reply_then_restart_counts(); // onNoReply(WriteClear) then restart snapshot with result 1 → lastClearOk
void test_lost_clear_reply_without_restart_blocks(); // onNoReply(WriteClear); snapshot uptime 101, clearsLeft 2 (was 3),
                                                // Fault 0x07 → Blocked, followUp None
void test_clear_never_landed_retries();         // onNoReply(WriteClear); snapshots with clearsLeft 3, uptime rising,
                                                // past 10 s → phase Idle, WriteClear again
```

- [ ] **Step 2: Run** → FAIL on the new tests.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `pio test -e native` → PASS.
- [ ] **Step 5: Commit** `feat(policy): clear iHeater faults when safe and verify the result`.

---

### Task 6: Manual resume record and the setup-save rule

**Files:**
- Modify: `lib/remote-policy/src/RemotePolicy.{h,cpp}` (CRLF)
- Test: `test/test_remote_policy/test_main.cpp`

**Interfaces:**
- Produces (namespace `iheaterlink::policy`):

```cpp
struct ResumeRecord {
  uint32_t magic;
  uint8_t mode;
  uint16_t timerMinutes;
  uint32_t remainingS;
  uint32_t check;
};
constexpr uint32_t kResumeMagic = 0x52534D31;  // "RSM1"
void sealResume(ResumeRecord &r);   // sets magic and check
// Valid only after a software reset, with matching magic and check, mode <= maxMode,
// and not an expired timer (timerMinutes > 0 with remainingS == 0).
bool resumeValid(const ResumeRecord &r, bool softwareReset, uint8_t maxMode);

// Whether a Setup save must set the target to 0 before applying.
bool setupSaveStopsHeat(bool uartOutput, bool wasKlipper, bool nowKlipper,
                        bool bindingChanged, bool outputChanged);
```

The check is any 32-bit checksum computed over the four fields one by one (never over the raw struct bytes: padding in RTC no-init memory is garbage). `setupSaveStopsHeat`: output changed → true; now Klipper → `!wasKlipper || bindingChanged`; now Manual → pulse: true; UART: `wasKlipper`.

- [ ] **Step 1: Write the failing tests.**

```cpp
void test_resume_sealed_valid();       // mode 3, 30 min, 1200 s, sealed, softwareReset → true
void test_resume_rejects_power_on();   // same record, softwareReset false → false
void test_resume_rejects_zeroed();     // all-zero record → false
void test_resume_rejects_flipped_bit(); // sealed, then remainingS ^= 1 → false
void test_resume_rejects_bad_mode();   // mode 8 with maxMode 7 → false
void test_resume_rejects_expired_timer(); // timerMinutes 30, remainingS 0 → false; timerMinutes 0, remainingS 0 → true
void test_setup_save_rule();           // (uart, manual→manual) false; (pulse, manual→manual) true;
                                       // (uart, klipper→manual) true; (any, klipper→klipper unchanged) false;
                                       // (any, klipper→klipper binding changed) true; (any, manual→klipper) true;
                                       // (uart, outputChanged) true
```

Register the new tests in `main()`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `pio test -e native` → PASS.
- [ ] **Step 5: Commit** `feat(policy): manual resume record and setup-save heat rule`.

---

### Task 7: Output setting and output interface

**Files:**
- Create: `src/controller/OutputPort.h`
- Modify: `src/controller/RmtOutputAdapter.h`, `src/controller/RmtOutputAdapter.cpp`
- Modify: `src/standalone/RemoteSettings.h`, `src/standalone/RemoteSettings.cpp`
- Modify: `src/standalone/StandaloneApp.cpp`

**Interfaces:**
- Produces:

```cpp
// src/controller/OutputPort.h, namespace iheaterlink
class OutputPort {
 public:
  virtual ~OutputPort() = default;
  virtual void begin() = 0;
  virtual void apply(const ControllerOutputCommand &command) = 0;
  virtual ControllerOutputCommand getLastCommand() const = 0;
  virtual bool isEnabled() const = 0;
  virtual void forceFrame() {}
};

// RemoteSettings.h, namespace iheaterlink::standalone
enum class OutputKind : uint8_t { Pulse = 0, Uart = 1 };
OutputKind loadOutputKind();            // NVS "iheater-remote" key "output"; default Pulse; readable before Wi-Fi
bool saveOutputKind(OutputKind kind);
```

- `RmtOutputAdapter` derives from `OutputPort` (its public methods become overrides; `getLastPulseCode()` stays).
- `StandaloneApp.cpp`: `s_rmt` (the old `s_output`) and `OutputPort *s_output`. `setup()` reads `loadOutputKind()` first and keeps it in `s_outputKind`; in this task both kinds point `s_output` at `s_rmt` (UART arrives in Task 8). Every `s_output.` becomes `s_output->`; `getLastPulseCode()` calls go through `s_rmt` and are logged and reported only in pulse mode.
- Setup form field `output` (`pulse` | `uart`). A changed value: `applyIntegrationTarget(0.0f, "output changed", …)` keeping the current target source string, `saveOutputKind`, then `scheduleRestart()`, notice "Restarting to switch the output." An unknown value: error "Unknown output.".
- Setup page: an Output select (Pulse on GPIO3 / UART on GPIO4–5) with the note "Switching restarts the Remote. The iHeater must run the matching build (pulse or UART)."
- Status JSON: `"output": "pulse" | "uart"`.

- [ ] **Step 1: Implement** the above.
- [ ] **Step 2: Gates.** `pio run -e esp32c3-super-mini-remote` → SUCCESS; `pio test -e native` → PASS.
- [ ] **Step 3: Commit** `feat(remote): Output setting and a shared output interface`.

---

### Task 8: UART link task

**Files:**
- Create: `src/standalone/UartLink.h`, `src/standalone/UartLink.cpp`
- Modify: `platformio.ini` (esp32 env build flags `-DIHEATER_UART_TX_PIN=4`, `-DIHEATER_UART_RX_PIN=5`)
- Modify: `src/standalone/StandaloneApp.cpp`

**Interfaces:**
- Consumes: `OutputPort` (Task 7), `UartPollPolicy`, `buildReadRequest`, `buildWriteSingle`, `parseReply` (Tasks 2–5).
- Produces:

```cpp
// namespace iheaterlink::standalone
struct UartLinkView {
  policy::LinkStatus link;
  bool hasSnapshot; policy::IheaterSnapshot snap;
  bool hasLimits; policy::IheaterLimits limits;
  bool protocolSupported, desiredKnown, rangeEmpty;
  policy::ClearPhase clear; uint8_t blockedFault; bool lastClearOk;
};
class UartLink : public OutputPort {
 public:
  void begin() override;  // installs the UART1 driver and starts the task
  void apply(const ControllerOutputCommand &command) override;  // Off → 0 °C; queued for the task
  ControllerOutputCommand getLastCommand() const override;
  bool isEnabled() const override;
  void setSetupRange(policy::TargetRange range);
  UartLinkView view() const;  // copy under the lock
};
```

- ESP-IDF UART driver on `UART_NUM_1`, 115200 8N1, pins from the build flags, RX buffer 256. No Arduino `HardwareSerial`.
- Task: priority 5, stack 4096 (adjust if the high-water mark says so), created in `begin()`. It registers itself with the task watchdog (`esp_task_wdt_add(nullptr)`) and resets it every cycle. Each cycle (`vTaskDelayUntil`, `kPollPeriodMs`): copy in the pending desired target and range (`setDesired` only once `apply()` has been called at least once); flush RX; send `cycleRead()`; read until `parseReply` is not Incomplete or `kReplyTimeoutMs` passes; `onReply`/`onNoReply`; if the read succeeded, the same for `followUp()`; copy the view out. Lock: one `portMUX_TYPE`, held only for those copies.
- `StandaloneApp.cpp`: in UART mode, `s_uart.begin()` runs where `s_rmt.begin()` runs today (before Wi-Fi), `s_output = &s_uart`, and the boot `applyMode(0, "boot", false)` is **not** called (Task 9 decides boot). `saveRange` and `loadDeviceSettings` also call `s_uart.setSetupRange`. Log a line on each link-status change: `[UART] link=<status>`.

- [ ] **Step 1: Implement** the above.
- [ ] **Step 2: Gates.** `pio run -e esp32c3-super-mini-remote` → SUCCESS, RAM and flash usage noted in the report; `pio test -e native` → PASS.
- [ ] **Step 3: Commit** `feat(remote): poll the iHeater over UART1 in its own task`.

---

### Task 9: Reboot recovery and planned restarts

**Files:**
- Modify: `src/standalone/StandaloneApp.cpp`

**Interfaces:**
- Consumes: `ResumeRecord`, `sealResume`, `resumeValid`, `setupSaveStopsHeat` (Task 6); `UartLink::view()` (Task 8).

- **Record:** `RTC_NOINIT_ATTR policy::ResumeRecord s_resume;` Written (sealed) by `applyMode` for manual modes with the mode, `s_timerMinutes` and `timerRemainingSeconds()`, refreshed once a second while a timer runs, and written with mode 0 when a Klipper target is applied. Invalidated (magic 0) on an Output change.
- **Software reset:** `esp_reset_reason()` is `ESP_RST_SW`, `ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT` or `ESP_RST_WDT`.
- **Boot, after `loadRemoteSettings()`:**
  - Manual source, either output: `resumeValid(s_resume, softwareReset, kMaxMode)` → set `s_timerMinutes`, apply the mode with `releasePrinter = false`, then set `s_timerOffAtMs = millis() + remainingS * 1000` when a timer was running; log `[REMOTE] resumed mode=… remaining=…s`. Otherwise `applyMode(0, "boot", false)`.
  - Klipper source, UART: in `loop()`, until done: if Moonraker has already delivered a target, done; else once `s_uart.view().hasSnapshot`, `applyIntegrationTarget(snap.airTargetT10 / 10.0f, "recovered", "klipper")`, done.
  - Klipper source, pulse: unchanged (`applyMode(0, "boot", false)`).
  - Pulse mode keeps its early `applyMode(0, "boot", false)` at the top of `setup()`.
- **OTA:** in UART mode `handleOtaUpload` does not call `applyMode(0, "ota:start", false)`. The `/fw` page's warning line becomes `__HEATNOTE__`, replaced with "Heating is stopped before the update is written." (pulse) or "Heating continues: the iHeater holds its target while the Remote restarts." (UART).
- **Setup save:** the `applyIntegrationTarget(0.0f, "binding changed" | "binding disabled", …)` calls run only when `setupSaveStopsHeat(uart, wasKlipper, nowKlipper, bindingChanged, outputChanged)` is true, where `bindingChanged` is today's `endpointChanged || token changed`.

- [ ] **Step 1: Implement** the above.
- [ ] **Step 2: Gates.** `pio run` → SUCCESS; `pio test -e native` → PASS.
- [ ] **Step 3: Commit** `feat(remote): resume heat after a software restart in UART mode`.

---

### Task 10: Web UI for the iHeater

**Files:**
- Modify: `src/standalone/StandaloneApp.cpp` (`statusJson`, setup form validation, setup page)
- Modify: `src/standalone/RemoteDashboard.h`
- Modify: `README.md` (UART wiring section)

**Interfaces:**
- Consumes: `UartLink::view()`, `faultName`, `effectiveRangeT10`.

- **Status JSON** (`StaticJsonDocument<2048>`): in UART mode an `iheater` object: `link` (`waiting`|`up`|`restarting`|`no-reply`), and once a snapshot exists `state` (`booting`|`idle`|`heating`|`hold`|`fault`), `airC`, `heaterC`, `auxC` (null for no reading), `targetC`, `setpointC`, `pwmPct` (one decimal), `fan`, `holdS`, `fault`, `faultName`, `clearable`, `allowed` (allow-mask bit for the active fault), `clearsLeft`, `lastClearResult`, `clear` (`idle`|`waiting-restart`|`blocked`), `fw` ("major.minor.patch"), `proto`, `protoOk`, and once limits exist `minC`, `maxC`, `rangeEmpty`.
- **Dashboard:** an "iHeater" card in the home grid, hidden unless `output == "uart"`, laid out as the spec's sketch. The fault line reads, in order: "none"; "<name> — waiting for the iHeater to restart"; "<name> — the iHeater could not erase it (power-cycle the iHeater)"; "<name> — needs MODE at power-on" (not allowed); "<name> — no remote clears left (power-cycle the iHeater)"; "<name> — clearing when safe". Warnings: link `no-reply` → "No reply from the iHeater: check the cable and that it runs the UART build."; `protoOk` false → "Unsupported iHeater protocol <n>: display only."; `rangeEmpty` → "Your Setup range is outside the iHeater's <min>–<max> °C: heating is off." The footer sentence about temperatures not being available shows in pulse mode only; the "Output pulse" metric shows in pulse mode only.
- **Setup validation:** in UART mode with limits known, refuse a range whose effective range is empty: "Range must overlap the iHeater's <min>–<max> °C." The range note on the Setup page shows the iHeater's range in UART mode.
- **README:** a short "UART link" section: Output setting, pins (GPIO4 TX → iHeater D+, GPIO5 RX ← iHeater D−, 5V → VBUS, GND), leave 3V3 unconnected at the iHeater, never connect the iHeater's JST 5 V at the same time, link to the iHeater repo's user guide section.

- [ ] **Step 1: Implement** the above.
- [ ] **Step 2: Gates.** All three gates pass.
- [ ] **Step 3: Commit** `feat(remote): show the iHeater's live state in the web UI`.

---

### Task 11: Bench check (ask before flashing)

Not executed by a subagent. Ask the user before any flash to COM21. With approval, flash, set Output to UART, and check against the real iHeater:

- link reaches Up; temperatures and state match the iHeater's LEDs;
- Manual 60 °C → `air_target` reads 600, state heating; off → 0;
- Klipper target via elpis follows within a cycle;
- web OTA in UART mode: log shows no `link=no-reply` and the iHeater never enters hold (or note the longest gap); heat resumes after the restart;
- TX-power Setup save restart in Manual with a timer: mode and remaining time resume;
- unplug the iHeater cable briefly (power off first, per the iHeater guide): UI shows no reply; reconnect recovers;
- record the task's stack high-water mark and free heap.

Fault clearing on hardware only if the user wants to provoke a fault; otherwise the policy tests cover it.
