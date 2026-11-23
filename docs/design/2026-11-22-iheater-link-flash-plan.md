# iHeater firmware update over the link — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upload an iHeater `.bin` on the Remote's `/fw` page, check it, and
flash it into the iHeater over the UART link: cool down, enter the ROM
bootloader, erase, write and verify, start it, and confirm Modbus answers.

**Architecture:** Three pure units in `lib/remote-policy` carry every
decision. `FirmwareImage` checks the file. `RomProtocol` builds AN3155 frames.
`FlashJob` sequences a whole job against an abstract `FlashLink`. The UART
task implements `FlashLink` on UART1 and runs the job in place of polling, so
UART1 keeps one owner. `StandaloneApp` holds the uploaded image and adds the
endpoints and the `/fw` card.

The design's approach A says "a state machine shaped like `UartPollPolicy`".
This plan uses a synchronous sequencer that calls an injected link instead.
It is still pure, and native tests still run whole jobs against a fake
iHeater. It needs no hand-written step/outcome protocol, because the UART
task blocks for the job anyway.

**Tech Stack:** C++17 (Arduino-ESP32 3.3.12, ESP-IDF UART driver, FreeRTOS),
Unity native tests, Python `unittest`.

**Spec:** `docs/design/2026-11-22-iheater-link-flash-design.md`, approved
2026-11-23. The iHeater-side contracts are in
`C:\git\github\iHeater-Standalone-Firmware\docs\design\2026-11-22-bootloader-entry-design.md`
("Flasher contract") and `...\2026-11-22-build-marker-design.md`. The
reference flasher is `...\tools\link_flash.py`. Read them; never edit that
repo.

## Global Constraints

- `0xB007` is written to register `0x0102` at most once per job, and only
  after every cool-down check has passed. It is never retried.
- Erase only pages 0..N−1, with N = ceil(size / 1024). Never above page 29.
  Never mass erase, write-protect, read-protect or option-byte commands.
- The first 256 bytes go last: every later chunk, then bytes 4–255, then word
  0 alone. Each write is read back and compared before the next stage.
- AN3155 timings: 1 s for a normal reply, 10 s for the erase ACK.
- Sync: 8E1, wait 100 ms, then up to 20 × `0x7F`, 100 ms apart. ACK or NACK
  means synced.
- Cool-down: heater ≤ 50.0 °C (≤ 500 in 0.1 °C), polled every 5 s, given up
  after 30 minutes.
- Restart: Modbus must answer within 15 s of Go.
- Recovery probe: after 10 s with no OK reply, once every 30 s, waiting
  100 ms for the reply.
- Image: 8 to 30,720 B. SP in 0x20000000..0x20001800 inclusive. Reset vector
  in 0x08000000..0x080077FF. The marker is at 0xC0, 32 B, magic
  `0x57464869`, and is looked for only when the file is ≥ 0xE0 B.
- The page line for a marker image is exactly
  `UART build v0.5.0-1a2b3c4, protocol 1, checksum OK`. The fallback warning
  is exactly
  `This image has no checksum or version; it looks like a UART build from its vector table.`
- The link state name is `updating`.
- Canadian English in comments and docs. New files in `lib/remote-policy/`
  use LF, like their neighbours (`git ls-files --eol` shows `i/lf w/lf`).
  Keep each file's existing endings.
- Never flash or upload to any device (Remote or iHeater) without asking the
  user first. The Remote on hestia (192.168.81.77) is never touched.
- All five AGENTS.md gates pass before each commit.
- Commit messages end with a blank line, then
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **The Remote restarts during a job** (Setup save, Wi-Fi change, Remote
   OTA): these are refused with 409 while a job runs. The iHeater is never
   left mid-erase because the owner saved a setting. Built in Task 7;
   checked in Task 8, bench step 4.
2. **The iHeater stops answering mid-cool-down** (unplugged, Remote cable
   loose): three missed reads in a row stop the job with `no-reply-cooling`,
   before `0xB007`. It doesn't keep "cooling" for 30 minutes. Test: Task 5,
   `test_cooling_stops_after_three_missed_reads`.
3. **An air reading is invalid while the heater reads fine**: the job stops
   with `bad-reading` before `0xB007`. The iHeater would refuse it anyway, and
   the owner gets the real reason, not "exception 0x02". Test: Task 5,
   `test_invalid_air_reading_stops_before_entry`.
4. **A garbage byte instead of ACK/NACK during sync** (line noise as the
   parity switches): it counts as a miss, and the job doesn't spin. Test:
   Task 4, `test_sync_ignores_noise_byte`.
5. **A new image is uploaded after a successful flash, before the main loop
   notices the success**: the new image is not freed by the
   free-after-success housekeeping. Built in Task 7; checked in Task 8, bench
   step 5.

---

### Task 1: Image check and CRC-32

**Files:**
- Modify: `lib/remote-policy/src/UartRegisters.h` (add `kSupportedProtocol`)
- Modify: `lib/remote-policy/src/UartPollPolicy.h:45` (use it instead of the
  literal `1`)
- Create: `lib/remote-policy/src/FirmwareImage.h`,
  `lib/remote-policy/src/FirmwareImage.cpp`
- Test: `test/test_firmware_image/test_main.cpp`

