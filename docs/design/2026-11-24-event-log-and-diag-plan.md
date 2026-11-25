# Event log, temperature graph and UART diag page — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show a condensed event log and a one-hour temperature graph on the
main page, and a login-only `/diag` page with a live, decoded UART console
and a register table.

**Architecture:** Four pure pieces in `lib/remote-policy/` (event ring,
change tracker, temperature history, console ring and decoder with access
tickets), each with native tests. A new firmware module,
`src/standalone/EventFeed.*`, owns the event ring and history and pushes
them over the existing status WebSocket as `"t"`-tagged messages. The UART
task copies raw exchange records into a ring that exists only while a
`/diag` client is subscribed; the main loop decodes and sends them.

**Tech Stack:** C++17, Arduino-ESP32 3.3.12 (pioarduino 55.03.312-1),
links2004/WebSockets 2.7.3, ArduinoJson 6, Unity native tests, inline
HTML/JS/SVG pages with no libraries.

**Spec:** `docs/design/2026-11-24-event-log-and-diag-design.md` (approved
2026-11-24). Read it before starting any task.

## Global Constraints

- Event ring: 64 events; text at most 64 bytes, formatted on the Remote;
  repeats of the same kind and text within 60 s of the entry's latest
  occurrence collapse (count up, `ms` moves, original seq kept).
- Graph: one sample every 10 s, 360 samples; air, heater and target as
  `int16` tenths; "no data" is `policy::kNoReading` (`INT16_MIN`).
- Console: 100 records; raw request up to 32 bytes, raw reply up to 64
  bytes; recorded only while at least one client is subscribed; ring
  allocated on the first subscribe and freed after the last leaves.
- Heap floor: refuse a console subscription when free heap minus the ring
  would fall below 40 KB (40960 B), or allocation fails.
- Socket: the status JSON keeps its shape (one addition: `iheater.regs`,
  Task 8). New messages carry `"t"`: `"ev"`, `"hist"`, `"uart"`; each carries
  `"now"` (Remote `millis()` at send). Pages ignore unknown `"t"` values and
  never pass a `"t"` message to their status handler.
- Backlog frames: at most 16 events or 120 samples per message, one client
  at a time; order on connect is status, then events, then history.
- Subscribe: `{"sub":"uart","ticket":"<32 hex>"}`; unsubscribe:
  `{"sub":""}` or closing the socket.
- No secret in any event, console line or message: no login verifier,
  proof, session token, ticket, API key, Wi-Fi password or pairing token.
- The 200 ms UART poll and its timing are unchanged; the UART task never
  formats text or allocates for the console.
- Canadian English in comments, docs and UI text. LF line endings.
- Commits end with a blank line and
  `Co-Authored-By: <the model that wrote the change> <noreply@anthropic.com>`.
- All five AGENTS.md gates pass before each commit that touches firmware
  (Tasks 6–8); policy-only tasks run the native suite.

Commands used below (Git Bash):

```bash
# native tests (one suite: add -f test_<name>)
export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native > "$TEMP/pio-test.log" 2>&1; tail -25 "$TEMP/pio-test.log"
# firmware build (ESP-IDF refuses MSYSTEM; this wrapper unsets it)
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$TEMP/pio-nomsys.ps1")" run -e esp32c3-super-mini-remote > "$TEMP/pio-build.log" 2>&1; tail -25 "$TEMP/pio-build.log"
```

If `$TEMP/pio-nomsys.ps1` is missing, it is four lines: remove `MSYSTEM` and
`MSYS` from the environment, set `PYTHONIOENCODING=utf-8` and
`PYTHONUTF8=1`, then `& pio @args; exit $LASTEXITCODE`.

## Review Focus

1. **`millis()` wrap (49.7 days).** Collapse, history spacing and the
   page's clock times all use unsigned differences, so nothing jumps or
   stops at the wrap. Tests: Task 1 and Task 3 wrap cases; Task 7 page uses
   `(now - ms) >>> 0`.
2. **Text cut at 64 bytes mid-character.** A cut never leaves half of a
   UTF-8 sequence ("°", "×", "–"), which would make the JSON invalid for
   the browser. Test: Task 1.
3. **The pairing token in Moonraker's unpair reason.** `lastError()` for
   "not paired" contains the token; the event must not. Test: Task 2.
4. **A stalled loop (TLS handshake, flash write) longer than 10 s.** The
   history fills missed slots with "no data" so the time axis stays true,
   and a stall over an hour leaves 360 "no data" samples, not garbage.
   Test: Task 3.
5. **Console overflow and resubscribe.** If the ring wraps between drains
   the page is told exactly how many lines were dropped and the next line
   is the oldest kept; a new first subscriber starts from an empty ring.
   Tests: Task 4. Also, a Remote restart while a page is open: the page
   clears its list and graph on the first backlog frame (`"first":true`)
   instead of mixing old and new sequence numbers (Task 7 bench check).

---

### Task 1: Event ring

