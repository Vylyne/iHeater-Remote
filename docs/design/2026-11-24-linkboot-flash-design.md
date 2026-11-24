# iHeater update through the link bootloader — design

Status: approved (2026-11-24).

Supersedes the update path in `2026-11-22-iheater-link-flash-design.md`.
The contract this builds against is iHeater-Standalone-Firmware
`docs/design/2026-11-24-link-bootloader-design.md` ("Flasher contract"), and
the reference flasher is that repo's `tools/linkboot_flash.py`.

## Why

The Remote never synced with the STM32 ROM bootloader, while the PC did with
retries. The iHeater now carries its own bootloader in pages 0–1. It speaks
the same Modbus RTU at 115200 8N1 as the app, so the Remote can update the
iHeater over the link it already polls every 200 ms, with no parity switch,
no sync byte and no autobaud.

## Goals

- "Update iHeater" on the Remote flashes a `build_type` 2 app image through
  the link bootloader, following the flasher contract step for step.
- Every lost or damaged reply is survived by resending the same frame.
- A board found in the bootloader (input 0x00 reads `0x8001`) can be flashed
  from the Remote's page, without 0xB007.
- Heater safety is unchanged: cool-down first, target 0, and the bootloader
  holds the heater and fan low.

## Non-goals

- Installing the bootloader. The first install of the combined image is
  `link_flash.py --enter` over the ROM, or an ST-Link.
- The ROM path. It is removed from the Remote (owner's decision, below).
- Any change to the Klipper module or the Moonraker component.

## Anti-goals

- Sending 0xB007 to a board without the link bootloader. On that firmware it
  wipes the app and leaves the board in the ROM.
- Any ROM (AN3155) byte on the link.

## Decisions (owner, 2026-11-24)

1. **Drop the ROM path.** The 8E1 switch, the ROM probe, the AN3155 job and
   `RomProtocol` go. Branch `fix/flash-retries` (ROM retries) stays unmerged.
2. **Link state:** while the iHeater is in the bootloader, the Remote reports
   `updating` to Klipper and Moonraker. Its own page shows the details.
3. **0xB007 may be resent** when a try gets no usable reply, up to 5 tries
   in all, as `linkboot_flash.py` does. Only after input 0x16 reads
   non-zero. Entry no longer erases anything. AGENTS.md's flashing rule
   changes to match.

## Approaches

1. **Rewrite the flash job as a Modbus sequencer (chosen).** It stays in
   `lib/remote-policy`, pure and tested over a fake link. It reuses the
   Remote's frame code, plus an FC10 builder. The UART task runs it as it
   runs the ROM job today.
2. Keep the ROM job beside it and pick a path by `build_type`. Rejected
   (decision 1): twice the code, and one path has never worked from the
   Remote.

## The job

Phases: Cooling, Entering, Erasing, Writing, Starting, Restarting, Finished.
Every exchange is tried up to 5 times with the identical frame when the reply
is missing or damaged. A Modbus exception is never retried; each step says
what it means.

1. **Check the image** before any frame: build record present, `build_type`
   2, length at most 28,672 B, CRC-32 right. Pad to a multiple of 128 bytes
   with 0xFF.
2. **Read input 0x00.**
   - It reads `0x8001`: the board is in the bootloader. Skip to step 4.
   - It reads 1: the app. Read input 0x16. If it's refused or reads 0, stop
     with "no link bootloader" and never send 0xB007.
   - Anything else: stop.
3. **Cooling, then Entering.**
   - Cool down as today: write target 0 if needed, wait for the heater to
     read 50 °C or less, with the same timeout, poll period and cancel.
   - Write 0xB007 to 0x0102.
     - An exception to the first try is a refusal: the board is still in its
       app.
     - An exception after an unanswered try means a resend reached the
       bootloader. Move on.
   - Poll input 0x00 every 100 ms, for up to 3 s, until it reads `0x8001`.
     If it never does, stop.
   - Cancel works up to here. Nothing has been erased yet, and the
     bootloader returns to the app after 30 s with no erase.
4. **Erasing.** Write 1 to 0x0300, waiting up to 2 s for the reply. An
   exception stops the job, and the detail carries `last_error` (input 0x05).
5. **Writing.** One FC10 write per 128-byte chunk to 0x0310: the chunk index,
   then 64 big-endian registers.
   - The bootloader accepts a resent chunk that already landed.
   - An exception 0x04 with `last_error` 3 (not blank) restarts from step 4.
   - Any other exception stops the job.
   - Progress is chunks written over the total.
6. **Starting.** Write 2 to 0x0300, waiting up to 2 s.
   - With no usable reply, go on to step 7.
   - An exception 0x04 means the bootloader refused the image: read
     `app_check` (input 0x04) and restart from step 4.
   - Any other exception means a resend landed in the running app. Go on to
     step 7.
7. **Restarting.** Poll input 0x00 for up to 5 s until it reads 1. Done.

One restart is shared between steps 5 and 6. A second refusal of either kind
stops the job.

## The link while not flashing

- The normal poll reads inputs from 0x00 as now. The bootloader answers any
  read inside 0x00–0x1F, with 0 at unused addresses.
- When input 0x00 reads `0x8001`, the Remote knows it's the bootloader. It
  keeps `why` (0x03), `app_check` (0x04) and `timeout_s` (0x06). It sends
  no target, clear or settings requests, reports `updating`, and the page
  says the iHeater is in its bootloader.
  - With `why` 2 (no valid app), the page offers **Update iHeater**. That
    starts at step 4.
  - With `why` 1, the bootloader goes back to the app by itself within 30 s.
- **Retry** after a failed job runs the whole job again. Step 2 finds out
  where the board is.

## Web UI and docs

- Drop the ROM wording ("application erased", "power-cycle"). Cancel lasts
  until the erase. A failure after the erase leaves the iHeater in its
  bootloader, heater and fan off, waiting for a flash.
- The image upload refuses anything but `build_type` 2, and the message
  names the type it got. The retained buffer shrinks to 28,672 B.
- The `retries` count stays, now counting resent frames.
- AGENTS.md: replace the iHeater flashing rule with the new one. Flash
  through the link bootloader only; never send ROM commands; send 0xB007
  only after input 0x16 reads non-zero; resend only an unanswered try, up to
  5 tries; never send a chunk index above 223.
- Mark the 2026-11-22 flash design and plan superseded for the update path.

## Tests

The fake ROM in `test/test_flash_job/` gives way to a fake link bootloader
over the fake app. Cases:

- lost and damaged replies on every step;
- a resent chunk that already landed;
- not blank, then one restart;
- boot refused, then one restart; a second refusal stops;
- a lost boot reply, then the app found by polling;
- old firmware (0x16 refused or 0): no 0xB007;
- a board already in the bootloader: no 0xB007;
- cancel before the erase; heat requested during cooling;
- nothing above chunk 223, and no 8E1 or ROM byte ever sent.

Plus image checks for `build_type` 2, and the poll policy's bootloader
detection.

## Left to the builder

- Outcome and phase names, and how FC10 is carried through `FlashLink`.
- Whether the bootloader view adds new JSON fields or reuses existing ones.