**Interfaces:**
- Produces (namespace `iheaterlink::policy`):
  ```cpp
  // UartRegisters.h
  constexpr uint16_t kSupportedProtocol = 1;
  // FirmwareImage.h
  constexpr uint32_t kFlashBase = 0x08000000;
  constexpr uint32_t kFlashPageBytes = 1024;
  constexpr uint16_t kLastFlashPage = 29;
  constexpr size_t kMinImageBytes = 8, kMaxImageBytes = 30720;
  constexpr uint32_t kRamBase = 0x20000000, kRamEnd = 0x20001800;  // SP, inclusive
  constexpr uint32_t kResetVectorEnd = 0x08007800;                // exclusive
  constexpr size_t kMarkerOffset = 0xC0, kMarkerBytes = 32;
  constexpr uint32_t kMarkerMagic = 0x57464869;
  constexpr uint16_t kMarkerVersion = 1, kBuildTypeUart = 1;
  constexpr uint16_t kMarkerFlagDirty = 1, kMarkerFlagNoGit = 2;
  enum class ImageCheck : uint8_t { Ok, TooSmall, TooLarge, BadStackPointer,
    BadResetVector, BadRecord, NotUartBuild, LengthMismatch, BadChecksum,
    UnsupportedProtocol, NotUartVectors };
  struct ImageInfo {
    ImageCheck result = ImageCheck::TooSmall;
    bool hasMarker = false;
    uint32_t size = 0;
    uint16_t protocol = 0, fwVersion = 0, fwPatch = 0, flags = 0;
    uint32_t gitSha = 0, crc = 0;
  };
  uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0);  // as zlib.crc32, chainable
  ImageInfo checkImage(const uint8_t *data, size_t len);
  const char *imageCheckMessage(ImageCheck result);
  size_t imageVersion(const ImageInfo &info, char *out, size_t cap);  // "v0.5.0-1a2b3c4[-dirty]"
  size_t imageSummary(const ImageInfo &info, char *out, size_t cap);  // the page line
  ```

- [ ] **Step 1: Write the failing tests** in
  `test/test_firmware_image/test_main.cpp`.

  The fixture is `makeImage(size)`:
  - byte i = `(i * 7 + 3) & 0xFF`;
  - word 0 = `0x20001800`, word 1 = `0x08000101`;
  - vectors 2..47 = `0x080065F1`, except vector 43 (offset 0xAC) =
    `0x08006315`;
  - at 0xC0: magic, version 1, size 32, build type 1, protocol 1, fw
    `0x0005`, patch 0, length 0, CRC 0, SHA 0, flags 0, reserved 0.

  `patchImage(buf, size, sha, flags)` sets length = size, SHA and flags, then
  the CRC over the buffer with the CRC field 0. Tests:
  - `test_crc32_matches_zlib`: `crc32("123456789")` == `0xCBF43926`;
    `crc32` of 32 zero bytes == `0x190A55AD`; `crc32` of length 0 == 0;
    chaining two halves gives the same result as one call.
  - `test_marker_fixture_crc_matches_zlib`: `makeImage(1024)` patched with
    SHA `0x1A2B3C4D`, flags 0, has CRC `0xF684F4BD` at 0xD4. This value came
    from the iHeater repo's `build_marker.patch`. `checkImage` gives `Ok`,
    `hasMarker`, protocol 1, fwVersion 5, gitSha `0x1A2B3C4D`.
  - `test_summary_line_for_marker_image`: `imageSummary` gives
    `UART build v0.5.0-1a2b3c4, protocol 1, checksum OK`.
  - `test_version_dirty_and_no_git`: flags 1 → `v0.5.0-1a2b3c4-dirty`;
    flags 2 → `v0.5.0`; flags 3 → `v0.5.0`.
  - `test_size_limits`: 7 B → `TooSmall`; 30,721 B → `TooLarge`; a
    30,720 B fallback image (no magic) → `Ok`.
  - `test_stack_pointer_range`: SP `0x1FFFFFFC` and `0x20001804` →
    `BadStackPointer`; `0x20000000` and `0x20001800` pass.
  - `test_reset_vector_range`: `0x07FFFFFF` and `0x08007800` →
    `BadResetVector`; `0x080077FF` passes.
  - `test_record_checks_in_order`: record version 2 → `BadRecord`; record
    size 33 → `BadRecord`; build type 2 → `NotUartBuild`; length 1023 in a
    1024 B file → `LengthMismatch`; one image byte flipped at 0x300 →
    `BadChecksum`; protocol 2 (re-patched) → `UnsupportedProtocol`.
  - `test_fallback_uart_vectors`: no magic, vector 43 differs → `Ok`,
    `!hasMarker`; `imageSummary` gives the fallback warning exactly.
  - `test_fallback_refuses_non_uart_vectors`: no magic, vector 43 equals the
    default → `NotUartVectors`.
  - `test_magic_ignored_below_0xE0`: a 0xDF B image with the magic bytes at
    0xC0 and vector 43 differing → `Ok`, `!hasMarker`.
  - `test_fallback_needs_whole_vector_table`: a 0xBF B image with no magic →
    `NotUartVectors`.

- [ ] **Step 2: Run to verify they fail**

  Run: `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native -f test_firmware_image`
  Expected: compile failure, `FirmwareImage.h: No such file`.

- [ ] **Step 3: Implement.**
  - Add `kSupportedProtocol` and switch `UartPollPolicy::protocolSupported`
    to use it.
  - Write `FirmwareImage.{h,cpp}`. Use a table-free bitwise CRC (poly
    `0xEDB88320`, init `~crc`, final `~`); the image is at most 30 KB.
  - Read all fields little-endian, byte by byte (no casts onto the buffer).
  - Run the checks in the order of the enum, as in design "Image check"
    1–5. The fallback's "most common value" is the mode of vectors 16..47;
    on a tie, the first value to reach the top count wins.
  - `imageCheckMessage` returns this exact text:

    | Result | Text |
    |---|---|
    | Ok | `OK` |
    | TooSmall | `The file is too small to be iHeater firmware.` |
    | TooLarge | `The file is over 30,720 bytes, too big for the iHeater.` |
    | BadStackPointer | `The file doesn't start with an iHeater vector table (stack pointer).` |
    | BadResetVector | `The file doesn't start with an iHeater vector table (reset vector).` |
    | BadRecord | `The build record has an unknown version or size.` |
    | NotUartBuild | `This is not a UART build of the iHeater firmware.` |
    | LengthMismatch | `The file is truncated or has extra bytes: its length doesn't match its build record.` |
    | BadChecksum | `The file is damaged: its checksum doesn't match.` |
    | UnsupportedProtocol | `This firmware uses a link protocol this Remote doesn't support.` |
    | NotUartVectors | `This file has no build record and doesn't look like a UART build.` |

  - `imageSummary` gives the marker line, or the fallback warning, or
    `imageCheckMessage`.
  - The version shows `gitSha >> 4` as 7 lowercase hex digits.