**Files:**
- Create: `lib/remote-policy/src/EventLog.h`, `lib/remote-policy/src/EventLog.cpp`
- Test: `test/test_event_log/test_main.cpp`

**Interfaces:**
- Produces (namespace `iheaterlink::policy`):

```cpp
enum class EventKind : uint8_t { Info, Action, Warn, Fault, Error };
const char *eventKindName(EventKind k);  // "info" "action" "warn" "fault" "error"

constexpr size_t kEventTextMax = 64;       // bytes, excluding the NUL
constexpr size_t kEventCapacity = 64;
constexpr uint32_t kEventCollapseMs = 60000;

struct Event {
  uint32_t seq = 0;  // from 1; 0 = none
  uint32_t ms = 0;   // millis() of the latest occurrence
  EventKind kind = EventKind::Info;
  uint16_t count = 1;
  char text[kEventTextMax + 1] = {};
};

// Copies src into out (kEventTextMax + 1 bytes), cutting at kEventTextMax
// bytes without splitting a UTF-8 sequence. Always NUL-terminates.
void copyEventText(char out[kEventTextMax + 1], const char *src);

class EventLog {
 public:
  // Adds an event, or collapses it into the newest entry with the same kind
  // and text when nowMs - entry.ms <= kEventCollapseMs (unsigned). Returns
  // the stored entry (new, or the bumped one with its original seq).
  const Event &add(EventKind kind, const char *text, uint32_t nowMs);
  size_t size() const;
  const Event &at(size_t i) const;  // 0 = oldest
};
```

- [ ] **Step 1: Write the failing tests** in `test/test_event_log/test_main.cpp`
  (same layout as `test/test_override/test_main.cpp`: `setUp`, `tearDown`,
  `main` with `UNITY_BEGIN`/`RUN_TEST`/`UNITY_END`):
  - `test_first_event_has_seq_1`: `add(Info,"a",100)` → `seq==1`, `count==1`, `ms==100`, `size()==1`.
  - `test_ring_keeps_last_64`: add 70 distinct texts → `size()==64`, `at(0).seq==7`, `at(63).seq==70`.
  - `test_repeat_within_60s_collapses`: `add(Warn,"x",1000)`, `add(Warn,"x",61000)` → one entry, `count==2`, `ms==61000`, `seq==1`; returned reference has `seq==1`.
  - `test_repeat_after_60s_is_new`: `add(Warn,"x",1000)`, `add(Warn,"x",61001)` → two entries.
  - `test_collapse_window_moves_with_latest`: adds of "x" at 0, 50000, 100000 → one entry, `count==3`.
  - `test_same_text_other_kind_is_new`: `add(Info,"x",0)`, `add(Warn,"x",1)` → two entries.
  - `test_collapse_only_into_newest_match`: "a"@0, "b"@1, "a"@2 → two entries; "a" has `count==2` (the newest entry with that text, searched newest first, even when another event came between).
  - `test_collapse_across_millis_wrap`: `add(Info,"x",0xFFFFF000)`, `add(Info,"x",0x00000800)` → one entry, `count==2`.
  - `test_text_cut_at_64_bytes`: 80 × 'a' → `strlen==64`.
  - `test_text_cut_keeps_utf8_whole`: 63 × 'a' + "°C" → `strlen==63` (the 2-byte "°" would end at byte 65, so it is dropped whole).
  - `test_kind_names`: all five names as listed.

- [ ] **Step 2: Run and see them fail**

Run: native tests with `-f test_event_log`.
Expected: build fails, `EventLog.h` not found.

- [ ] **Step 3: Implement `EventLog.h/.cpp`** with a fixed `Event[kEventCapacity]` array and a head index; no heap.

- [ ] **Step 4: Run and see them pass**

Run: native tests with `-f test_event_log`. Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/remote-policy/src/EventLog.* test/test_event_log
git commit -m "feat(policy): event ring with repeat collapse"
```

---

### Task 2: Event tracker and event texts

**Files:**
- Create: `lib/remote-policy/src/EventTracker.h`, `lib/remote-policy/src/EventTracker.cpp`
- Test: `test/test_event_tracker/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 `EventKind`, `kEventTextMax`, `copyEventText`;
  `LinkStatus`, `ClearPhase` (`UartPollPolicy.h`); `faultName`
  (`UartRegisters.h`).
- Produces:

