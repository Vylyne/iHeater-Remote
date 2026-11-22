# Arduino-ESP32 3.x Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run the C3 firmware on Arduino-ESP32 3.3.12, with a pulse
train identical to the 2.0.17 one.

**Architecture:**
- First, a capture-comparison tool and a pure, tested frame builder land on
  2.0.17. The builder pins today's waveform in native tests.
- Then one task switches the platform, ports the RMT output to the ESP-IDF 5
  transmit driver on top of that builder, and fixes the watchdog call and any
  other build breaks.
- A bench task proves the result on COM23 against the 2.0.17 captures.

**Tech Stack:**
- pioarduino platform `55.03.312-1` (Arduino-ESP32 3.3.12, ESP-IDF 5.5.5);
- C++ with Unity native tests;
- Python 3 `unittest` for the capture tool;
- Saleae Logic 2 CSV exports.

**Spec:** docs/design/2026-11-21-arduino-3x-upgrade-design.md

## Global Constraints

- Platform: `platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip`.
- Waveform:
  - SYNC LOW 120 000 µs;
  - each count is HIGH 2000 µs, then LOW 2000 µs;
  - a frame every 1000 ms from the dedicated `rmt_tx` task (stack 3072,
    priority `tskIDLE_PRIORITY + 2`);
  - idle low;
  - Off is 10 pulses; a target is its °C value in pulses (45–80).
- Measured 2.0.17 baseline (`logic captures/`, git-ignored), in steady state:
  - HIGH and LOW are both 2000.2 µs;
  - frames start every 1000.1 ms;
  - Off sends 10 pulses and 60 °C sends 60.
- Bench tolerances:
  - every width within ±10 µs of the baseline mean;
  - the frame period within ±5 ms of the baseline mean;
  - the same pulse count per frame;
  - every low gap between frames at least 122 ms (the 120 ms SYNC plus the
    last 2 ms LOW).
- The legacy RMT driver (`driver/rmt.h`, `rmt_driver_install`) must not be
  linked: with the new driver also linked, it aborts at boot.
- NVS stays where it is, so settings and the pairing token survive.
- Libraries stay ArduinoJson `^6.21.3` and WebSockets `2.7.3` unless they fail
  to build. A library that fails moves to the oldest version that builds on
  3.x.
- Line endings:
  - Keep each file's existing line endings.
  - New files are LF, following `.gitattributes` (`* text=auto eol=lf`).
  - The policy files are LF today, despite AGENTS.md's CRLF note.
- Write Canadian English in comments and docs.
- Flashing: COM23 and COM24 are authorised; say so each time you upload.
  Never touch the Remote on the printer.
- Commit messages end with a blank line, then a `Co-Authored-By` trailer
  naming the model that wrote the change.
- Run all five gates (AGENTS.md) before each commit, with `pio` output
  redirected to a file.

## Review Focus

1. **A setting changes while a frame is going out.** `forceFrame()` wakes the
   task mid-frame. The current frame must finish unchanged, and the next one
   carries the new count (Task 4, step 5: the switching capture).
2. **The line at boot or reset.** The STM32 counts rising edges, so a glitch
   high before the first SYNC would be read as a pulse. The capture across a
   reset shows no stray pulse (Task 4, step 5).
3. **OTA from 2.0.17 straight to 3.x.** This is how the printer's Remote will
   be updated, with its 2.0.17 bootloader. If the partitions are unchanged,
   COM23 gets 3.x by OTA from its running 2.0.17, not over USB (Task 4,
   step 2).
4. **The watchdog on 3.x.** If 3.x starts the task watchdog differently,
   UART mode runs the fallback or loses the watchdog. The boot log in UART
   mode must not say `task watchdog unavailable` (Task 4, step 7).
5. **A frame that does not fit the buffer.** Truncating it would send a
   different, valid temperature. The builder refuses instead, and the
   adapter sends nothing (Task 2: `test_pulse_frame_refuses_when_too_small`).

---

### Task 1: Capture comparison tool