- [ ] **Step 4: Run to verify they pass**

  Run: the same command, then the full `pio test -e native`.
  Expected: `test_firmware_image` all pass; the existing 123 tests still
  pass.

- [ ] **Step 5: Commit**
  ```bash
  git add lib/remote-policy/src/FirmwareImage.* lib/remote-policy/src/UartRegisters.h lib/remote-policy/src/UartPollPolicy.h test/test_firmware_image
  git commit -m "feat(policy): check iHeater images and their build record"
  ```

### Task 2: AN3155 frame builders

**Files:**
- Create: `lib/remote-policy/src/RomProtocol.h`,
  `lib/remote-policy/src/RomProtocol.cpp`
- Test: `test/test_flash_job/test_main.cpp` (created here, and extended by
  Tasks 4 and 5)

**Interfaces:**
- Consumes: `kLastFlashPage` (Task 1).
- Produces:
  ```cpp
  constexpr uint8_t kRomAck = 0x79, kRomNack = 0x1F, kRomSync = 0x7F;
  constexpr uint8_t kRomGet = 0x00, kRomGetId = 0x02, kRomRead = 0x11,
                    kRomGo = 0x21, kRomWrite = 0x31, kRomErase = 0x43,
                    kRomExtErase = 0x44;
  constexpr uint16_t kExpectedPid = 0x445;
  constexpr size_t kRomChunkBytes = 256;
  constexpr uint32_t kRomReplyMs = 1000, kRomEraseMs = 10000;
  uint8_t romXor(const uint8_t *data, size_t len);
  void buildRomCommand(uint8_t cmd, uint8_t out[2]);
  void buildRomAddress(uint32_t addr, uint8_t out[5]);  // big-endian + XOR
  size_t buildEraseBody(uint16_t pages, bool extended, uint8_t *out, size_t cap);
  size_t buildWriteBody(const uint8_t *data, size_t len, uint8_t *out, size_t cap);
  void buildReadCount(size_t count, uint8_t out[2]);  // count-1, ~(count-1)
  ```

- [ ] **Step 1: Write the failing tests**
  - `test_command_frame`: `0x31` → `{0x31, 0xCE}`; `0x00` → `{0x00, 0xFF}`.
  - `test_address_frame`: `0x08000100` → `{08 00 01 00 09}`.
  - `test_extended_erase_body`: 3 pages →
    `{00 02 00 00 00 01 00 02 01}`, return 9.
  - `test_legacy_erase_body`: 3 pages → `{02 00 01 02 01}`, return 5.
  - `test_erase_refuses_page_30`: 30 pages returns 63 (extended) and 32
    (legacy); 31 pages and 0 pages return 0 for both; a short `cap` returns
    0.
  - `test_write_body`: `{1,2,3,4}` → `{03 01 02 03 04 07}`, return 6; length
    5, 0 and 260 return 0; 256 bytes return 258 with first byte `0xFF`.
  - `test_read_count`: 256 → `{FF 00}`; 4 → `{03 FC}`.

- [ ] **Step 2: Run to verify they fail**

  Run: `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native -f test_flash_job`
  Expected: compile failure, `RomProtocol.h: No such file`.

- [ ] **Step 3: Implement** `RomProtocol.{h,cpp}`.
  - Erase body: extended is `(N−1)` BE16, each page BE16, then XOR. Legacy is
    `N−1`, each page byte, then XOR.
  - Write body is `len−1`, the data, then the XOR of all of those. It
    requires `len` 1..256 and a multiple of 4.

- [ ] **Step 4: Run to verify they pass.** Same command. Expected: all pass.

- [ ] **Step 5: Commit**
  ```bash
  git add lib/remote-policy/src/RomProtocol.* test/test_flash_job
  git commit -m "feat(policy): build STM32 ROM bootloader frames"
  ```

### Task 3: The `updating` link state

**Files:**
- Modify: `lib/remote-policy/src/UartPollPolicy.h`
  (`enum class LinkStatus`), `lib/remote-policy/src/UartPollPolicy.cpp`
  (`linkStatusName`)
- Modify: `src/standalone/StandaloneApp.cpp:494-506` (`uartLinkName`)
- Modify: `klipper/heater_iheater_remote.py:24`, `moonraker/iheater_remote.py:19`
- Test: `test/test_remote_policy/test_main.cpp`,
  `klipper/test_heater_iheater_remote.py`, `moonraker/test_iheater_remote.py`
- Docs: `klipper/README.md` (the `linked` and link sections)

**Interfaces:**
- Produces: `LinkStatus::Updating` (appended last, after `NoReply`);
  `linkStatusName(LinkStatus::Updating)` == `"updating"`.

- [ ] **Step 1: Write the failing tests**
  - Native `test_link_status_name_updating`: `"updating"`.
  - Klipper `test_link_updating_falls_back_to_sensor`: like
    `test_link_not_up_falls_back_to_sensor`, with `link='updating'`; the
    report is accepted and `linked` is false.
  - Moonraker `test_updating_link_accepted`: `good()` with
    `link='updating'` is accepted (not 400).

- [ ] **Step 2: Run to verify they fail**

  Run: the native suite, plus
  `python -m unittest discover -s klipper -p "test_*.py"` and the same for
  `moonraker`.
  Expected: the three new tests fail; the Python ones fail with "link must
  be one of".

- [ ] **Step 3: Implement.**
  - Add the enum value and its name.
  - Add `case Updating: return "updating";` to `uartLinkName`.
  - Append `'updating'` to both `_LINKS` tuples.
  - In `klipper/README.md`, list `updating` with the other link states:
    "the Remote is updating the iHeater's firmware; `linked` is false".

- [ ] **Step 4: Run to verify they pass.** All five gates. Expected: green.

- [ ] **Step 5: Commit**
  ```bash
  git commit -am "feat: add the updating link state"
  ```

### Task 4: Flash job — ROM phases, Retry and Recovery

**Files:**
- Create: `lib/remote-policy/src/FlashJob.h`,
  `lib/remote-policy/src/FlashJob.cpp`