```cpp
enum class WifiState : uint8_t { Connecting, Joined, Lost, Ap };
enum class MoonState : uint8_t { Off, Disconnected, Connecting, NotReady, Ready };

struct TrackedState {
  WifiState wifi = WifiState::Connecting;
  MoonState moon = MoonState::Off;   // Off while the source is Manual
  bool paired = false;               // MoonrakerLink::heaterObject() non-empty
  bool uart = false;                 // the fields below apply only with UART output
  LinkStatus link = LinkStatus::Waiting;
  bool hasSnapshot = false;
  bool protocolSupported = true;
  uint16_t proto = 0;
  uint8_t fault = 0;                 // snap.activeFault
  uint32_t ihUptimeS = 0;
  uint16_t resetCause = 0;
  ClearPhase clear = ClearPhase::Idle;
  bool lastClearOk = false;
};

struct TrackedEvent {
  EventKind kind;
  char text[kEventTextMax + 1];
};

class EventTracker {
 public:
  // Compares `now` with the previous call (the first call compares with a
  // default TrackedState) and writes up to `max` events, in the order of
  // the rules below. Returns how many were written.
  size_t update(const TrackedState &now, TrackedEvent *out, size_t max);
};

// Text for a target change applied with the given source string (the
// `source` argument of applyMode / applyIntegrationTarget). False when the
// change is not worth an event ("boot").
bool targetEventText(const char *source, float targetC, char out[kEventTextMax + 1]);

// A fixed text for MoonrakerLink::lastError() at unpair time. Never copies
// the input: the "not paired" reason contains the pairing token.
const char *unpairReasonText(const char *error);
```

Rules and exact texts (these are the event wording; the tests pin them):

| Change | Kind | Text |
|---|---|---|
| wifi → Joined | Info | `Wi-Fi connected` |
| wifi Joined → Lost | Warn | `Wi-Fi lost` |
| wifi → Ap | Warn | `Hotspot started` |
| moon → Ready | Info | `Moonraker connected, Klipper ready` |
| moon → NotReady | Warn | `Klipper not ready` |
| moon Ready/NotReady → Disconnected or Connecting | Warn | `Moonraker disconnected` |
| paired false → true | Info | `Paired with Klipper` |
| link → Up (uart) | Info | `iHeater link up` |
| link → NoReply (uart) | Error | `iHeater not replying` |
| link → Updating (uart) | Info | `iHeater in its bootloader` |
| protocolSupported true → false (with snapshot) | Error | `iHeater link protocol %u not supported` |
| fault 0 → n, or n → m (with snapshot) | Fault | `iHeater fault: %s` (`faultName(n)`) |
| fault n → 0 (with snapshot) | Info | `iHeater fault cleared` |
| ihUptimeS went backwards (snapshot before and now) | Warn | `iHeater restarted (reset cause 0x%04X)` |
| clear Idle → AwaitRestart | Action | `Fault clear sent` |
| clear AwaitRestart → Idle | Info / Warn | `Fault clear worked` / `Fault clear did not work` (by `lastClearOk`) |
| clear → Blocked | Error | `iHeater could not clear the fault: power-cycle it` |

`targetEventText`: target 0 → `Heat off (<who>)`, else `Target <t> °C (<who>)`
with `<t>` to one decimal place and a trailing `.0` dropped (`65`, `62.5`).
`<who>`: `web`, `button:click`, `button:long` → `manual press`; `klipper` →
`Klipper`; `timer` → `timer ended`; `timer: back to klipper` → `timer ended,
back to Klipper`; `back to klipper` → `back to Klipper`; anything else →
the source as given. `boot` → returns false.

`unpairReasonText`: starts with `not paired` → `Not paired: no heater has
this Remote's token`; contains `more than one` → `Pairing token used by
more than one heater`; starts with `no heater_iheater_remote` → `No
heater_iheater_remote in Klipper`; else `Unpaired from Klipper`.

- [ ] **Step 1: Write the failing tests**, one per table row plus:
  - `test_first_update_from_defaults`: `{wifi=Joined}` → exactly one event `Wi-Fi connected`.
  - `test_no_change_no_events`: same state twice → second call returns 0.
  - `test_link_fields_ignored_without_uart`: `uart=false`, `link=NoReply` → 0 events.
  - `test_fault_without_snapshot_ignored`: `hasSnapshot=false`, `fault=3` → 0 events.
  - `test_restart_on_uptime_backwards`: snapshot uptime 500, then snapshot uptime 2 with resetCause 4 → one `iHeater restarted (reset cause 0x0004)` event.
  - `test_restart_needs_snapshot_both_times`: snapshot uptime 500, then no snapshot, then snapshot uptime 2 → no restart event on either later call.
  - `test_max_limits_output`: a change producing 3 events with `max=2` → returns 2.
  - `test_target_text_manual_press`: `("web",65)` → `Target 65 °C (manual press)`.
  - `test_target_text_half_degree`: `("klipper",62.5)` → `Target 62.5 °C (Klipper)`.
  - `test_target_text_off_timer`: `("timer",0)` → `Heat off (timer ended)`.
  - `test_target_text_unknown_source`: `("printer link lost 10 min",0)` → `Heat off (printer link lost 10 min)`.
  - `test_target_text_boot_skipped`: `("boot",0)` → false.
  - `test_unpair_reason_hides_token`: input `not paired: add remote_token: ABCD-EFGH to your [heater_iheater_remote] section` → result does not contain `ABCD`, equals the fixed text.
  - `test_unpair_reason_other`: `no reply` → `Unpaired from Klipper`.

