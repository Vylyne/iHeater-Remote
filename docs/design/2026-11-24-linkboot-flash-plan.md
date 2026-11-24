# iHeater update through the link bootloader — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Remote flashes a `build_type` 2 iHeater app image through the
iHeater's Modbus link bootloader, and drops the STM32 ROM path.

**Architecture:** `FlashJob` stays a pure, synchronous sequencer in
`lib/remote-policy`, but it now speaks only Modbus RTU frames through a
smaller `FlashLink` (one `exchange` call with a per-frame timeout). The UART
task runs it in place of polling, as today. The poll policy learns to
recognise the bootloader (input 0x00 = `0x8001`) and holds all writes while
it's there.

**Tech Stack:** C++17, PlatformIO, Arduino-ESP32 3.3.12 (pioarduino
55.03.312-1), Unity native tests under MinGW.

**Spec:** `docs/design/2026-11-24-linkboot-flash-design.md` (approved
2026-11-24). The contract it builds on is iHeater-Standalone-Firmware
`docs/design/2026-11-24-link-bootloader-design.md`, and the reference flasher
is that repo's `tools/linkboot_flash.py`. Read both; never edit that repo.

## Global Constraints

- All five gates pass before every commit (AGENTS.md "Gate commands"). Run
  `pio run` from PowerShell with no `MSYSTEM` (for example
  `powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$TEMP/pio-nomsys.ps1")" run -e esp32c3-super-mini-remote`).
- Canadian English in comments, page text and docs. LF line endings.
- Commit messages end with a blank line, then
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Never flash or upload to a device. No bench work in this plan.
- No change to `klipper/`, `moonraker/`, `scripts/` or `lib/modbus-rtu/`.
- Contract values, verbatim: slave 1, 115200 8N1; input 0x00 reads `0x8001`
  in the bootloader and 1 in the app; bootloader inputs 0x01 `bl_version`,
  0x02 `bl_state`, 0x03 `why` (1 asked, 2 no valid app), 0x04 `app_check`,
  0x05 `last_error`, 0x06 `timeout_s`, 0x07 `chunks_written`; app input 0x16
  is the bootloader version (refused or 0 on old firmware); 0x0102 ← `0xB007`
  enters; 0x0300 ← 1 erases pages 2–29, ← 2 boots (exception 0x04 when the app
  is invalid); chunks are FC10 to 0x0310, exactly 65 registers (index, then 64
  big-endian data registers, first byte high), index n at offset 128·n from
  0x08000800, n ≤ 223; `last_error` 3 = not blank.
- Job values, verbatim: 5 tries per exchange with the identical frame; 2000 ms
  reply wait for erase and boot; 100 ms entry and app poll period; 3000 ms
  entry wait; 5000 ms app wait; 1 restart shared between not-blank and
  boot-refused; 128-byte chunks padded with 0xFF; image at most 28,672 B.
- Never send 0xB007 unless input 0x16 read non-zero in this job. Never send a
  chunk index above 223. Never send a byte that isn't a whole Modbus frame to
  slave 1 with FC 0x03, 0x04, 0x06 or 0x10.
- Never log or return the login verifier, a proof or a session token (no auth
  code is touched here, but keep it so).

## Review Focus

1. **A resend that lands in a different state.** 0xB007 lands, its reply is
   lost, and the resend reaches the bootloader (exception 0x02); a boot
   command lands and the resend reaches the app. Both must carry on, not
   stop. Tests: `test_entry_resend_reaches_bootloader`,
   `test_boot_resend_lands_in_app` (Task 4).
2. **The normal poll while the iHeater sits in the bootloader.** Zeros in
   inputs 0x08–0x15 must not be shown as temperatures, sent to Klipper as
   readings, or trigger a target, clear or settings write. Tests:
   `test_bootloader_holds_writes`, `test_bootloader_drops_snapshot` (Task 3).
3. **An old image or an old iHeater.** A `build_type` 1 file is refused with
   its type named; an app without the bootloader never gets 0xB007. Tests:
   `test_rejects_rom_build_naming_type` (Task 1),
   `test_old_firmware_refused_0x16`, `test_old_firmware_zero_0x16` (Task 4).
4. **Cancel after the board entered the bootloader.** Nothing is erased; the
   page must not say the app is gone, and cancel must stop before the erase.
   Test: `test_cancel_after_entry_before_erase` (Task 4).