- Test: `test/test_flash_job/FakeIheater.h` (new),
  `test/test_flash_job/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 constants and `kNoReading`, `decodeInputs`; Task 2
  builders; `PollRequest`, `Reply`, `ReplyKind`, `LinkStatus`,
  `kPollPeriodMs`.
- Produces:
  ```cpp
  enum class LinkParity : uint8_t { None, Even };
  enum class FlashStart : uint8_t { Normal, Recovery, Retry };
  enum class FlashPhase : uint8_t { Idle, Cooling, Entering, Syncing, Erasing,
                                    Writing, Starting, Restarting, Finished };
  enum class FlashOutcome : uint8_t { None, Updated, PowerCycleNeeded,
    Cancelled, HeatRequested, CoolTimeout, BadReading, NoReplyCooling,
    EntryRefused, EntryNotConfirmed, ImageRejected, SyncFailed, RomError,
    VerifyFailed, NoReplyAfterUpdate };
  constexpr uint32_t kCoolPollMs = 5000, kCoolTimeoutMs = 30u * 60u * 1000u;
  constexpr int16_t kSafeHeaterT10 = 500;
  constexpr uint8_t kCoolMissLimit = 3;
  constexpr uint32_t kSyncSettleMs = 100, kSyncGapMs = 100;
  constexpr uint8_t kSyncTries = 20;
  constexpr uint32_t kRestartWaitMs = 15000;
  constexpr uint32_t kRomProbeAfterMs = 10000, kRomProbePeriodMs = 30000,
                     kRomProbeReplyMs = 100;
  struct FlashStatus {
    FlashPhase phase = FlashPhase::Idle;
    FlashOutcome outcome = FlashOutcome::None;  // None while idle or running
    FlashPhase failedPhase = FlashPhase::Idle;
    uint16_t progressPermille = 0;   // bytes verified / padded length
    int16_t heaterT10 = kNoReading;  // last cool-down reading
    bool entryUnconfirmed = false;   // 0xB007 got no usable reply
    bool extendedErase = false, unexpectedPid = false;
    uint16_t pid = 0;
    char detail[48] = "";            // e.g. "write at 0x08000100: NACK"
  };
  class FlashLink {
   public:
    virtual ~FlashLink() = default;
    virtual void setParity(LinkParity parity) = 0;  // also drops pending input
    virtual void send(const uint8_t *data, size_t len) = 0;  // returns once sent
    virtual size_t receive(uint8_t *buf, size_t len, uint32_t timeoutMs) = 0;
    virtual void flushInput() = 0;
    virtual bool modbus(const PollRequest &req, Reply &reply) = 0;  // false: nothing usable in time
    virtual void sleepMs(uint32_t ms) = 0;
    virtual uint32_t nowMs() = 0;
    virtual bool heatWanted() = 0;
    virtual bool cancelRequested() = 0;
    virtual void publish(const FlashStatus &status) = 0;
  };
  FlashStatus runFlashJob(FlashLink &link, const uint8_t *image, size_t len, FlashStart start);
  bool probeRom(FlashLink &link);
  bool romProbeDue(uint32_t nowMs, uint32_t lastOkMs, uint32_t lastProbeMs);
  bool retryAllowed(FlashOutcome outcome);
  bool applicationIntact(FlashOutcome outcome);
  const char *flashPhaseName(FlashPhase phase);
  const char *flashOutcomeName(FlashOutcome outcome);
  const char *flashOutcomeMessage(FlashOutcome outcome);
  ```
  - Phase names: `idle cooling entering syncing erasing writing starting
    restarting finished`.
  - Outcome names: `none updated power-cycle cancelled heat-requested
    cool-timeout bad-reading no-reply-cooling entry-refused
    entry-not-confirmed image-rejected sync-failed rom-error verify-failed
    no-reply-after-update`.
  - `retryAllowed`: SyncFailed, RomError, VerifyFailed, NoReplyAfterUpdate.
  - `applicationIntact`: Cancelled, HeatRequested, CoolTimeout, BadReading,
    NoReplyCooling, EntryRefused, EntryNotConfirmed, ImageRejected.

- [ ] **Step 1: Write `FakeIheater`** (a test utility, implementing
  `FlashLink`).
  - **Clock:** a virtual clock starts at 1000 ms. `sleepMs` advances it, and
    `receive` advances it by the timeout when fewer bytes than asked are
    queued.
  - **Mode:** the fake is in `App` or `Rom` mode, and tracks parity.
  - **ROM mode, at 8E1:**
    - It answers sync, Get, Get ID, Erase/Extended Erase, Write, Read and Go
      byte-accurately per AN3155, against a 32 KB flash array of `0xFF`.
    - It ignores all bytes sent at 8N1. In App mode, it ignores all bytes
      sent at 8E1.
    - It records the order of write addresses, the erased pages, the Go
      address, and every published `FlashStatus`.
    - Knobs:
      - `extended` (Get lists `0x44`, else `0x43`), `pid`;
      - `alreadySynced` (the first `0x7F` gets NACK);
      - `romSilent`;
      - `noiseBeforeSyncAck` (one `0x00` byte answers the first `0x7F`);
      - `nackWriteAt` (NACK the data phase of a write at that address);
      - `corruptReadAt` (flip one byte read back at that address);
      - `modeAfterGo` (`App`, `Rom` or `Silent`).
  - **App mode, `modbus()`:**
    - It returns `false` unless parity is None and the fake is in App mode.
    - It builds 22 input registers so that `decodeInputs` gives its
      `heaterT10`, `airT10` and `airTargetT10`, taking the next value from
      an optional `heaterSeq` on each read.
    - Later steps (Task 5) add the target-write and `0xB007` handling.

- [ ] **Step 2: Write the failing tests.** Each test starts with
  `makeImage(3000)`, borrowed from Task 1's fixture and patched, so N = 3
  pages, padded length 3000.
  - `test_retry_starts_at_sync`: Rom mode, `FlashStart::Retry`.
    - The outcome is `Updated` and the published phases are Syncing,
      Erasing, Writing, Starting, Restarting, Finished.
    - No `modbus()` call was made before Restarting.
    - Flash bytes 0..2999 equal the image, and the erased pages are
      `{0,1,2}`.
  - `test_recovery_skips_cooling_and_entry`: the same with
    `FlashStart::Recovery`.
  - `test_write_order_word0_last`: the write addresses are `0x08000100`,
    `0x08000200`, … `0x08000B00`, then `0x08000004` (252 B), then
    `0x08000000` (4 B).
  - `test_padding_to_word`: a 3001 B image writes its last chunk as 188 B,
    ending in three `0xFF`.
  - `test_sync_nack_already_synced`: `alreadySynced` → `Updated`.
  - `test_sync_ignores_noise_byte`: `noiseBeforeSyncAck` → `Updated`. The
    clock advanced by at least `kSyncGapMs` before the second `0x7F`.
  - `test_sync_timeout`: `romSilent` → `SyncFailed`, failedPhase Syncing,
    detail `sync: no reply after 20 tries`, `retryAllowed`. The clock
    advanced between 2 and 5 s. Parity is None at the end.
  - `test_legacy_erase`: `extended=false` → the legacy body was sent and
    `extendedErase` is false.
  - `test_unexpected_pid_warns`: pid `0x440` → `Updated`, `unexpectedPid`.
  - `test_write_nack_reports_step`: `nackWriteAt=0x08000200` → `RomError`,
    detail `write at 0x08000200: NACK`, failedPhase Writing, no Go.
  - `test_verify_mismatch_stops`: `corruptReadAt=0x08000300` →
    `VerifyFailed`, detail `verify at 0x08000300: data mismatch`. Word 0 was
    never written, and there was no Go.
  - `test_go_to_flash_base`: the Go address is `0x08000000`.
  - `test_restart_rom_answers_power_cycle`: `modeAfterGo=Rom` →
    `PowerCycleNeeded`, `!retryAllowed`.
  - `test_restart_silent`: `modeAfterGo=Silent` →
    `NoReplyAfterUpdate`, about 15 s of clock, `retryAllowed`.
  - `test_progress_reaches_1000_last`: the published `progressPermille`
    values never decrease, and 1000 appears only in the last Writing
    publish.
  - `test_oversize_image_rejected`: a length of 30,721 → `ImageRejected`
    with nothing sent.
  - `test_probe_rom`: ACK → true; NACK → true; silence → false. Parity is
    None after each.
  - `test_rom_probe_due`:
    - `(9999, 0, 0)` → false; `(10000, 0, 0)` → true;
    - `(50000, 45000, 0)` → false;
    - `(50000, 1000, 30000)` → false; `(60000, 1000, 30000)` → true.

- [ ] **Step 3: Run to verify they fail.**
  `pio test -e native -f test_flash_job`. Expected: compile failure,
  `FlashJob.h: No such file`.

- [ ] **Step 4: Implement** `FlashJob.{h,cpp}`.
  - For now, a `Normal` start returns `ImageRejected` with the detail
    `normal start not built yet`. Task 5 replaces that.
  - **Sync:**
    - `setParity(Even)`, then `sleepMs(100)`.
    - Up to 20 tries of: `flushInput`, send `0x7F`, then `receive(1, 100)`.
      ACK or NACK means synced. Anything else, `sleepMs(100)`.
    - When synced, `sleepMs(50)` and `flushInput`.
  - **Get:** the command, ACK, N, N+1 bytes, ACK. `0x44` present means
    extended erase. Otherwise `0x43`. Neither gives `RomError` with
    `get: neither Erase nor Extended Erase offered`.
  - **Get ID:** the PID, big-endian, from N+1 bytes.
  - **Erase:** wait `kRomEraseMs` for the final ACK.
  - **Writes and verifies:** write a chunk, then read it back and compare,
    before the next chunk. The order follows the Global Constraints.
  - **Details:** every NACK or timeout puts `<step>: NACK` or
    `<step>: timeout` in `detail`. The steps are `get`, `get id`, `erase`,
    `write at 0x%08X`, `verify at 0x%08X` and `go` (uppercase hex).
  - **Go:** the command, ACK, the address, ACK.
  - **Restarting:**
    - `setParity(None)`.
    - Until 15 s pass: `modbus(ReadInputs)`, where Ok means `Updated`;
      otherwise `sleepMs(kPollPeriodMs)`.
    - Then `probeRom`: true means `PowerCycleNeeded`, false means
      `NoReplyAfterUpdate`.
  - Every return path leaves the parity None and publishes the final status
    with phase Finished.
  - `flashOutcomeMessage` is the page text. Use these exactly:
    - PowerCycleNeeded:
      `The iHeater was updated but is still in its bootloader. Power-cycle the iHeater; don't flash it again.`
    - EntryNotConfirmed:
      `Entry not confirmed; the iHeater is still running its firmware.`
    - HeatRequested: `Stopped: heat requested.`

    Write the rest in the same voice.