**Files:**
- Create: `tools/pulse_capture/compare_capture.py`,
  `tools/pulse_capture/test_compare_capture.py`,
  `tools/pulse_capture/README.md`

**Interfaces:**
- Produces:
  - CLI `python tools/pulse_capture/compare_capture.py BASELINE.csv CANDIDATE.csv`
    compares two captures.
  - CLI `python tools/pulse_capture/compare_capture.py --codes 10,60 CAPTURE.csv`
    checks one capture whose counts may change.
  - Each prints one summary line per capture, then `PASS` or
    `FAIL: <reasons>`. The exit code is 0 or 1.
  - Library functions:
    - `load_capture(path) -> list[tuple[float, int]]`
    - `analyse(transitions) -> Analysis`. `Analysis` is a dataclass with these
      fields:
      - `counts: list[int]`;
      - `highs_us`, `lows_us`, `periods_ms`, `gaps_ms`, all `list[float]`.
    - `compare(baseline, candidate) -> list[str]` returns its reasons;
      empty means pass.
    - `check_codes(analysis, codes) -> list[str]`.

The input format is a Logic 2 export: a header row, then
`time_seconds,level` rows, one per transition. The column names are ignored.

Analysis rules:
- A frame is a run of HIGH pulses separated by LOW gaps of 50 ms or less. A
  LOW over 50 ms ends a frame.
- The first and last frames are dropped, since the capture may have cut
  them.
- Only the gaps between kept frames count. The leading and trailing segments
  are ignored.
- `periods_ms` runs from the first rising edge of one kept frame to the next
  one's.
- `lows_us` holds only the LOWs inside a frame.

Compare rules:
- Fewer than 3 kept frames in either capture fails.
- The candidate has the same set of counts as the baseline, and each set
  holds exactly one count.
- Every candidate HIGH and LOW is within ±10 µs of the baseline's mean of the
  same kind.
- Every candidate period is within ±5 ms of the baseline's mean period.
- Every candidate gap is at least 122 ms.

Every reason names the kept frame's index (`frame 4: HIGH 2011.0 us`), so a
reader can tell which frame failed.

`check_codes` rules:
- Every kept count is in `codes`.
- Every width is within ±10 µs of 2000.2 µs.
- Every gap is at least 122 ms.
- There is no period check: a forced frame comes early by design.

- [ ] **Step 1: Write the failing tests** in `test_compare_capture.py`.
  - Build captures with a helper:
    `make_capture(counts, high_us=2000.2, low_us=2000.2, period_ms=1000.1, gap_ms=None, lead_partial=0)`.
    It returns transitions and lets a test write a temporary CSV.
  - A given `gap_ms` sets the LOW between frames, and the period then
    follows from it.
  - `counts` lists every frame, including the first and last, which the
    analysis drops.
  - The tests:
    - `test_identical_passes`: `compare` of `make_capture([60]*6)` with itself
      is `[]`.
    - `test_width_within_tolerance_passes`: a candidate with
      `high_us=2009.0` passes.
    - `test_wide_pulse_fails`: `high_us=2011.0` fails with a reason
      containing `HIGH`.
    - `test_period_off_fails`: `period_ms=1006.2` fails with `period`.
    - `test_count_differs_fails`: a baseline of `[60]*6` against a candidate
      of `[59]*6` fails with `count`.
    - `test_short_gap_fails`: `gap_ms=100` fails with `gap`.
    - `test_partial_first_frame_dropped`: `lead_partial=3` (3 pulses before
      the first full frame) still passes against a clean baseline.
    - `test_too_few_frames_fails`: `[60]*3` keeps 1 frame, which fails.
    - `test_check_codes_switching_passes`: counts `[10,10,60,60,10,10]` with
      `codes={10,60}` give `[]`.
    - `test_check_codes_stray_pulse_fails`: counts `[10,1,10,10]` fail with
      `count 1`.
    - `test_cli_exit_codes`: the CLI on two written CSVs returns 0 for a
      match and 1 for a mismatch.