5. **The largest image.** 28,672 B writes index 223 last and nothing above;
   28,673 B is refused before any frame. Tests:
   `test_largest_image_ends_at_chunk_223`, `test_oversize_sends_nothing`
   (Task 4), `test_max_image_bytes` (Task 1).

---

### Task 1: Image check for the app build

**Files:**
- Modify: `lib/remote-policy/src/FirmwareImage.h`, `lib/remote-policy/src/FirmwareImage.cpp`
- Modify: `test/image_fixture.h`, `test/test_firmware_image/test_main.cpp`
- Modify: `src/standalone/StandaloneApp.cpp` (only the `fallback` and
  `hasMarker` uses, so the build stays green)

**Interfaces:**
- Produces:
  - `kAppBase = 0x08000800`, `kMaxImageBytes = 28672`,
    `kResetVectorEnd = 0x08007800` (exclusive), `kBuildTypeApp = 2`
    (replaces `kBuildTypeUart`).
  - `enum class ImageCheck { Ok, TooSmall, TooLarge, BadStackPointer,
    BadResetVector, NoRecord, BadRecord, WrongBuildType, LengthMismatch,
    BadChecksum, UnsupportedProtocol }` (`NotUartBuild` and `NotUartVectors`
    go).
  - `ImageInfo` gains `uint16_t buildType`; loses `hasMarker`.
  - `size_t imageCheckDetail(const ImageInfo &info, char *out, size_t cap)`:
    the message, with the type named for `WrongBuildType`, e.g. "This is a
    build_type 1 image (the old ROM build). The link bootloader takes
    build_type 2 (the app build)."
  - `imageSummary` drops the fallback line; for a refusal it writes
    `imageCheckDetail`.

- [ ] **Step 1: Update the fixture.** `makeImage(size)` writes reset vector
  `0x08000901`, other vectors in `0x080008xx`–`0x08006xxx`, and build type 2.
  Delete `fallbackImage` and its tests.
- [ ] **Step 2: Write the failing tests** in `test_firmware_image`:
  - `test_accepts_app_build`: `makeImage(1024)` → `Ok`, `buildType == 2`.
  - `test_rejects_rom_build_naming_type`: build type 1 (CRC refreshed) →
    `WrongBuildType`; `imageCheckDetail` output contains "build_type 1".
  - `test_rejects_bootloader_build`: build type 3 → `WrongBuildType`.
  - `test_no_record_refused`: magic cleared → `NoRecord`.
  - `test_reset_vector_below_app_base`: `0x08000101` → `BadResetVector`.
  - `test_max_image_bytes`: 28,672 B → `Ok`; 28,673 B → `TooLarge`.
- [ ] **Step 3: Run and watch them fail.**
  `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native -f test_firmware_image`
  Expected: compile errors or FAIL on the new names and values.
- [ ] **Step 4: Implement.** Order of checks: size, SP, reset vector in
  `[kAppBase, kResetVectorEnd)`, record present (else `NoRecord`), version and
  size, build type, length, CRC, protocol. No vector-table fallback.
- [ ] **Step 5: Fix `StandaloneApp.cpp`.** Remove `image["fallback"]` and
  `doc["fallback"]`, and the `im.fallback` use in the card script; where an
  upload is refused, send `imageCheckDetail`. The card's file hint becomes
  "Choose the iHeater app build .bin (build_type 2)."
- [ ] **Step 6: Run all five gates.** Expected: all pass.
- [ ] **Step 7: Commit** `feat(image): accept only build_type 2 app images`.

### Task 2: FC10 frames

**Files:**
- Modify: `lib/remote-policy/src/UartFrames.h`, `lib/remote-policy/src/UartFrames.cpp`
- Test: `test/test_uart_link/test_main.cpp`