- [ ] **Step 2: Run and see them fail** (`-f test_event_tracker`). Expected: header not found.

- [ ] **Step 3: Implement `EventTracker.h/.cpp`.** Keep the previous `TrackedState` as a member; format with `snprintf` into a local buffer then `copyEventText`.

- [ ] **Step 4: Run and see them pass** (`-f test_event_tracker`). Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/remote-policy/src/EventTracker.* test/test_event_tracker
git commit -m "feat(policy): event tracker and event texts"
```

---

### Task 3: Temperature history

**Files:**
- Create: `lib/remote-policy/src/TempHistory.h`, `lib/remote-policy/src/TempHistory.cpp`
- Test: `test/test_temp_history/test_main.cpp`

**Interfaces:**
- Consumes: `kNoReading` (`UartRegisters.h`).
- Produces:

```cpp
struct HistSample { int16_t airT10, heaterT10, targetT10; };  // kNoReading = no data

class TempHistory {
 public:
  static constexpr size_t kCapacity = 360;
  static constexpr uint32_t kPeriodMs = 10000;
  // The first call samples at once. Later calls sample when at least
  // kPeriodMs has passed since the newest sample; k whole periods passed →
  // k-1 no-data samples (at most kCapacity), then `current`. The newest
  // sample's time stays on the 10 s grid (newest + k * kPeriodMs).
  // Returns the number of samples added.
  size_t tick(uint32_t nowMs, const HistSample &current);
  size_t size() const;
  const HistSample &at(size_t i) const;  // 0 = oldest
  uint32_t newestMs() const;              // time of at(size() - 1)
};
```

- [ ] **Step 1: Write the failing tests:**
  - `test_first_tick_samples`: `tick(5000, s)` → 1, `size()==1`, `newestMs()==5000`.
  - `test_no_sample_before_period`: then `tick(14999, s)` → 0.
  - `test_sample_on_grid`: then `tick(15300, s2)` → 1, `newestMs()==15000`, `at(1)` equals s2.
  - `test_stall_fills_no_data`: first at 0, then `tick(35000, s)` → 3 added; `at(1)` and `at(2)` are all `kNoReading`; `at(3)==s`; `newestMs()==30000`.
  - `test_ring_keeps_360`: 400 ticks 10 s apart → `size()==360`, `at(359)` is the last sample.
  - `test_stall_over_an_hour`: first at 0, then `tick(5*3600*1000, s)` → `size()==360`, `at(0..358)` no data, `at(359)==s`.
  - `test_period_across_millis_wrap`: first at `0xFFFFE000`, then `tick(0x00001800, s)` → 1 added (0x3800 ms later), `newestMs()==0xFFFFE000 + 10000` (unsigned).

- [ ] **Step 2: Run and see them fail** (`-f test_temp_history`).

- [ ] **Step 3: Implement** with a fixed `HistSample[kCapacity]` ring (about 2.2 KB).

- [ ] **Step 4: Run and see them pass** (`-f test_temp_history`).

- [ ] **Step 5: Commit**

```bash
git add lib/remote-policy/src/TempHistory.* test/test_temp_history
git commit -m "feat(policy): ten-second temperature history"
```

---

### Task 4: Console ring and decoder

**Files:**
- Create: `lib/remote-policy/src/UartConsole.h`, `lib/remote-policy/src/UartConsole.cpp`
- Modify: `lib/remote-policy/src/UartFrames.h/.cpp` only if the Modbus CRC-16 is not already public: expose it as `uint16_t modbusCrc(const uint8_t *data, size_t len);`
- Test: `test/test_uart_console/test_main.cpp`

**Interfaces:**
- Consumes: `UartFrames.h` constants, `decodeInputs`, `faultName`,
  `kRegBootloader`, `kBootloaderMagic`, `kRegCommand`, `kCmdErase`,
  `kCmdBoot`, `kRegChunk`, `kNoReading`.
- Produces:

```cpp
enum class ConsoleResult : uint8_t { Ok, Exception, Timeout, BadReply };
constexpr size_t kConsoleReqBytes = 32, kConsoleReplyBytes = 64, kConsoleLines = 100;

struct ConsoleRecord {
  uint32_t seq = 0, ms = 0;
  ConsoleResult result = ConsoleResult::Ok;
  uint8_t code = 0;            // exception code for Exception
  uint16_t reqLen = 0, replyLen = 0;   // full lengths; bytes below are cut
  uint8_t req[kConsoleReqBytes] = {};
  uint8_t reply[kConsoleReplyBytes] = {};
};

// Fills a record from one exchange (copies at most 32 / 64 bytes).
void makeConsoleRecord(ConsoleRecord &r, const uint8_t *req, size_t reqLen,
                       const uint8_t *reply, size_t replyLen,
                       ConsoleResult result, uint8_t code, uint32_t ms);