- [ ] **Step 5: Run to verify they pass.** Run `-f test_flash_job`, then the
  full native suite. Expected: green.

- [ ] **Step 6: Commit**
  ```bash
  git add lib/remote-policy/src/FlashJob.* test/test_flash_job
  git commit -m "feat(policy): flash the iHeater through its ROM bootloader"
  ```

### Task 5: Flash job — cool-down, entry and the start decision

**Files:**
- Modify: `lib/remote-policy/src/UartRegisters.h` (constants),
  `lib/remote-policy/src/UartPollPolicy.h` (`PollKind`),
  `lib/remote-policy/src/FlashJob.{h,cpp}`
- Test: `test/test_flash_job/FakeIheater.h`, `test/test_flash_job/test_main.cpp`

**Interfaces:**
- Consumes: Task 4's `FlashJob` and its fake.
- Produces:
  ```cpp
  // UartRegisters.h
  constexpr uint16_t kRegBootloader = 0x0102, kBootloaderMagic = 0xB007;
  // UartPollPolicy.h: PollKind gains WriteBootloader (appended last)
  // FlashJob.h
  bool heatWanted(bool targetMode, float targetC);  // targetMode && targetC > 0
  struct FlashStartInputs {
    bool uartOutput = false, imageOk = false, heatWanted = false,
         jobRunning = false, romDetected = false;
    LinkStatus link = LinkStatus::Waiting;
    FlashOutcome lastOutcome = FlashOutcome::None;
  };
  enum class FlashRefusal : uint8_t { None, NotUart, NoImage, HeatWanted,
                                      JobRunning, LinkDown, NothingToRetry };
  struct FlashStartDecision { FlashRefusal refusal; FlashStart start; };
  FlashStartDecision decideFlashStart(const FlashStartInputs &in, bool retry);
  const char *flashRefusalMessage(FlashRefusal refusal);
  ```