**Interfaces:**
- Produces:
  - `constexpr uint8_t kFcWriteMultiple = 0x10;`
  - `constexpr uint8_t kMaxWriteRegs = 65;`
  - `constexpr size_t kMaxFrameLen = 9 + 2 * kMaxWriteRegs;` (139)
  - `size_t buildWriteMultiple(uint16_t addr, const uint16_t *values, uint8_t count, uint8_t *out, size_t cap);`
    returns `9 + 2·count`, or 0 when count is 0, over `kMaxWriteRegs`, or
    over `cap`.
  - `parseReply(req, buf, len)` keeps its signature. For an FC10 request
    (pass the frame's first 8 bytes), an Ok reply is 8 bytes whose first 6
    echo the request's address, FC, start and count, with a good CRC.

- [ ] **Step 1: Write the failing tests:**
  - `test_build_write_multiple_layout`: 65 values to 0x0310 → length 139;
    bytes 0–6 are `01 10 03 10 00 41 82`; values big-endian; CRC matches
    `crc16` of bytes 0–136, low byte first.
  - `test_build_write_multiple_rejects`: count 0, count 66, cap 138 → 0.
  - `test_parse_write_multiple_ok`: echo reply → `Ok`, `count == 0`.
  - `test_parse_write_multiple_wrong_count`: echo with count 0x40 → `Invalid`.
  - `test_parse_write_multiple_exception`: `01 90 04 crc` → `Exception`, 4.
- [ ] **Step 2: Run and watch them fail.**
  `pio test -e native -f test_uart_link` — Expected: compile errors.
- [ ] **Step 3: Implement** `buildWriteMultiple` and the FC10 branch of
  `parseReply`.
- [ ] **Step 4: Run all five gates.** Expected: all pass.
- [ ] **Step 5: Commit** `feat(frames): add FC10 write-multiple`.

### Task 3: The poll policy sees the bootloader

**Files:**
- Modify: `lib/remote-policy/src/UartRegisters.h`,
  `lib/remote-policy/src/UartPollPolicy.h`, `lib/remote-policy/src/UartPollPolicy.cpp`
- Test: `test/test_uart_link/test_main.cpp`

**Interfaces:**
- Produces (`UartRegisters.h`):
  - `kBootloaderProtocol = 0x8001`, `kRegBootloaderVersion = 0x16`,
    `kRegCommand = 0x0300`, `kRegChunk = 0x0310`, `kCmdErase = 1`,
    `kCmdBoot = 2`, `kChunkBytes = 128`, `kMaxChunkIndex = 223`,
    `kLbErrNotBlank = 3`, `kExcDeviceFailure = 4`,
    `kBlWhy = 0x03`, `kBlAppCheck = 0x04`, `kBlLastError = 0x05`,
    `kBlTimeoutS = 0x06`.
  - `struct BootloaderInfo { uint16_t version, state, why, appCheck, lastError, timeoutS, chunksWritten; };`
- Produces (`UartPollPolicy`):
  - `bool inBootloader() const;` true while the last Ok input read had
    0x00 = `0x8001`.
  - `const BootloaderInfo &bootloader() const;` (inputs 0x01–0x07).
  - `link(now)` returns `Updating` while `inBootloader()` and Ok replies are
    recent; with no reply it ages to `NoReply` as today.
  - On a bootloader read: `hasSnapshot()` turns false, and `followUp()`
    returns `None`. An app read (0x00 = 1) clears `inBootloader()`.

- [ ] **Step 1: Write the failing tests:**
  - `test_bootloader_detected`: an Ok 22-register reply with regs[0] =
    `0x8001`, regs[3] = 2, regs[4] = 4, regs[6] = 30 → `inBootloader()`,
    `bootloader().why == 2`, `.appCheck == 4`, `.timeoutS == 30`,
    `link(now) == Updating`.
  - `test_bootloader_drops_snapshot`: app read, then bootloader read →
    `hasSnapshot()` false, `protocolSupported()` false.
  - `test_bootloader_holds_writes`: desired target 60 °C set, settings unread,
    then bootloader read → `followUp(now).kind == None`.
  - `test_bootloader_left_on_app_read`: bootloader read, then app read →
    `inBootloader()` false, `link == Up`.
  - `test_bootloader_silent_ages_to_no_reply`: bootloader read, then
    no replies for `kNoReplyAfterMs` → `NoReply`.
- [ ] **Step 2: Run and watch them fail.** `pio test -e native -f test_uart_link`
- [ ] **Step 3: Implement** in `onReply` for `ReadInputs`, before
  `decodeInputs`.
- [ ] **Step 4: Run all five gates.** Expected: all pass.
- [ ] **Step 5: Commit** `feat(link): recognise the iHeater's link bootloader`.

### Task 4: The flash job over Modbus

The core of the plan. It replaces the ROM job, its tests and its firmware
glue in one commit, because the firmware doesn't build with half of it.

**Files:**
- Rewrite: `lib/remote-policy/src/FlashJob.h`, `lib/remote-policy/src/FlashJob.cpp`
- Delete: `lib/remote-policy/src/RomProtocol.h`, `lib/remote-policy/src/RomProtocol.cpp`
- Rewrite: `test/test_flash_job/FakeIheater.h` (as a fake app plus link
  bootloader), `test/test_flash_job/test_main.cpp`
- Modify: `src/standalone/UartLink.h`, `src/standalone/UartLink.cpp`
- Modify: `src/standalone/StandaloneApp.cpp` (compile fixes only: start
  decision, `romDetected`, `PowerCycleNeeded`; the page is Task 5)

**Interfaces:**
- Consumes: Task 1 `checkImage`, `kMaxImageBytes`; Task 2
  `buildWriteMultiple`, `kMaxFrameLen`, `parseReply`; Task 3 constants,
  `BootloaderInfo`, `inBootloader()`, `bootloader()`.
- Produces (`FlashJob.h`):
  - `enum class FlashPhase { Idle, Cooling, Entering, Erasing, Writing, Starting, Restarting, Finished };`
  - `enum class FlashOutcome { None, Updated, ImageRejected, NoReply, UnexpectedState, NoLinkBootloader, Cancelled, HeatRequested, CoolTimeout, BadReading, NoReplyCooling, EntryRefused, EntryNotConfirmed, EraseRefused, WriteRefused, BootRefused, NoAppAfterUpdate };`
    Names for `flashOutcomeName`: `none`, `updated`, `image-rejected`,
    `no-reply`, `unexpected-state`, `no-link-bootloader`, `cancelled`,
    `heat-requested`, `cool-timeout`, `bad-reading`, `no-reply-cooling`,
    `entry-refused`, `entry-not-confirmed`, `erase-refused`, `write-refused`,
    `boot-refused`, `no-app-after-update`.
  - Constants: `kFlashTries = 5`, `kJobReplyMs = 500`, `kLongReplyMs = 2000`,
    `kEntryPollMs = 100`, `kEntryWaitMs = 3000`, `kAppWaitMs = 5000`,
    `kRestarts = 1`; cooling constants unchanged (`kCoolPollMs`,
    `kCoolTimeoutMs`, `kSafeHeaterT10`, `kCoolMissLimit`).
  - `FlashStatus { phase, outcome, failedPhase, progressPermille, heaterT10, entryUnconfirmed, uint16_t retries, lastError, appCheck; char detail[48]; }`
    (`extendedErase`, `unexpectedPid`, `pid` go). `retries` counts resent
    frames; `progressPermille` is chunks written over total.
  - ```cpp
    class FlashLink {
     public:
      virtual ~FlashLink() = default;
      // Sends one whole frame, then judges what comes back with
      // parseReply(frame, ...). False: no complete reply within timeoutMs.
      virtual bool exchange(const uint8_t *frame, size_t len, Reply &reply, uint32_t timeoutMs) = 0;
      virtual void sleepMs(uint32_t ms) = 0;
      virtual uint32_t nowMs() = 0;
      virtual bool heatWanted() = 0;
      virtual bool cancelRequested() = 0;
      virtual void publish(const FlashStatus &status) = 0;
    };
    ```
  - `FlashStatus runFlashJob(FlashLink &link, const uint8_t *image, size_t len);`
    (no start kind: input 0x00 decides).
  - `FlashStartInputs { uartOutput, imageOk, heatWanted, jobRunning, restartPending; LinkStatus link; bool inBootloader; uint16_t bootloaderWhy; FlashOutcome lastOutcome; }`
  - `enum class FlashRefusal { None, NotUart, NoImage, HeatWanted, JobRunning, LinkDown, BootloaderLeaving, NothingToRetry, RestartPending };`
    `BootloaderLeaving`: "The iHeater is in its bootloader and returns to its
    firmware by itself within 30 s. Try again then."
  - `FlashRefusal decideFlashStart(const FlashStartInputs &in, bool retry);`
    Same order as today. A normal start needs `link == Up`, or
    `inBootloader && bootloaderWhy == 2`; `inBootloader && why == 1` gives
    `BootloaderLeaving`. A retry ignores the link, as now.
  - `bool retryAllowed(FlashOutcome)`: false for `None`, `Updated`,
    `Cancelled`, `HeatRequested`, `ImageRejected`, `NoLinkBootloader`.
  - `bool applicationIntact(const FlashStatus &s)`: true when `outcome` is
    `Updated`, or `failedPhase` is `Idle`, `Cooling` or `Entering`.
  - `probeRom`, `romProbeDue`, `LinkParity`, `FlashStart` go.
- Produces (`UartLink`):
  - `bool exchangeFrame(const uint8_t *frame, size_t len, policy::Reply &reply, uint32_t timeoutMs);`
    the old `exchange` body with the timeout as a parameter; the poll path
    builds its frame and calls it with `kReplyTimeoutMs`.
  - `FlashPort` implements the new `FlashLink`; `exchange` feeds the watchdog
    after each call; a 2000 ms wait is fed in slices as `sleepMs` does.
  - `bool startFlash(const uint8_t *image, size_t len);`
  - `void cancelFlash();` takes effect until the erase.
  - `UartLinkView`: `romDetected` goes; adds `bool inBootloader;
    policy::BootloaderInfo boot;`.
  - The ROM probe (step 3b), `lastProbeMs_` and `romDetected_` go.

The fake (`FakeIheater.h`), one class implementing `FlashLink`:
- State: `App` or `Bootloader`, flash pages 2–29 as bytes (0xFF after erase),
  `why`, `appCheck`, `lastError`, `bootloaderVersion` (0 = old firmware,
  `refuse16` = answers 0x16 with exception 0x02), heater and air readings, a
  fake clock advanced by `sleepMs` and by each unanswered `exchange`.
- Checks every frame: slave 1, good CRC, FC in {03, 04, 06, 10}, FC10 only to
  0x0310 with 65 registers and index ≤ 223. Any breach sets `violation` with
  a description; every test ends with `TEST_ASSERT_FALSE(fake.violation)`.
- Logs every frame (bytes, timeout) for assertions.
- Fault injection by frame kind and occurrence: `dropRequest` (not acted on,
  no reply), `dropReply` (acted on, no reply), `damageReply` (acted on,
  reply with a bad CRC → `Invalid`).
- Behaviour: the app answers 0x0102 ← 0xB007 by switching to `Bootloader`
  with `why` 1 (if `bootloaderVersion` ≠ 0) and refuses it with exception 4
  when the heater is hot or the target isn't 0. The bootloader answers 0x0102
  with exception 2, an erase by blanking pages 2–29, a chunk by writing it
  (accepting identical data already there; `last_error` 3 and exception 4 for
  other data), a boot by checking a scripted `bootOk` list then switching to
  `App`. A chunk to a non-blank page can be scripted with `notBlankOnce`.

- [ ] **Step 1: Write the fake and the failing tests.** Each test asserts its
  outcome, `failedPhase`, and `fake.violation == false`:
  - `test_updates_from_app`: `Updated`; flash bytes equal the image padded with
    0xFF; one 0xB007; phases published in the order Cooling, Entering,
    Erasing, Writing, Starting, Restarting, Finished; `progressPermille`
    1000 last; `retries == 0`.
  - `test_already_in_bootloader_skips_entry`: board in `Bootloader` with
    `why` 2 → `Updated`, no 0xB007, no write to 0x0100.
  - `test_old_firmware_refused_0x16`, `test_old_firmware_zero_0x16`:
    `NoLinkBootloader`, no 0xB007, no write to 0x0100, `applicationIntact`.
  - `test_unexpected_state`: 0x00 reads 2 → `UnexpectedState`.
  - `test_entry_refused_on_first_try`: `EntryRefused`, intact.
  - `test_entry_resend_reaches_bootloader`: first 0xB007 `dropReply` →
    `Updated`, two 0xB007 frames, identical bytes, `retries == 1`.
  - `test_entry_request_lost_then_lands`: first 0xB007 `dropRequest` →
    `Updated`.
  - `test_entry_all_unanswered_but_entered`: every 0xB007 `dropReply` →
    `Updated`, `entryUnconfirmed`, five 0xB007 frames.
  - `test_entry_not_confirmed`: every 0xB007 `dropRequest` →
    `EntryNotConfirmed` after 3000 ms of 100 ms polls, intact.
  - `test_cancel_after_entry_before_erase`: cancel raised once the board is
    in `Bootloader` → `Cancelled`, no erase frame, intact.
  - `test_erase_waits_2s` and `test_boot_waits_2s`: those frames carry
    timeout 2000; every other frame `kJobReplyMs`.
  - `test_erase_reply_lost_resent`: `Updated`, two erase frames.
  - `test_erase_refused_reports_last_error`: erase exception 4 with
    `last_error` 2 → `EraseRefused`, `lastError == 2`, detail names it.
  - `test_chunk_reply_lost_resent`, `test_chunk_reply_damaged_resent`:
    `Updated`; that chunk's frame sent twice, identical.
  - `test_not_blank_restarts_once`: `Updated`, two erase frames.
  - `test_not_blank_twice_stops`: `WriteRefused`, `lastError == 3`.
  - `test_chunk_other_exception_stops`: exception 2 → `WriteRefused`.
  - `test_boot_refused_restarts_once`: `bootOk = {false, true}` → `Updated`,
    two erases, `appCheck` read once.
  - `test_boot_refused_twice_stops`: `BootRefused`, `appCheck` reported.
  - `test_restart_shared`: not blank once, then boot refused → `BootRefused`.
  - `test_boot_reply_lost_polls_for_app`: `Updated`.
  - `test_boot_resend_lands_in_app`: first boot `dropReply`; the app answers
    the resend with exception 2 → `Updated`.
  - `test_app_never_answers`: app stays silent → `NoAppAfterUpdate` after
    5000 ms.
  - `test_no_reply_gives_up_after_5`: chunk 3 always `dropRequest` →
    `NoReply`, `failedPhase == Writing`, exactly 5 frames for chunk 3,
    detail "chunk 3: no reply".
  - `test_largest_image_ends_at_chunk_223`: 28,672 B → `Updated`, last chunk
    index 223.
  - `test_oversize_sends_nothing`, `test_rom_build_sends_nothing`: 28,673 B,
    and a build type 1 image → `ImageRejected`, zero frames.
  - Cooling, kept from today and rewritten over the fake:
    `test_cooling_writes_target_only_when_nonzero`,
    `test_cooling_waits_until_50C`, `test_cooling_times_out_after_30_min`,
    `test_cancel_during_cooling`, `test_heat_requested_during_cooling`,
    `test_heat_requested_just_before_entry`,
    `test_invalid_heater_reading_stops`,
    `test_cooling_stops_after_three_missed_reads`.
  - Tables and start rules: `test_outcome_tables` (every outcome has a name
    and message), `test_decide_flash_start` (adds bootloader `why` 2 allowed,
    `why` 1 → `BootloaderLeaving`), `test_restart_pending_refuses_every_start`,
    `test_retry_allowed`, `test_application_intact`.
- [ ] **Step 2: Run and watch them fail.** `pio test -e native -f test_flash_job`
  Expected: compile errors against the old `FlashJob.h`.
- [ ] **Step 3: Implement the job** in `FlashJob.cpp`, following
  `linkboot_flash.py`'s flow and the spec's steps 1–7. One helper does the
  tries: build the frame once, `exchange` it up to `kFlashTries` times while
  the result is false or `Invalid`, count each resend in `retries`, and
  return the last reply. A Modbus exception is never retried. Entry and app
  polls are single reads every `kEntryPollMs`. Detail strings fit 48 bytes:
  "chunk 12: no reply", "erase refused: last_error 2",
  "boot refused: app_check 4", "input 0x00 reads 0x0002".
- [ ] **Step 4: Run `test_flash_job`.** Expected: all pass.
- [ ] **Step 5: Firmware glue.** `UartLink` as in Interfaces. In
  `StandaloneApp.cpp`: `ihStartDecision` fills `inBootloader` and
  `bootloaderWhy` and returns a `FlashRefusal`; `handleIheaterStart` calls
  `startFlash(image, len)`; the housekeeping frees the image on `Updated`
  only; `handleIheaterCancel` allows `Idle`, `Cooling` and `Entering`;
  `canCancel` matches. Delete `RomProtocol.*`.
- [ ] **Step 6: Run all five gates.** Expected: all pass. Then
  `grep -rn "RomProtocol\|probeRom\|LinkParity\|romDetected\|8E1" lib src test`
  Expected: no matches.
- [ ] **Step 7: Commit** `feat(flash): update the iHeater through its link bootloader`
  with a body naming the ROM path's removal.

### Task 5: Page, JSON, log and README

**Files:**
- Modify: `src/standalone/StandaloneApp.cpp` (card HTML and script,
  `fillUpdateJson`, `logUartFlash`)
- Modify: `README.md` ("Updating the iHeater's firmware")

**Interfaces:**
- Consumes: Task 4 `FlashStatus`, `applicationIntact`, `UartLinkView.inBootloader`
  and `.boot`, `flashRefusalMessage`.
- Produces: `iheaterUpdate` JSON — removes `romDetected`, `unexpectedPid`,
  `pid`; adds `retries`, `lastError`, `appCheck`, `appIntact` (bool), and
  `bootloader` (an object `{why, appCheck, timeoutS}` while `inBootloader`,
  else absent).

- [ ] **Step 1: JSON.** As above. `canCancel` is true while busy and the
  phase is `idle`, `cooling` or `entering`.
- [ ] **Step 2: Card.** Replace `ihRom` with `ihBoot`: for `why` 2, "The
  iHeater is in its bootloader with no valid firmware. Upload the app build
  and press Update iHeater."; for `why` 1, "The iHeater is in its bootloader
  and returns to its firmware within <timeoutS> s." Replace `ihErase` with a
  warning shown while running in `erasing`, `writing` or `starting`: "The
  iHeater's firmware is being replaced. Keep the Remote and the iHeater
  powered." After a failure with `appIntact` false: "The iHeater is in its
  bootloader with the heater and fan off, waiting for firmware. Press Retry."
  Drop `syncing` from the busy phases.
- [ ] **Step 3: Log.** `logUartFlash` prints
  `[UART] flash outcome=<name> phase=<failedPhase> retries=<n> <detail>`;
  never image bytes.
- [ ] **Step 4: README.** Rewrite steps 1–4 and the recovery paragraphs for
  the link bootloader: `build_type` 2 only, 28,672 B, Cancel until the erase,
  Retry reruns the whole job, a board left in its bootloader shows on the
  page and is updated from there. The first install of the bootloader is
  `link_flash.py --enter` from a PC (or an ST-Link), not the Remote. Remove
  the power-cycle paragraph.
- [ ] **Step 5: Run all five gates.** Expected: all pass.
- [ ] **Step 6: Commit** `feat(web): show the link bootloader on the iHeater card`.

### Task 6: Rules and superseded docs

**Files:**
- Modify: `AGENTS.md` (the "iHeater flashing" rule)
- Modify: `docs/design/2026-11-22-iheater-link-flash-design.md`, and its
  plan in `docs/design/` (status line only)
- Modify: `docs/design/2026-11-24-linkboot-flash-design.md` (status:
  approved 2026-11-24)
- Memory (outside the repo): `link-flash-rom-unreachable.md` and its
  `MEMORY.md` line

- [ ] **Step 1: AGENTS.md.** Replace the flashing rule with: "iHeater
  flashing: only through the link bootloader. Never send ROM (AN3155)
  commands or switch the link from 8N1. Send `0xB007` only after input 0x16
  reads non-zero, and resend it only when a try got no usable reply, up to 5
  tries. Never send a chunk index above 223."
- [ ] **Step 2: Status lines.** The 2026-11-22 design and plan get "Status:
  superseded for the update path by `2026-11-24-linkboot-flash-design.md`."
- [ ] **Step 3: Memory.** Record that the Remote updates through the link
  bootloader (this branch), the ROM path is gone, and `fix/flash-retries`
  stays unmerged.
- [ ] **Step 4: Run all five gates.** Expected: all pass.
- [ ] **Step 5: Commit** `docs: flash through the link bootloader only`.

## Self-review

- Spec coverage: steps 1–7 → Task 4; link while not flashing → Task 3 and
  Task 4's start rules; web UI → Tasks 1 and 5; AGENTS.md and superseded docs
  → Task 6; every listed test case → Task 4 (job), Task 1 (image), Task 3
  (poll policy).
- Left to the builder, as the spec allows: outcome and phase names (fixed
  above), FC10 through `FlashLink` (a whole-frame `exchange`), JSON (new
  fields, above).
- `retries` comes back with a new meaning (resent frames); it was only on the
  unmerged branch.