// Ring over caller-owned storage. One writer, one reader; not thread-safe
// (the firmware holds its spinlock around each call).
class ConsoleRing {
 public:
  ConsoleRing(ConsoleRecord *storage, size_t capacity);
  void push(const ConsoleRecord &r);   // assigns seq (from 1)
  // Copies up to `max` records the reader has not seen, oldest first.
  // `dropped` = records overwritten before the reader saw them (0 if none).
  size_t drain(ConsoleRecord *out, size_t max, uint32_t &dropped);
};

// "<request> → <reply>", NUL-terminated, cut to outLen - 1 bytes.
void describeExchange(const ConsoleRecord &r, char *out, size_t outLen);
```

Decoder texts (pinned by the tests; registers in hex, ranges inclusive, `→`
is U+2192):

- FC04 → `read inputs 0x%04X–0x%04X`; FC03 → `read holding 0x%04X–0x%04X`
  (`–` is U+2013).
- FC06 to `kRegBootloader` with `kBootloaderMagic` → `enter bootloader`;
  to `kRegCommand` with 1 / 2 → `erase` / `boot`; else
  `write 0x%04X = %u`.
- FC10 to `kRegChunk` → `chunk %u` (the first register value, bytes 7–8);
  else `write 0x%04X ×%u` (quantity).
- Anything else → `FC 0x%02X at 0x%04X`; fewer than 6 request bytes →
  `request of %u bytes`.
- Replies: Timeout → `no reply`; Exception → `exception 0x%02X`; BadReply
  → `bad CRC` when the captured reply is whole (`replyLen <= 64`, at least
  4 bytes) and its CRC fails, `reply too long` when `replyLen > 64`, else
  `reply does not match`; Ok write → `ok`; Ok FC04 read of 22 registers at
  0x0000 with the whole reply captured → `proto %u, air %s, heater %s, PWM
  %u%%, fault %s` (temperatures `45.2 °C` or `—`; PWM = permille / 10;
  fault `none` or `faultName`); any other Ok read → `ok, %u registers`.

- [ ] **Step 1: Write the failing tests** (build frames with the existing
  `buildReadRequest` / `buildWriteSingle` / `buildWriteMultiple`, and replies
  by hand with the CRC):
  - `test_ring_seq_and_order`: push 3, drain(max 10) → 3, seqs 1,2,3, `dropped==0`; drain again → 0.
  - `test_ring_overflow_reports_dropped`: capacity 100, push 130 without draining → drain(max 200) returns 100, `dropped==30`, first seq 31.
  - `test_ring_drain_in_batches`: push 20; drain(max 8) three times → 8, 8, 4, seqs continuous.
  - `test_new_ring_starts_empty`: a fresh `ConsoleRing` on reused storage drains 0 and its first push has seq 1.
  - `test_record_cuts_bytes`: 137-byte request, 70-byte reply → `reqLen==137`, `replyLen==70`, first 32 / 64 bytes copied.
  - `test_describe_read_inputs`: FC04 0x0000×22, Ok reply with proto 1, air 452, heater 801, pwm 350, fault 0 → `read inputs 0x0000–0x0015 → proto 1, air 45.2 °C, heater 80.1 °C, PWM 35%, fault none`.
  - `test_describe_no_reading`: same with air `kNoReading` → contains `air —`.
  - `test_describe_enter_bootloader`: FC06 0x0102=0xB007, Ok → `enter bootloader → ok`.
  - `test_describe_erase_and_boot`: FC06 0x0300=1 / =2 → `erase → ok` / `boot → ok`.
  - `test_describe_chunk`: FC10 to 0x0310 with first register 12 → starts `chunk 12`.
  - `test_describe_write_target`: FC06 0x0100=450 → `write 0x0100 = 450 → ok`.
  - `test_describe_exception`: Exception code 2 → ends `→ exception 0x02`.
  - `test_describe_timeout`: → ends `→ no reply`.
  - `test_describe_bad_crc`: whole 7-byte reply with a flipped CRC byte → ends `→ bad CRC`.
  - `test_describe_too_long`: `replyLen` 90 → ends `→ reply too long`.
  - `test_describe_unknown_fc`: FC 0x2B at 0x0000 → starts `FC 0x2B at 0x0000`.
  - `test_describe_cuts_to_buffer`: `outLen` 16 → `strlen==15`.

- [ ] **Step 2: Run and see them fail** (`-f test_uart_console`).

- [ ] **Step 3: Implement `UartConsole.h/.cpp`** (and expose the CRC if needed).

- [ ] **Step 4: Run and see them pass** (`-f test_uart_console`), then the whole native suite. Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/remote-policy/src/UartConsole.* lib/remote-policy/src/UartFrames.* test/test_uart_console
git commit -m "feat(policy): UART console ring and line decoder"
```

---

### Task 5: Console access rules

The socket cannot see the browser's `HttpOnly` session cookie from script,
and the WebSocket library gives header values without the client number.
So the logged-in `/diag` page asks the web server for a one-time ticket
(`POST /api/diag/ticket`, which goes through the normal login and origin
checks, proxy rules included) and sends it with `{"sub":"uart"}`. This is
the plan's way of "checking the session the same way the web server does".