- [ ] **Step 1: Extend `FakeIheater`'s App mode.**
  - **Target writes:** a `WriteTarget` to `0x0100` stores the value and
    counts it.
  - **`WriteBootloader`:**
    - It counts each call.
    - If `b007Exception` is set, it replies with that exception and stays in
      App mode.
    - Otherwise it replies Ok, unless `dropB007Reply` is set, and switches to
      Rom mode, unless `b007StaysApp` is set.
    - It refuses (exception 2) unless the air target is 0 and the heater is
      ≤ 500, mirroring the iHeater.
  - **Readings:** `appSilentAfter` makes reads after the Nth return false.
  - **Callbacks:** `heatWanted()` and `cancelRequested()` return knobs that a
    test can flip at a given clock time.

- [ ] **Step 2: Write the failing tests.** All of these are `FlashStart::Normal`
  unless named otherwise.
  - `test_normal_update_end_to_end`: heater 300, air target 0.
    - The outcome is `Updated`, with exactly one `WriteBootloader` and no
      target write.
    - The phases run Cooling, Entering, Syncing, … Finished.
  - `test_cooling_writes_target_only_when_nonzero`: air target 600 → exactly
    one target write of 0, then `Updated`.
  - `test_cooling_waits_until_50C`: `heaterSeq` is 800, 650, 501, 500.
    - `WriteBootloader` is sent after the fourth read.
    - The clock shows about 15 s of 5 s polls.
    - The published `heaterT10` follows the readings.
  - `test_cooling_times_out_after_30_min`: the heater stays at 600 →
    `CoolTimeout`, no `WriteBootloader`, and the clock is ≥ 1,800,000 ms
    past the start.
  - `test_cancel_during_cooling`: cancel at +10 s → `Cancelled`, no
    `WriteBootloader`, `applicationIntact`.
  - `test_heat_requested_during_cooling`: heat at +10 s → `HeatRequested`.
  - `test_heat_requested_just_before_entry`: heat turns true at the moment
    the heater first reads ≤ 500 → `HeatRequested`, no `WriteBootloader`.
  - `test_invalid_heater_reading_stops`: heater `kNoReading` →
    `BadReading`.
  - `test_invalid_air_reading_stops_before_entry`: air `kNoReading` →
    `BadReading`, no `WriteBootloader`.
  - `test_cooling_stops_after_three_missed_reads`: `appSilentAfter=1` →
    `NoReplyCooling` after 3 misses, no `WriteBootloader`.
  - `test_entry_exception_stops`: `b007Exception=2` → `EntryRefused`,
    detail `exception 0x02`.
    - Nothing was sent at 8E1.
    - The fake is still in App mode, with exactly one `WriteBootloader`.
  - `test_entry_no_reply_still_syncs`: `dropB007Reply` → `Updated`,
    `entryUnconfirmed`, with exactly one `WriteBootloader`.
  - `test_entry_not_confirmed`: `dropB007Reply` and `b007StaysApp` →
    `EntryNotConfirmed`, failedPhase Syncing, `!retryAllowed`,
    `applicationIntact`. There was no second `WriteBootloader`.
  - `test_heat_wanted_rule`: `(true, 0.0)` → false; `(true, 0.5)` → true;
    `(false, 60)` → false.
  - `test_decide_flash_start`:
    - Up and all clear → `{None, Normal}`.
    - NoReply with `romDetected` → `{None, Recovery}`.
    - NoReply without it → `LinkDown`.
    - The refusals are checked in this order: `NotUart`, `JobRunning`,
      `NoImage`, `HeatWanted`, `LinkDown`.
    - Retry with `lastOutcome=SyncFailed` and the link NoReply →
      `{None, Retry}`.
    - Retry with `lastOutcome=Updated` → `NothingToRetry`.
    - Retry still refuses `HeatWanted`. For Retry, `NothingToRetry` takes
      the place of `LinkDown` in the order.

- [ ] **Step 3: Run to verify they fail.** Run `-f test_flash_job`.
  Expected: the new tests fail, and Task 4's still pass.

- [ ] **Step 4: Implement.**
  - **Cooling:** loop until the job leaves this phase.
    1. Check cancel, then heat.
    2. `modbus(ReadInputs)` and `decodeInputs`. A false return, a non-Ok
       reply or a decode failure is a miss; three in a row stop the job with
       `NoReplyCooling`.
    3. If the heater or air reading is `kNoReading`, stop with `BadReading`.
    4. If the air target isn't 0, write target 0.
    5. If the heater is ≤ 500 and the air target read 0, go to Entering.
    6. At 30 minutes, stop with `CoolTimeout`.
    7. `sleepMs(kCoolPollMs)`.
  - **Entering:**
    - Check cancel and heat once more.
    - Send `{WriteBootloader, 0x0102, 0xB007}`.
    - An Ok reply means entered. An Exception reply gives `EntryRefused`.
      Anything else sets `entryUnconfirmed`.
  - **When the sync fails after an unconfirmed entry:** `setParity(None)`,
    `sleepMs(50)`, one `modbus(ReadInputs)`. Ok gives `EntryNotConfirmed`;
    otherwise `SyncFailed`.
  - **`flashRefusalMessage`:**

    | Refusal | Text |
    |---|---|
    | NotUart | `Switch the output to UART first.` |
    | NoImage | `Upload a firmware file that passes the check first.` |
    | HeatWanted | `Turn the heat off first: Manual Off, or a Klipper target of 0.` |
    | JobRunning | `An iHeater update is already running.` |
    | LinkDown | `The iHeater isn't answering, and its bootloader wasn't detected.` |
    | NothingToRetry | `There's no failed update to retry.` |

