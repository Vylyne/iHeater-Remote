# iHeater firmware update over the link — design

Status: superseded for the update path by `2026-11-24-linkboot-flash-design.md`.

## Why

The iHeater's UART build can now enter the STM32 ROM bootloader on request
(register 0x0102, `0xB007`; see
`iHeater-Standalone-Firmware/docs/design/2026-11-22-bootloader-entry-design.md`,
"Flasher contract"). Its BOOT button can't be reached without opening the
enclosure. The owner wants the Remote to update the iHeater's firmware over
the same two UART wires, with no hands on the board.

## Goals

- Upload an iHeater `.bin` on the Remote's firmware page, check it, and flash
  it over the link: cool down, enter the ROM, flash, verify, and confirm the
  iHeater talks Modbus again.
- Refuse a damaged, truncated or wrong-build image before anything is sent to
  the iHeater.
- Recover an iHeater left in the ROM by an interrupted flash, from the same
  page.
- Show Klipper that an update is under way.

## Non-goals

- An iHeater image built into the Remote's firmware (the owner considered it
  and chose upload only, for now).
- Flashing from the printer host through Klipper or Moonraker.
- Signed images, rollback, or flashing the pulse or analog builds.
- Any change to iHeater-Standalone-Firmware from this repo. The build marker
  (below) is a contract that repo implements.

## Anti-goals

- Interrupting a print: an update never starts, and a cool-down never
  continues, while heat is wanted.
- Sending `0xB007` more than once per job, or before every check has passed.
- Touching iHeater flash pages 30 and 31 (calibration and the latched fault),
  or sending mass erase, write-protect, read-protect or option-byte commands.

## Constraints

- The iHeater flasher contract, steps 1–8, binds this design. In short:
  - air target 0 and heater at or below 50 °C before `0xB007`;
  - `0xB007` once, then nothing at 8N1;
  - 8E1 and a `0x7F` sync, retried for a few seconds;
  - Extended Erase of pages 0..N−1 only, never above page 29;
  - write, then read back and verify;
  - the first 256 bytes last: bytes 4–255, verified, then word 0 alone,
    verified;
  - Go 0x08000000.
- AN3155 timings as `tools/link_flash.py` uses them: 1 s per normal reply,
  10 s for the erase ACK.
- Pure decisions go in `lib/remote-policy/` with native tests (AGENTS.md).
- The Remote on hestia isn't touched. Bench work uses the test C3 (COM23) and
  elpis.

## Approaches

1. **Flash inside the existing UART task, with a pure flasher in
   `lib/remote-policy` (chosen).** The UART task stays the only code that
   touches UART1. It switches between polling and flashing. A state machine
   shaped like `UartPollPolicy` decides each step, and native tests run whole
   jobs against a fake ROM. The cost is a second mode in the UART task, and
   watchdog feeding during long waits.
2. A separate flash task that borrows UART1 from the poll task. Each task is
   simpler, but UART1 then has two owners and a hand-off where races and a
   leaked driver can hide.
3. Flash from the web request. Least code, but it blocks the web UI and
   Moonraker for up to 30 minutes of cool-down, and it can't show progress.

## Build marker (contract for iHeater-Standalone-Firmware)

A 32-byte record at image offset **0xC0**, right after the STM32F042 vector
table (16 + 32 entries). It's little-endian, and every build that should be
flashable over the link carries it.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0x00 | 4 | `magic` | `0x57464869` (the bytes `i H F W`) |
| 0x04 | 2 | `record_version` | 1 |
| 0x06 | 2 | `record_size` | 32 |
| 0x08 | 2 | `build_type` | 1 = UART build; other values aren't flashable over the link |
| 0x0A | 2 | `protocol_version` | the link protocol, as input register 0x00 |
| 0x0C | 2 | `fw_version` | major << 8 \| minor, as input register 0x01 |
| 0x0E | 2 | `fw_patch` | as input register 0x02 |
| 0x10 | 4 | `image_length` | bytes the CRC covers: the whole `.bin` |
| 0x14 | 4 | `image_crc32` | CRC-32 (IEEE, as `zlib.crc32`) of bytes 0..`image_length`−1, computed with this field as 0 |
| 0x18 | 4 | `git_sha` | the first 8 hex digits of the iHeater's `HEAD`, as a number |
| 0x1C | 2 | `flags` | bit 0: tracked files differed from `HEAD`; bit 1: no git info (`git_sha` is 0) |
| 0x1E | 2 | reserved | 0 |