**Files:**
- Modify: `lib/remote-policy/src/UartConsole.h/.cpp`
- Modify: `lib/remote-policy/src/AuthPolicy.cpp` (`classifyRequest`)
- Test: `test/test_uart_console/test_main.cpp`, `test/test_auth_policy/test_main.cpp`

**Interfaces:**
- Produces:

```cpp
constexpr size_t kTicketLen = 16;
constexpr uint32_t kConsoleHeapFloor = 40960;

class ConsoleTickets {
 public:
  static constexpr size_t kSlots = 4;
  static constexpr uint32_t kLifetimeMs = 60000;
  void issue(const uint8_t ticket[kTicketLen], uint32_t nowMs);   // a full table drops the oldest
  bool redeem(const uint8_t ticket[kTicketLen], uint32_t nowMs);  // once, within the lifetime
};
bool parseTicketHex(const char *text, uint8_t out[kTicketLen]);  // exactly 32 hex digits
bool consoleHeapOk(uint32_t freeHeap, size_t ringBytes);         // freeHeap >= ringBytes + kConsoleHeapFloor
```

- `classifyRequest("/diag", false, …)` → `ProtectedPage`.

- [ ] **Step 1: Write the failing tests:**
  - `test_ticket_redeems_once`: issue T at 0 → `redeem(T,1000)` true, again false.
  - `test_ticket_expires`: issue at 0 → `redeem(T,60001)` false.
  - `test_ticket_expiry_across_wrap`: issue at `0xFFFFFF00` → `redeem(T,0x00000100)` true.
  - `test_unknown_ticket_refused`: `redeem` of a never-issued ticket → false; an all-zero ticket → false even on an empty table.
  - `test_fifth_ticket_drops_oldest`: issue T1..T5 → T1 refused, T5 accepted.
  - `test_parse_ticket_hex`: 32 hex (mixed case) → true; 31 or 33 chars, or a `g` → false.
  - `test_console_heap_floor`: `consoleHeapOk(40960+11000, 11000)` true; `(40959+11000, 11000)` false.
  - In `test_auth_policy`: `test_classify_diag_is_protected`.

- [ ] **Step 2: Run and see them fail** (`-f test_uart_console -f test_auth_policy`).

- [ ] **Step 3: Implement.** Compare tickets in constant time (as `AuthCrypto`/`SessionTable` do for session tokens).

- [ ] **Step 4: Run and see them pass** (whole native suite).

- [ ] **Step 5: Commit**

```bash
git add lib/remote-policy/src/UartConsole.* lib/remote-policy/src/AuthPolicy.cpp test/test_uart_console test/test_auth_policy
git commit -m "feat(policy): one-time console tickets and /diag protection"
```

---

### Task 6: Event and history feed in the firmware