- [ ] **Step 5: Run to verify they pass.** Run `-f test_flash_job`, then all
  five gates. Expected: green.

- [ ] **Step 6: Commit**
  ```bash
  git add lib/remote-policy/src test/test_flash_job
  git commit -m "feat(policy): cool down and enter the bootloader before flashing"
  ```

### Task 6: Run the job in the UART task

**Files:**
- Modify: `src/standalone/UartLink.h`, `src/standalone/UartLink.cpp`

**Interfaces:**
- Consumes: everything from Tasks 4 and 5.
- Produces, on `UartLink`:
  ```cpp
  // Main loop → task. False when a job is already queued or running, or the
  // link is disabled. The caller keeps `image` alive and unchanged until
  // flashBusy() turns false.
  bool startFlash(const uint8_t *image, size_t len, policy::FlashStart start);
  void cancelFlash();     // only takes effect while cooling
  bool flashBusy() const; // queued or running
  ```
  `UartLinkView` gains `policy::FlashStatus flash;`,
  `bool flashRunning = false;` and `bool romDetected = false;`.

- [ ] **Step 1: Implement**
  - Add a private nested `class FlashPort final : public policy::FlashLink`
    in `UartLink.h`, defined in `UartLink.cpp`, holding a `UartLink &`.
    - `setParity`: `uart_wait_tx_done(kPort, pdMS_TO_TICKS(20))`, then
      `uart_set_parity(kPort, UART_PARITY_EVEN or UART_PARITY_DISABLE)`, then
      `uart_flush_input`.
    - `send`: `uart_write_bytes` + `uart_wait_tx_done(kPort, pdMS_TO_TICKS(100))`.
    - `receive`: read until `len` bytes or the deadline, in slices of at
      most 500 ms, calling `feedWatchdog()` after each slice. A 10 s erase
      wait must never starve the task watchdog (5 s).
    - `modbus`: the existing `exchange()`.
    - `sleepMs`: `vTaskDelay` in slices of at most 500 ms, feeding the
      watchdog.
    - `nowMs`: `millis()`.
    - `heatWanted`: under `mux_`, `policy::heatWanted(lastCommand_.mode ==
      ControllerOutputMode::TargetTemperature, lastCommand_.targetTempC)`.
    - `cancelRequested`: a flag under `mux_`.
    - `publish`: under `mux_`, copy into `view_.flash`, and set
      `view_.flashRunning = true` and `view_.link = LinkStatus::Updating`.
  - `buildFrame` gains `PollKind::WriteBootloader` →
    `buildWriteSingle(req.addr, req.value, out)`.
  - `taskLoop`, at the top of each cycle:
    - Take a queued job under `mux_`. If there is one, run
      `runFlashJob(port, image, len, start)`, then store the result under
      `mux_`, clear the busy flags and `publishView()`.
    - Any `apply()` during the job stays pending (`pendingDirty_`), and the
      next cycle sends it as usual.
  - Track `lastOkMs_` in `runRequest` (on each Ok).
    - After the cycle's requests, if no Ok and
      `romProbeDue(millis(), lastOkMs_, lastProbeMs_)`, set
      `romDetected_ = probeRom(port)` and `lastProbeMs_ = millis()`.
    - Any Ok clears `romDetected_`.
    - Publish `romDetected` in the view.
  - `publishView` keeps `link = Updating`, the `flash` status and
    `flashRunning` while a job runs, and copies the last `flash` status
    afterwards.
  - Raise `kTaskStackBytes` from 4096 to 6144. The job holds a 258 B write
    body and a 256 B read buffer on the stack.
  - Log each phase change and the outcome on Serial as
    `[UART] flash phase=<name>` and `[UART] flash outcome=<name> <detail>`.
    Log from loopTask (like `updateUartLinkLog`), not from the task.

- [ ] **Step 2: Build**

  Run: `powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$TEMP/pio-nomsys.ps1")" run -e esp32c3-super-mini-remote > "$TEMP/build.log" 2>&1; tail -20 "$TEMP/build.log"`
  Expected: `SUCCESS`, with no new warnings from `UartLink.cpp`.

- [ ] **Step 3: Run all five gates.** Expected: green.

- [ ] **Step 4: Commit**
  ```bash
  git commit -am "feat(firmware): run iHeater flash jobs in the UART task"
  ```

### Task 7: Upload, endpoints and the `/fw` card

**Files:**
- Modify: `src/standalone/StandaloneApp.cpp`

**Interfaces:**
- Consumes: `checkImage`, `imageSummary`, `kMaxImageBytes`,
  `decideFlashStart`, `flashRefusalMessage`, `heatWanted`, `retryAllowed`,
  the names and messages (Tasks 1, 4, 5); `UartLink::startFlash`,
  `cancelFlash`, `flashBusy`, and the view fields (Task 6).
- Produces: the HTTP endpoints below, and `iheaterUpdate` in the status
  JSON.

- [ ] **Step 1: Implement image storage**
  - Add file-static `uint8_t *s_ihImage`, `size_t s_ihImageLen`,
    `policy::ImageInfo s_ihInfo`, `String s_ihError`, and
    `uint32_t s_ihImageSeq` (bumped at each upload START) with
    `uint32_t s_ihJobImageSeq` (copied from it when a job starts).
  - Register `/api/iheater/image` with both handlers, like `/api/ota`, and
    the four action routes below.
  - `handleIheaterImageUpload`:
    - On START, refuse when `s_uart.flashBusy()`, setting
      `s_ihError = "An iHeater update is running."` and copying nothing.
      Otherwise free the old buffer and `malloc(kMaxImageBytes)`; on null,
      set `s_ihError = "Not enough memory for the image."`.
    - On WRITE, copy while it fits. Past 30,720 B, set the TooLarge message
      and stop copying.
    - On END, `s_ihInfo = checkImage(...)`. If that isn't `Ok`, free the
      buffer and keep the info for the message.
    - On ABORTED, free the buffer, with `s_ihError = "Upload aborted."`.
  - `handleIheaterImagePost` returns 200 `{"ok":true,"line":<imageSummary>,"fallback":<!hasMarker>}`,
    or 400 `{"error":<text>}`.