A post-build step fills in `image_length`, `git_sha`, `flags` and, last,
`image_crc32`. The CRC therefore covers the SHA and flags. The iHeater side
is specified in
`iHeater-Standalone-Firmware/docs/design/2026-11-22-build-marker-design.md`.

The page shows the version as `v<major>.<minor>.<patch>-<7 hex digits of
git_sha>`, with `-dirty` added when bit 0 is set, and with no suffix when
bit 1 is set: for example, `v0.5.0-1a2b3c4`.

## Image check (pure, `lib/remote-policy`)

In order, refusing at the first failure, with a reason the page shows:

1. The size is 8 to 30,720 B.
2. The stack pointer is in 0x20000000..0x20001800, and the reset vector is in
   0x08000000..0x080077FF. These match `link_flash.py`'s `check_image`.
3. **If the magic is present at 0xC0:**
   - `record_version` is 1 and `record_size` is 32;
   - `build_type` is 1;
   - `image_length` equals the file size;
   - the CRC matches.

   The page shows "UART build v0.5.0-1a2b3c4, protocol 1, checksum OK".
4. **If the magic is absent (older images):** the vector-table fallback.
   Vector 43 (USART1, offset 0xAC) must differ from the most common value in
   vectors 16..47 (the shared default handler). On today's builds this holds
   only for `iheater_rev1_1_uart`. The page warns: "This image has no checksum
   or version; it looks like a UART build from its vector table."
5. **A marker image with a protocol version the Remote doesn't support** is
   refused. A fallback image has no version to check.

The magic is only looked for when the file is at least 0xE0 B. An upload
over 30,720 B is refused while it streams. Uploads are refused while a job
runs.

## Starting an update

The page allows **Update iHeater** only when all of these hold:

- the output is UART;
- an image passed the check;
- no heat is wanted: Manual mode is Off, or the Klipper target is 0;
- either the link is `up` (a normal update) or the ROM has been detected (a
  recovery).

## The job

The UART task runs it. Each phase is shown on the page and logged on Serial.

1. **Cooling**, normal update only.
   - Read the air target, and write 0 only if it isn't 0.
   - Poll `heater_temp` every 5 s until it reads 50 °C or less. Give up after
     30 minutes.
   - Stop the job if the heater reading is invalid.
   - **Cancel** is allowed here. If heat becomes wanted (Manual on, or a
     Klipper target above 0), the job stops with "heat requested".
   - The application is still intact when the job stops in this phase.
2. **Entering**, normal update only.
   - Write `0xB007` to 0x0102 once, with no retry.
   - An exception reply means refused: stop, and the application is
     untouched.
   - No usable reply: go on to the sync anyway, and note it, as
     `link_flash.py` does. If the sync then fails, switch back to 8N1 and
     poll once. If Modbus answers, the application is still running: stop
     with "entry not confirmed; the iHeater is still running its firmware".
     If it doesn't, it's a phase-3 failure as below.
   - From here on there's no Cancel. The page says the iHeater's application
     is erased.
3. **Syncing.**
   - Switch UART1 to 8E1 and wait 100 ms.
   - Send `0x7F` up to 20 times, 100 ms apart. ACK means ready. NACK means
     already synced; drain the input and go on.
   - Then Get, to pick Extended or legacy Erase, and Get ID. A product ID
     other than 0x445 is a warning, as in `link_flash.py`.
4. **Erasing** pages 0..N−1, where N = ceil(size / 1024). A page above 29 is
   refused in policy, before the command is built.
5. **Writing and verifying,** in 256-byte writes, each read back and
   compared:
   - every chunk after the first;
   - then bytes 4–255;
   - then word 0.
6. **Go** 0x08000000.
7. **Restarting.**
   - Switch UART1 back to 8N1 and resume polling.
   - Done when Modbus answers within 15 s.
   - If Modbus stays silent but a ROM probe answers, the page asks the owner
     to power-cycle the iHeater, rather than flash again.