- [ ] **Step 2: Run the tests.** Run
  `python -m unittest discover -s tools/pulse_capture -p "test_*.py"`.
  Expected: an ImportError for `compare_capture`.
- [ ] **Step 3: Implement `compare_capture.py`** with the interfaces and
  rules above, using only the standard library.
- [ ] **Step 4: Run the tests, then the tool on the real baselines.**
  - The test command from Step 2 must pass.
  - Then run
    `python tools/pulse_capture/compare_capture.py "logic captures/2.0.17 ESP32-C3 Super Mini Pulse - Off.csv" "logic captures/2.0.17 ESP32-C3 Super Mini Pulse - Off.csv"`
    and the same for the 60 °C file.
  - Expected: PASS twice. The summaries show counts `{10}` and `{60}`, widths
    of about 2000.2 µs and a period of about 1000.1 ms.
- [ ] **Step 5: Write the README and commit.**
  - The README is a few lines: what to capture (GPIO3, at least 5 s, Logic
    2's "Export Raw Data" as CSV), the two commands and the tolerances.
  - Commit `feat(tools): compare logic-analyser pulse captures`.

---

### Task 2: A pure pulse-frame builder, used by the 2.0.17 adapter

**Files:**
- Create: `lib/remote-policy/src/PulseFrame.h`,
  `lib/remote-policy/src/PulseFrame.cpp`
- Modify: `test/test_remote_policy/test_main.cpp`,
  `src/controller/RmtOutputAdapter.h`, `src/controller/RmtOutputAdapter.cpp`

**Interfaces:**
- Produces, in namespace `iheaterlink::policy`:
  ```cpp
  struct PulseTiming { uint32_t syncLowUs; uint32_t pulseHighUs; uint32_t pulseLowUs; };
  struct PulseSymbol { uint16_t duration0; uint8_t level0; uint16_t duration1; uint8_t level1; };
  constexpr uint32_t kPulseFieldMaxUs = 32767;
  // Fills out[] with the frame's symbols (no end marker). Returns the count,
  // or 0 if the frame does not fit in capacity or a pulse width is 0 or over
  // kPulseFieldMaxUs.
  size_t buildPulseFrame(uint8_t pulseCode, const PulseTiming &timing,
                         PulseSymbol *out, size_t capacity);
  ```
- The SYNC split is the one the adapter uses today. Each symbol takes up to
  32767 µs into `duration0`, then up to 32767 µs into `duration1`, both at
  level 0, until the SYNC is used up.
- The builder adds one symbol `{pulseHighUs, 1, pulseLowUs, 0}` per count.

- [ ] **Step 1: Write the failing tests** in `test_main.cpp` and register
  them with `RUN_TEST`. The timing is `T{120000, 2000, 2000}`.
  - `test_pulse_frame_sync_split`: code 10 into a 128-slot buffer returns
    12.
    - `out[0]` is `{32767,0,32767,0}`.
    - `out[1]` is `{32767,0,21699,0}`.
  - `test_pulse_frame_counts`: for code 10, `out[2]` to `out[11]` are each
    `{2000,1,2000,0}`. Code 60 returns 62; code 80 returns 82.
  - `test_pulse_frame_no_zero_duration`: for codes 10, 45 and 80, no
    returned symbol has a zero `duration0` or `duration1`. The hardware reads
    a zero as the end of the frame.
  - `test_pulse_frame_refuses_when_too_small`: code 80 with capacity 81
    returns 0; with capacity 82 it returns 82.
  - `test_pulse_frame_refuses_bad_width`: `{120000, 0, 2000}` and
    `{120000, 32768, 2000}` both return 0.
- [ ] **Step 2: Run the native tests.** Run
  `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native > <file> 2>&1`
  and read the tail. Expected: a compile error, because `PulseFrame.h` is
  missing.
- [ ] **Step 3: Implement the builder, then switch the adapter to it.**
  - In `rebuildFrame`, call `buildPulseFrame` into a
    `policy::PulseSymbol frame_[MAX_ITEMS - 1]`.
  - Copy each symbol into `items_`, then append the `val = 0` end marker as
    today.
  - On a return of 0, set `itemCount_ = 0`, and have `sendFrame` skip the
    write.
  - Move the Russian comments on the 15-bit field and the SYNC split to the
    builder, in Canadian English.
  - Keep the legacy driver calls in this task.
- [ ] **Step 4: Run both `pio` gates.** Expected:
  - the C3 build succeeds;
  - every native test passes, including the five new ones.
- [ ] **Step 5: Run all five gates and commit**
  `refactor(firmware): build pulse frames in remote-policy`.

---

### Task 3: Switch to Arduino-ESP32 3.3.12

**Files:**
- Modify: `platformio.ini`, `src/controller/RmtOutputAdapter.h`,
  `src/controller/RmtOutputAdapter.cpp`, `src/standalone/UartLink.cpp`,
  `AGENTS.md`, and any source file a 3.x build error names.
- Maybe: `platformio.ini` `board_build.partitions` (see Step 5)

**Interfaces:**
- Consumes `buildPulseFrame` and `PulseSymbol` from Task 2.
- `RmtOutputConfig` drops `rmt_channel_t channel` and `memBlockNum`, and
  gains `size_t memBlockSymbols = 96`. That is two of the C3's 48-symbol
  blocks, as before. Nothing outside the adapter reads the removed fields;
  check with `git grep`.
- The public interface (`begin`, `apply`, `getLastCommand`,
  `getLastPulseCode`, `isEnabled`, `forceFrame`, `getConfig`) is unchanged.

- [ ] **Step 1: Pin the platform** URL from the Global Constraints in
  `platformio.ini`, then build. Run
  `pio run -e esp32c3-super-mini-remote > <file> 2>&1`. Expected: it fails.
  Record each distinct error, such as the legacy RMT header and the
  `esp_task_wdt_init` signature, in the task report.
- [ ] **Step 2: Port `RmtOutputAdapter` to `driver/rmt_tx.h`.**
  - Set up the channel:
    - `rmt_tx_channel_config_t` with `clk_src = RMT_CLK_SRC_DEFAULT`,
      `resolution_hz = 1000000`, `mem_block_symbols = memBlockSymbols` and
      `trans_queue_depth = 1`;
    - a copy encoder;
    - `rmt_enable`.
  - Each frame is `rmt_transmit` with `loop_count = 0` and
    `flags.eot_level = 0`.
  - `items_` becomes `rmt_symbol_word_t symbols_[MAX_ITEMS]`, filled from
    `buildPulseFrame`, with no end marker.
  - `sendFrame` calls `rmt_tx_wait_all_done(channel_, pdMS_TO_TICKS(1000))`
    before it rebuilds or transmits. The copy encoder then never reads a
    buffer being rewritten, and a forced frame never cuts into the current
    one.
  - If any setup call fails, print
    `[RMT] <call> failed (err=%d)` once and leave `initialized_` false.
  - The task, its stack and priority, the period and `forceFrame` are
    unchanged.
- [ ] **Step 3: Fix the watchdog fallback** in `UartLink::registerWatchdog`.
  - Pass `esp_task_wdt_config_t{ .timeout_ms = kFallbackWdtSeconds * 1000, .idle_core_mask = 0, .trigger_panic = true }`.
  - Update the comment above it to say what the 3.x build sets.
- [ ] **Step 4: Fix any remaining build errors.** Make the smallest change
  that keeps the 2.0.17 behaviour. List each fix in the report, with its
  error line.
- [ ] **Step 5: Check the size.** If the build reports a flash overflow, or
  an app over 0x140000 bytes:
  - add `board_build.partitions = min_spiffs.csv`;
  - say so in the report (the bench then flashes over USB and re-uploads
    LittleFS).

  Otherwise leave the partitions alone. Either way, record the build's
  Flash and RAM lines.
- [ ] **Step 6: Prove the legacy driver is gone.**
  - `git grep -n "driver/rmt.h"` finds nothing.
  - `"$HOME/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm" .pio/build/esp32c3-super-mini-remote/firmware.elf | grep -cE " (rmt_driver_install|rmt_config)$"`
    prints 0.
  - The build log has no `deprecated` warning about RMT or the task
    watchdog.
- [ ] **Step 7: Update the docs.** In `AGENTS.md`, line 4 becomes
  `Arduino-ESP32 3.3.12 via pioarduino 55.03.312-1`, and the gate section
  notes that the first build downloads the platform.
  `git grep -n "2\.0\.17" -- . ':!docs/design'` finds nothing else.
- [ ] **Step 8: Run all five gates and commit**
  `feat(firmware)!: move to Arduino-ESP32 3.3.12`. The body names the RMT
  port and says whether the partitions changed.

---

### Task 4: Bench check on COM23 (the controller and the user)

This task has no code unless a check fails. A failure goes to
systematic-debugging, and its fix is committed with a test where one is
possible.

- [ ] **Step 1: Record the starting state.** On the running 2.0.17 board,
  note what the setup page shows: the Wi-Fi network, the control source, the
  Moonraker host and the pairing token.
- [ ] **Step 2: Install 3.x.** Say before each upload.
  - If Task 3 left the partitions alone, upload the 3.x `firmware.bin`
    through the board's own `/fw` page. That is OTA from 2.0.17, the
    printer's path.
  - Otherwise, upload over USB:
    `pio run -e esp32c3-super-mini-remote -t upload --upload-port COM23`,
    then `-t uploadfs`.
- [ ] **Step 3: Check the boot log.** Watch the serial log on COM23 for
  30 s. Expected:
  - no `CONFLICT! driver_ng`;
  - no `[RMT] ... failed`;
  - no reset loop;
  - the board rejoins Wi-Fi.
- [ ] **Step 4: Check the settings and pages.** The setup page shows the same
  values as Step 1, and `/`, `/setup`, `/fw` and the logo all load.
- [ ] **Step 5: The user takes three captures** on GPIO3 into
  `logic captures/`:
  - `3.3.12 ESP32-C3 Super Mini Pulse - Off.csv`;
  - `3.3.12 ESP32-C3 Super Mini Pulse - 60 degrees C.csv`;
  - `3.3.12 ESP32-C3 Super Mini Pulse - switching.csv`. Over about 15 s,
    switch Off and 60 °C a few times.

  Then:
  - run `compare_capture.py` with each 2.0.17 baseline against its 3.3.12
    capture. Expected: PASS twice.
  - run `compare_capture.py --codes 10,60` on the switching capture.
    Expected: PASS.

  Last, the user takes `3.3.12 ESP32-C3 Super Mini Pulse - reset.csv`:
  about 10 s at 60 °C, pressing the reset button once. Run
  `compare_capture.py --codes 60` on it. Expected:
  - every frame after the reset passes;
  - the only reasons allowed name the one frame the reset cut;
  - there is no frame with a count of 1 or 2 near the reset (a glitch at
    boot).
- [ ] **Step 6: Pair on elpis.**
  - Install the Klipper and Moonraker parts on elpis with `./install.sh`, and
    add a `[heater_iheater_remote chamber]` section with the C3's token.
  - Point the C3 at elpis's Moonraker.
  - Expected:
    - after `SET_HEATER_TEMPERATURE HEATER=chamber TARGET=60`, the C3 shows
      target 60 and pulse code 60;
    - reports arrive, and
      `printer["heater_iheater_remote chamber"].iheater_state` is set.
- [ ] **Step 7: Check UART mode.** Switch the output to UART and watch for
  2 min. Expected:
  - the link shows `no-reply`;
  - there is no reset;
  - there is no `task watchdog unavailable` line.

  Then switch back to Pulse.
- [ ] **Step 8: Test OTA again.** Upload the same 3.x build through `/fw`.
  Expected: the board reboots into it, with its settings intact.
- [ ] **Step 9: Record the results.** Put each step's result in the ledger.
  If any fix was needed, run the five gates and commit it.