**Files:**
- Create: `src/standalone/EventFeed.h`, `src/standalone/EventFeed.cpp`
- Modify: `src/standalone/StandaloneApp.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces (namespace `iheaterlink::standalone`), used by Tasks 7–8:

```cpp
void feedBegin(WebSocketsServer &socket);
// Formats (vsnprintf into kEventTextMax + 1 bytes), adds to the ring and
// broadcasts the stored entry as a one-event "ev" message.
void feedRecord(policy::EventKind kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
// Call from the status socket's event handler after the status is sent.
void feedOnSocketEvent(uint8_t client, WStype_t type, const uint8_t *payload, size_t len);
// Once per loop: one backlog frame at most, history sampling (broadcasts
// each new sample as a one-sample "hist" message).
void feedLoop(uint32_t nowMs, const policy::HistSample &current);
```

Message shapes (arrays, not objects per entry):

```text
{"t":"ev","now":N,"first":true,"e":[[seq,ms,"kind",count,"text"],...]}
{"t":"hist","now":N,"first":true,"step":10000,"ms":M,"s":[[air,heater,target],...]}
```

- `"first":true` only on the first frame of a client's backlog; a page
  clears its list or graph on it. Each client always gets at least one
  `"ev"` and one `"hist"` frame (empty arrays when there is nothing).
- `"ms"` in a `"hist"` frame is the time of its last sample; sample *i* of
  *n* is at `ms - (n-1-i) * step`. No-data values are JSON `null`.
- Backlog: on `WStype_CONNECTED` queue the client (FIFO sized
  `WEBSOCKETS_SERVER_CLIENT_MAX`); `feedLoop` sends at most one frame per
  call, for the queue's head, ≤16 events or ≤120 samples; drop a client
  from the queue on `WStype_DISCONNECTED`. Build each frame with
  ArduinoJson (escapes the text) into a `String` freed after sending.
- `HistSample` for `feedLoop` (StandaloneApp): UART output with
  `hasSnapshot` and link Up → `snap.airT10`, `snap.heaterT10`; else no
  data. Target: `getLastCommand()` in TargetTemperature mode → tenths;
  else no data. Pulse output → all no data.

Event hooks in `StandaloneApp.cpp` (Serial lines stay as they are):

- `setup()` after `feedBegin`: `Remote started (<reason>)` (Info), with a
  short name for `esp_reset_reason()` (power-on, software, panic,
  watchdog, brownout, other).
- Each loop: fill a `policy::TrackedState` (Wi-Fi from `s_apMode` and
  `WiFi.status()`; Moonraker from `stateString()` and `heaterObject()`,
  `Off` unless `klipperSource()`; UART fields from `s_uart.view()`), run
  the tracker, `feedRecord` each event. Throttle to every 250 ms like
  `pushStatusIfChanged`.
- `applyMode` and `applyIntegrationTarget`: `targetEventText(source, target)`
  → Action event when it returns true.
- `applyMode`: when `s_override.press(...)` starts an override (returns
  true) → Action `Manual override of Klipper started`.
- `onUnpaired` callback → Warn `unpairReasonText(s_moonraker.lastError())`.
- `handleTimerPost` → Action `Timer set to %u min` or `Timer off`.
- `handleIheaterStart` (queued) → Action `iHeater update started`;
  `logUartFlash` on a new outcome → Info `iHeater updated` for success,
  else Error `iHeater update failed: <outcome> <detail>` (cut to 64).
- OTA: upload start → Action `Remote update started`; failure → Error
  `Remote update failed: <s_otaError>`; success → Info `Remote updated,
  restarting`.
- `handleSetupPost` saved → Action `Setup saved`; `handleWifiPost` /
  `handleWifiClearPost` → Action `Wi-Fi settings saved, restarting` /
  `Wi-Fi forgotten, restarting`.

- [ ] **Step 1: Implement `EventFeed.*`** and the hooks above;
  `onStatusSocketEvent` keeps sending the status first, then calls
  `feedOnSocketEvent`; `loop()` calls `feedLoop` after `s_statusSocket.loop()`.
- [ ] **Step 2: Check that no event text can carry a secret:** `grep -n "feedRecord" src/standalone/*.cpp` and read each call: none passes the pairing token, `lastError()` raw, a password, ticket or session value.
- [ ] **Step 3: Run all five gates.** Expected: build SUCCESS, native PASS, three Python suites OK.
- [ ] **Step 4: Commit**

```bash
git add src/standalone/EventFeed.* src/standalone/StandaloneApp.cpp
git commit -m "feat(remote): push events and temperature history over the status socket"
```

---

### Task 7: Graph and event log on the dashboard

**Files:**
- Modify: `src/standalone/RemoteDashboard.h`
- Modify: `src/standalone/StandaloneApp.cpp` (only the `/fw` card's socket handler, if it parses every message as status)

**Interfaces:**
- Consumes: Task 6 message shapes.

- [ ] **Step 1: Route socket messages.** `connectStatus()`'s `onmessage`:
  parse; `"ev"` → events, `"hist"` → graph, any other `"t"` → ignore;
  no `"t"` → `applyStatus`. Do the same in the `/fw` page's `conn()`.
- [ ] **Step 2: Graph card** (home view, wide, after the iHeater card,
  hidden with pulse output): inline SVG, air, heater and target lines,
  breaks at `null`, a clock-time axis (browser local time) over the last
  hour, the latest values in the legend. Store samples keyed by their
  time so a live sample that arrives during the backlog is not doubled;
  drop samples older than one hour of Remote time.
- [ ] **Step 3: Event list card** under the graph: newest first by `ms`
  then `seq`; replace an entry with the same `seq` (collapsed repeat,
  shown as `×N` when count > 1); fault and error kinds marked with
  `--bad`, warn with `--warn`; clock time `new Date(Date.now() - ((now -
  ms) >>> 0))`, shown as time of day (with the date when not today).
  Clear both on `"first":true`.
- [ ] **Step 4: Link to `/diag`** as a rail tab (like Setup), title
  "Diagnostics".
- [ ] **Step 5: Run all five gates.** Expected: all pass.
- [ ] **Step 6: Commit**

```bash
git add src/standalone/RemoteDashboard.h src/standalone/StandaloneApp.cpp
git commit -m "feat(remote): temperature graph and event log on the dashboard"
```

Bench check (after Task 8, with the owner): the page fills from the
backlog on load; a new event appears live; restarting the Remote with the
page open clears and refills the list (no mixed sequence numbers).

---

### Task 8: UART console, `/diag` page and register table

**Files:**
- Modify: `src/standalone/UartLink.h`, `src/standalone/UartLink.cpp`
- Modify: `src/standalone/EventFeed.h`, `src/standalone/EventFeed.cpp`
- Create: `src/standalone/DiagPage.h`
- Modify: `src/standalone/StandaloneApp.cpp`
- Modify: `README.md`

**Interfaces:**
- Consumes: Tasks 4–6.
- Produces:

```cpp
// UartLink
bool consoleStart();   // allocates kConsoleLines records; false if consoleHeapOk fails or new fails
void consoleStop();    // frees the ring
size_t consoleDrain(policy::ConsoleRecord *out, size_t max, uint32_t &dropped);

// EventFeed
void feedBeginConsole(UartLink *uart);  // nullptr with pulse output
void feedIssueTicket(char hexOut[2 * policy::kTicketLen + 1]);  // random (esp_fill_random), stored in ConsoleTickets
```

- [ ] **Step 1: Console tap in `UartLink`.** A `ConsoleRing *console_`
  and storage pointer, set and cleared under `mux_`; a `volatile bool
  consoleOn_` read without the lock. In `exchangeFrame`, at each return,
  when `consoleOn_`: build a `ConsoleRecord` with `makeConsoleRecord`
  (Ok / Exception with code / BadReply / Timeout from `reply.kind` and the
  return value), then under `mux_` push it if `console_` is still set. No
  formatting, no allocation in the task. `consoleStart` allocates outside
  the lock with `new (std::nothrow)`, then publishes under the lock;
  `consoleStop` unpublishes under the lock, then frees. `consoleDrain`
  copies at most 8 records per call under the lock.
- [ ] **Step 2: Subscriptions in `EventFeed`.** Per-client
  `bool subscribed[WEBSOCKETS_SERVER_CLIENT_MAX]`. On `WStype_TEXT`, parse
  with a small `StaticJsonDocument`:
  - `{"sub":"uart","ticket":t}`: pulse output → `{"t":"uart","now":N,"state":"pulse"}`;
    `parseTicketHex` and `redeem` fail → `"state":"login"`; first
    subscriber and `consoleStart()` fails → `"state":"memory"`; else mark
    subscribed → `"state":"on"`.
  - `{"sub":""}` or `WStype_DISCONNECTED` → unsubscribe; when none are left,
    `consoleStop()`.
  - Anything else is ignored, as now.
- [ ] **Step 3: Console lines.** In `feedLoop`, when anyone is subscribed,
  `consoleDrain` up to 8 records and send one message to each subscriber:

  ```text
  {"t":"uart","now":N,"drop":D,"l":[[seq,ms,"text","reqHex","replyHex",reqLen,replyLen],...]}
  ```

  `text` from `describeExchange` (96-byte buffer); hex of the captured
  bytes only; `"drop"` only when D > 0.
- [ ] **Step 4: Routes.** `GET /diag` → `guarded(handleDiagPage)` (sends
  `kDiagHtml` from flash); `POST /api/diag/ticket` → `guarded(...)`
  returning `{"ticket":"<hex>"}`. Never print the ticket to Serial.
- [ ] **Step 5: Register table data.** In `statusJson`, under `iheater`
  and only with a snapshot: `"regs":{...}` holding every
  `IheaterSnapshot` field, and with limits every `IheaterLimits` field,
  and in the bootloader `why`, `appCheck`, `timeoutS`, each keyed by its
  C++ field name with its raw number. Raise the `StaticJsonDocument` from
  3072 to 4096 and log once to Serial if `doc.overflowed()`.
- [ ] **Step 6: `DiagPage.h`** (`kDiagHtml`, same palette and fonts as
  the dashboard, link back to `/`): the register table from status
  pushes (field name, raw value, and °C for `*T10` fields); the console
  (newest at the bottom, auto-scroll unless the user scrolled up), with
  Pause (stops appending; the Remote keeps recording), Show bytes (shows
  `reqHex` / `replyHex`, noting when `reqLen`/`replyLen` exceed what was
  captured), and Clear (view only); `"N lines dropped"` rows; on each
  socket open: `POST /api/diag/ticket`, then send the subscribe message;
  a 401/403 or `"state":"login"` → "Log in to see the console" with a
  `/login?next=/diag` link; `"memory"` → "The Remote is short of memory;
  try again later"; `"pulse"` or status `output==="pulse"` → "The UART
  console needs UART output". Ignore `"ev"` and `"hist"` messages.
- [ ] **Step 7: README.** A short "Event log and diagnostics" section:
  what the main page shows, that history is lost on restart, that `/diag`
  needs a login, and that the console records only while open.
- [ ] **Step 8: Run all five gates.** Expected: all pass. Note the
  build's RAM figure against the previous build (expect about +7 KB
  static for the event ring and history).
- [ ] **Step 9: Commit**

```bash
git add src/standalone/UartLink.* src/standalone/EventFeed.* src/standalone/DiagPage.h src/standalone/StandaloneApp.cpp README.md
git commit -m "feat(remote): login-only UART console and register table on /diag"
```

Bench check (with the owner, no flashing without asking): `/diag` while
logged out asks for a login; logged in, lines arrive about five a second;
Show bytes shows the frames; closing the tab frees the ring (heap log line
returns to its earlier value); an iHeater update shows `enter bootloader`,
`erase`, `chunk N` and `boot` lines.