**A failure from phase 3 on** leaves the iHeater in the ROM, with the heater
and fan off. The page names the step that failed and offers **Retry**, which
starts again at phase 3 with the same image. The image buffer is kept for
this.

**While a job runs**, `apply()` targets are held, not sent. After the job,
normal polling resumes and sends the desired target as usual.

## Recovery detection

In UART output, once the link has had no OK reply for 10 s, the UART task
probes for the ROM every 30 s:

1. switch to 8E1;
2. send `0x7F`, and wait 100 ms for ACK or NACK;
3. switch back to 8N1.

If the ROM answers, the page shows "iHeater is in its bootloader: upload
firmware to recover", and an update skips phases 1 and 2. A stray `0x7F`
seen by an iHeater application is dropped by its Modbus framing.

## Image storage

The upload streams into a 30,720 B heap buffer, allocated when an upload
starts. It's freed after a successful flash, when another file replaces it,
or when the owner discards it. A Remote restart loses it; recovery then needs
a new upload.

## Web UI

The `/fw` page gets an "iHeater firmware" card, shown only in UART output.
It holds:

- the running iHeater version and protocol;
- the file picker;
- the parsed image line, or the reason it was refused;
- **Update iHeater**, **Cancel** (cooling only) and **Retry** (after a
  failure);
- the phase, with progress for writing and verifying.

The endpoints follow the existing style:

- `POST /api/iheater/image` (multipart, field `firmware`) returns the check
  result;
- `POST /api/iheater/update`, `/api/iheater/cancel` and `/api/iheater/retry`;
- `POST /api/iheater/image/discard`.

Job state goes in the existing status JSON and WebSocket push, as an
`iheaterUpdate` object: the phase, progress, error and whether the ROM was
detected.

## Klipper and Moonraker

A new link state, `updating`, is reported while a job runs (phases 1–7). The
Klipper module (`_LINKS`) and the Moonraker component (`_LINKS`) accept it,
with tests. On the Remote, `LinkStatus` and its report name gain the same state.
Klipper already treats any link other than `up` as not linked,
and doesn't shut down on link loss, so this only makes the reason visible.

## Testing

- **Native** (`test/test_remote_policy/`):
  - the image check: every refusal reason, a marker image with a good and a
    bad CRC, the fallback on synthesised vector tables, and the 30,720 B
    limit;
  - the CRC-32 against `zlib.crc32` vectors;
  - the job state machine against a fake ROM:
    - the cool-down writes the target only when it's non-zero;
    - Cancel, and "heat requested";
    - `0xB007` sent once, and an exception stops the job;
    - no reply still leads to the sync;
    - sync ACK, NACK and timeout;
    - Extended and legacy Erase, and page 30 refused;
    - write order, with word 0 last;
    - a verify mismatch stops the job;
    - Go;
    - Retry starts at the sync;
    - recovery skips cooling and entry.
- **Python:** the `updating` link state in the Klipper and Moonraker tests.
- **Bench:**
  1. **First, `link_flash.py`** from the PC through the USB-UART on a real
     iHeater's link wires (iHeater design bench check 1). The Remote's
     on-device flashing waits for this.
  2. A normal update from the Remote, with a marker image and with an older
     image (fallback).
  3. Refusals that never send `0xB007`: a pulse-build image, a truncated
     file, a bad CRC, heat wanted, and Cancel during cool-down.
  4. Recovery: interrupt a flash, power-cycle the iHeater, and check that the
     Remote detects the ROM and recovers it.
  5. Klipper on elpis shows `updating`, then `up`.

## Left to the builder

- How the job state machine is split: for example, the image check, the
  AN3155 command builder and the job sequencer as separate units.
- The watchdog: feed it in slices during long waits (the erase ACK, the
  cool-down).
- How UART1 switches parity at run time (`uart_set_parity` or reconfiguring).
- Exact page wording, apart from the warnings quoted above.

## Docs

- README: a "Updating the iHeater's firmware" section.
- AGENTS.md: the trust boundary. Never mass erase, never a page above 29, and
  `0xB007` once per job.
- `klipper/README.md`: the `updating` link state.