- [ ] **Step 2: Implement the actions** (all POST; refusals are 409 `{"error":…}`;
  success is `sendActionStatus()`):
  - `/api/iheater/update` and `/api/iheater/retry`: fill `FlashStartInputs`.
    - `heatWanted` comes from `s_output->getLastCommand()`.
    - `link` and `romDetected` come from `s_uart.view()`.
    - `lastOutcome` comes from `view.flash.outcome`.
    - Then call `decideFlashStart(in, retry)` and, if there is no refusal,
      `s_uart.startFlash(s_ihImage, s_ihImageLen, start)`, and set
      `s_ihJobImageSeq = s_ihImageSeq`.
  - `/api/iheater/cancel`: 409
    `Cancel is only possible while cooling down.` unless
    `view.flash.phase == Cooling`.
  - `/api/iheater/image/discard`: 409 while busy; otherwise free and clear.
  - While `s_uart.flashBusy()`, the following return 409
    `An iHeater update is running.` and change nothing:
    - `/api/setup`, `/api/wifi` and `/api/wifi/clear`;
    - `/api/ota`, which sets `s_otaError` at upload START and doesn't call
      `Update.begin`.
  - Housekeeping in `loop()`: when `flashBusy()` falls from true to false
    and the outcome is `Updated` or `PowerCycleNeeded`, free the image, but
    only if `s_ihImageSeq == s_ihJobImageSeq`, so a newer upload survives.
- [ ] **Step 3: Implement the status JSON.**
  - Raise the `statusJson` document to `StaticJsonDocument<3072>`.
  - In UART output, add `iheaterUpdate`:
    - the job: `phase`, `outcome`, `message` (when finished), `failedPhase`,
      `detail`;
    - `progress` (permille / 10, an integer) and `heaterC` (null when there
      is no reading);
    - `entryUnconfirmed`, `unexpectedPid`, `romDetected`;
    - `running`, `canCancel` (Cooling) and `canRetry` (`retryAllowed`, the
      image is loaded and the job isn't busy);
    - `image`: `{loaded, size, line, fallback}`, or `{error}`.
  - The link name `updating` needs no change here; it comes from
    `linkStatusName`.
- [ ] **Step 4: Implement the `/fw` card.**
  - `handleFirmwarePage` swaps a `__IHCARD__` placeholder for the card
    markup in UART output, and for an empty string otherwise.
  - The card shows:
    - the running iHeater version and protocol, from the snapshot;
    - "iHeater is in its bootloader: upload firmware to recover" when
      `romDetected`;
    - the file picker;
    - the image line, the fallback warning in the page's `.warning` style,
      or the error;
    - **Update iHeater**, **Cancel** and **Retry**, enabled from the JSON
      flags;
    - the phase with progress, and the final message.
  - From Entering on, it says the iHeater's application is erased and not to
    power off either board.
  - Its JS subscribes to the same status WebSocket as the dashboard (port
    81; see `RemoteDashboard.h`) and posts the image as multipart field
    `firmware`.
  - Keep the existing OTA card and its styles.
- [ ] **Step 5: Build, and run all five gates.** Use the same build command
  as Task 6. Expected: `SUCCESS`, all green. Note the RAM and flash use
  from the build tail.
- [ ] **Step 6: Commit**
  ```bash
  git commit -am "feat(firmware): update the iHeater from the firmware page"
  ```

### Task 8: Docs and bench checks

**Files:**
- Modify: `README.md` (new section "Updating the iHeater's firmware", after
  "Controls"), `AGENTS.md` (Rules)

- [ ] **Step 1: Docs**
  - **README:**
    - when the card appears (UART output);
    - what the check refuses;
    - the cool-down and the "no heat" rule;
    - that the iHeater's application is erased from entry until the end;
    - Retry;
    - recovery after an interruption;
    - the power-cycle message;
    - that Klipper shows `updating`. The host's Klipper and Moonraker parts
      must be updated first; until then, reports during an update are
      refused, which is harmless.
  - **AGENTS.md Rules,** one bullet: "iHeater flashing: never mass erase or
    erase above page 29, never send write/read-protect or option-byte
    commands, and send `0xB007` at most once per job."
- [ ] **Step 2: Commit**
  ```bash
  git commit -am "docs: describe updating the iHeater over the link"
  ```
- [ ] **Step 3: Bench checks.** These are manual; ask the user before each
  upload to any device, and record the results in the ledger.
  1. **`link_flash.py` on a real iHeater** through the PC's USB-UART (iHeater
     design bench check 1). The user wires it. The Remote's on-device
     flashing waits for this to pass.
  2. On the test C3 (COM23), in UART output on a real iHeater, a normal
     update:
     - with a marker image;
     - with an older image (the fallback warning shows);
     - Klipper on elpis shows `updating`, then `up`.
  3. Refusals that never send `0xB007`, each checked on the C3's Serial log:
     - a pulse-build `.bin`;
     - a truncated file;
     - a file with one byte changed (bad CRC);
     - Manual on (heat wanted);
     - Cancel during cool-down.
  4. Setup save, Wi-Fi save and a Remote OTA during cooling: each returns
     409 (Review Focus 1).
  5. Upload an image, let it flash to `updated`, and upload a second one at
     once. The second stays loaded (Review Focus 5).
  6. Recovery:
     - Interrupt a flash by pulling the Remote's power during Writing.
     - Power the Remote again. Within about 40 s, `/fw` shows "iHeater is in
       its bootloader".
     - Upload and update. It finishes `updated`, or with the power-cycle
       message; power-cycling the iHeater then gives `up`.
     - Also interrupt a flash and power-cycle the iHeater while the Remote
       stays up (design bench check 4). Confirm the Remote detects the ROM
       (the "iHeater is in its bootloader" line) within about 40 s and
       recovers it with an update. This also checks that the 8N1 polls the
       Remote sends before its first probe don't upset the freshly started
       ROM's autobaud. If they do, the fix is to probe before polling at
       boot.
  7. `stackFree` in the dashboard's link details stays above 1 KB after a
     job.
