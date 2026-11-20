# iHeater Remote UART client — design

Status: for approval · 2026-11-20

## Intent

Make the Remote the Modbus RTU master for the iHeater's two-way UART build
(`iheater_rev1_1_uart`), so the Remote knows what the iHeater is really
doing, not only what it was told. The Remote sets the chamber target over
the link, shows the iHeater's live state in its web UI, and clears faults
on its own when the iHeater says it is safe.

Why: the pulse output is one-way. The Remote cannot see temperatures,
PWM, fan, or faults, and every iHeater fault needs a trip to the printer
and a MODE press at power-on. The user does not expect one-way control to
be preferred again; pulse stays for iHeaters still on the pulse build.

The contract this builds against is the firmware side's design,
`iHeater-Standalone-Firmware/docs/design/2026-11-19-uart-link-design.md`
(register map, hold, faults, "Changes after approval"), and its user guide,
`docs/en/README.md`, "Working with iHeater Remote (UART build)". This
document does not repeat the register map.

## Goals

1. A Setup option, **Output: Pulse or UART**, in the same firmware image.
   Pulse behaves exactly as today on GPIO3.
2. In UART mode the Remote polls the iHeater continuously, writes the
   target, and keeps the link alive whatever else the Remote is doing.
3. The target range comes from the iHeater (`min_target` 0x0202,
   `max_air_target` 0x0203), narrowed by the user's Setup range.
4. Faults clear automatically when the iHeater reports them clearable.
5. A Remote reboot does not drop heat that should continue (see "Reboot
   recovery").
6. The web UI shows the iHeater's live state.
7. The Remote's policy tests run on the PC, including a test of the master
   against the iHeater's own Modbus slave code.

## Non-goals

- Reporting iHeater data into Klipper (`heater_virtual` temperature and
  power, an `iheater.report` Moonraker plugin). That is the next project.
- Writing iHeater settings (read-only in protocol v1).
- Reflashing the iHeater over the link, or switching the iHeater between
  its pulse and UART builds. Both are flashes on the iHeater; noted as
  future work.
- Changing the pulse output or its behaviour.

## Anti-goals

- **Polling stops while the Remote is busy.** Any gap over 3 s puts the
  iHeater into link hold, and 63 s drops heat mid-print. Wi-Fi joins,
  Moonraker reconnects, TLS handshakes, web requests and settings saves
  must never delay the poller.
- **A Remote reboot turns off heat the printer still wants.**
- **The Remote retries a clear that cannot work** and burns the iHeater's
  three clears for nothing.
- **A heater fault the iHeater refuses to clear remotely** (0x02, 0x06,
  0xFF) is ever attempted. The Remote clears only when
  `fault_clearable_now` reads 1, which already excludes these.

## Constraints

- ESP32-C3 Super Mini, Arduino-ESP32 2.0.17, single core.
- **Pins:** UART1, TX on GPIO4 to the iHeater's D+ (its RX), RX on GPIO5
  from D− (its TX). The user's 5-pin JST carries 5V, GND, 3V3, GPIO4,
  GPIO5. **The 3V3 wire stays unconnected at the iHeater end**: USB-C has
  no 3.3 V line and the iHeater makes its own from VBUS. GPIO20/21 are not
  used: they are UART0, where the ROM prints at every boot. If the wiring
  turns out reversed, the pins swap in firmware (build flags), no
  resoldering.
- **Power:** the Remote's 5V powers the iHeater. Losing Remote power also
  powers the iHeater down, so only Remote *software* resets (update,
  setup save, crash, watchdog) need recovery.
- **Link:** 115200 8N1, slave 1, FC 03/04/06/10. Poll the input block at
  least every 250 ms. Reply timeout at least 200 ms (a clear reply waits
  for a flash erase). About 5 s of silence after every iHeater boot,
  including the one after a clear. Clear only with FC 06. Re-read after
  any exception.
- Pure decisions go in `lib/remote-policy/` with tests (AGENTS.md).
  Policy files use CRLF.

## Approaches

- **A. Own Modbus master on the iHeater's own core (chosen).** Copy the
  iHeater's `Lib/ModbusRtu/modbus_rtu.{c,h}` (pure C slave, CRC, exception
  codes, frame acceptance rule) into the Remote unchanged. Write a small
  master in `lib/remote-policy/`: request builders and a reply parser for
  the four function codes, plus a poll state machine for every decision.
  A thin task moves bytes on UART1. Tests run the master against the
  copied slave with fake registers, so the two sides cannot drift.
- B. eModbus (`ModbusClientRTU`). Mature and asynchronous, but it adds its
  own worker task and queue under ours, so "polling never stops" would
  rest on its internals, and the clear and recovery logic is still ours.
  Rejected.
- C. ModbusMaster. Small, but blocking, loose about timeouts and exception
  codes (2 s default), and one transaction at a time over `Stream`.
  Rejected.

Picked by the user after learning the iHeater side is hand-written too.

## Design

### Output selection

- New setting **Output**: Pulse (default, today's behaviour) or UART,
  stored in the Remote's NVS namespace and chosen on the Setup page.
- Applied at boot only. Saving a change restarts the Remote (the Setup
  page already has a restart path).
- Pulse and UART sit behind one output interface (`apply(command)`,
  `getLastCommand()`, enabled), so the rest of the app does not care which
  is active. Only the active one is started: in UART mode the RMT task
  never runs and GPIO3 stays an input; in pulse mode UART1 is never
  installed.

### Poller task

- A FreeRTOS task, created in `setup()` before Wi-Fi is configured, at a
  priority above Arduino's `loopTask` (1) and below the Wi-Fi stack.
- It owns UART1 through the ESP-IDF UART driver and does nothing else.
  It never takes a lock that the main loop can hold across network or
  flash work; it shares state with the main loop through one small
  snapshot and one small command, each guarded by a short critical
  section (copy in, copy out).
- From boot the poller reads, but writes nothing until the app has
  loaded its control source (after the Wi-Fi wait) and said what it
  wants.
- Cycle every 200 ms (inside the 250 ms the contract asks for): one FC 04
  read of input registers 0x0000–0x0015 (22 registers, about 5 ms on the
  wire), then any writes the policy asks for.
- Reply timeout 200 ms. A timeout, bad CRC or wrong length counts as a
  missed reply; the next cycle starts on schedule regardless. The poller
  never backs off.
- The task is registered with the ESP task watchdog. If it ever stalls
  long enough to trip it, the Remote resets and recovers (see "Reboot
  recovery"); the iHeater's 60 s hold covers the gap.

### Pure master and policy (`lib/remote-policy/`)

Pure, no Arduino, covered by tests:

- **Frames:** build FC 03, 04 and 06 requests; parse a reply into one of
  ok (with register values), exception (with its code), or invalid
  (short, bad CRC, wrong address, wrong function, wrong count). The CRC
  and exception constants come from the copied `modbus_rtu.h`.
- **Snapshot decode:** the 22 input registers into named fields, signed
  tenths to °C, `0x8000` to "no reading", uptime from its two words.
- **Poll state machine:** given the latest snapshot, the time, and what
  the app wants, return the next action: read inputs, read settings,
  write target, write clear, or nothing. It decides:
  - **Settings read:** FC 03 of 0x0200–0x0209 on first contact and after
    every iHeater reboot (uptime went backwards, or link re-established).
  - **Target write:** FC 06 to 0x0100 when the desired target (in tenths)
    differs from `air_target` and the iHeater is not faulted. This also
    re-sends the target after any iHeater restart (target 0 after boot
    and after a clear). Exception 0x03 or 0x02 on a target write: no
    retry until the desired target or the iHeater's state changes.
  - **Auto-clear:** FC 06 of `active_fault` to 0x0101 when state is 4,
    `fault_clearable_now` is 1, `remote_clears_left` > 0, and no clear is
    in flight. Then expect a reply and about 5 s of silence. Success is
    confirmed after the restart: `last_clear_result` = 1 and
    `remote_clears_left` one lower. A lost reply followed by that same
    evidence also counts as success.
    - Exception 0x03 (refused): re-read; the next decision uses fresh
      registers.
    - Exception 0x04 (erase failed; the clear is spent and the iHeater
      does not restart): **stop auto-clearing** that fault until it
      changes or the Remote restarts, and show it in the UI.
    - A lost reply where `remote_clears_left` went down but uptime kept
      counting (no restart) is the same erase failure, since the iHeater
      leaves `last_clear_result` at 0 in that case.
  - **Protocol check:** if `proto_version` is not 1, poll and display
    only; never write.
- **Link status** for the UI: waiting (no reply yet), up, restarting
  (silence expected after a boot or clear), no reply (silence not
  expected, after 1 s of missed replies).
- **Effective range:** Setup range intersected with the iHeater's
  `[min_target, max_air_target]`. Until the settings block has been read,
  only a target of 0 is written.
- **Manual resume record** validity (see below).

### Range

- UART mode clamps targets to the effective range above. Targets are
  converted to tenths with rounding. A request below `min_target` clamps
  up to it, as today.
- Setup's min/max fields keep working as a narrowing of the iHeater's
  range; the pulse range constants stay as they are for pulse mode.
- **Never above the user's own maximum.** If the Setup range and the
  iHeater's range do not overlap, the effective range is empty: the
  target is always 0 and the UI says why. Once the iHeater's range has
  been read, the Setup page shows it and refuses a Setup range outside it.

### Reboot recovery

Today `setup()` applies mode 0 (off) at boot. In UART mode that would
write target 0 before anything else, so boot changes:

- **Klipper source:** the Remote does not write a target until it has
  read `air_target_cmd` back. It adopts that value as a printer target
  (so the existing 10-minute link-loss rule still covers it) and keeps
  it until Moonraker delivers Klipper's target or reports unpaired (which
  already sets 0). If Klipper never returns, the 10-minute rule turns it
  off.
- **Manual source (both outputs):** the manual mode and the timer's
  remaining seconds are kept in RTC memory that survives software resets
  but not power loss (`RTC_NOINIT_ATTR`), with a magic value and a
  checksum, refreshed on every change and once a second while a timer
  runs. At boot, if the record is valid and the reset was not a power-on
  or brownout reset, the Remote resumes that mode with the remaining
  time (the reboot adds a few seconds). Otherwise it stops, as today. A
  mode with no timer resumes with no timer. No flash is written.
- **Klipper source on pulse:** unchanged (off until Klipper sends).

**Planned restarts (user decision).** UART is the enhanced build: in UART
mode a firmware update or a Setup save resumes the current operation.

- **Update (web OTA), UART mode:** heat is no longer set to 0 before the
  image is written. The poller keeps running during the upload; the
  restart is covered by the recovery above (Klipper: read-back; Manual:
  RTC record). The firmware page's "Heating is stopped before the update
  is written" becomes UART-aware.
- **Setup save, UART mode:** a save that leaves the control source, the
  printer binding (endpoint, token) and the Output setting unchanged does
  not touch the target, restart or not. The existing rules that set 0 on
  a source or binding change stay.
- **Pulse mode:** unchanged. Heat stops before an update and on a Manual
  Setup save, as today; the pulse iHeater has no hold window.
- **Switching Output** (Pulse to UART or back) always stops heat and
  clears the resume record: it needs a different cable and iHeater build.

### Web UI

- Status JSON gains an `iheater` object in UART mode: link status, state,
  air and heater temperatures, target in effect, heater setpoint, PWM,
  fan, hold seconds left, active fault (code and name), clearable, clears
  left, last clear result, the iHeater's firmware and protocol versions,
  and its range. The JSON document grows to fit.
- Home page, UART mode, an "iHeater" card next to the command card:

  ```
  iHeater                      link: up
  Air      52.4 °C → 60.0 °C   Heater 118.2 °C (set 125)
  Output   43 %   Fan on       State heating
  Fault    none                Clears left 3
  ```

  On a fault: the code and name, "clearing when safe" or the reason it
  won't clear (not clearable remotely, no clears left, erase failed).
- In pulse mode the card is hidden and the page is as today. The footer
  line saying temperatures are not available shows only in pulse mode.
- Setup page: the Output choice, with a note that switching restarts the
  Remote and that the iHeater must run the matching build.

### PC tests

- A `native` PlatformIO environment, as the iHeater repo already has,
  runs `test_remote_policy` on the PC with MinGW (`~/scoop/apps/mingw`).
  The test file drops its Arduino include.
- New tests cover the frames, decode, poll state machine, range and
  resume record, and a loopback test: the master's requests go through
  the copied `modbus_handle_frame()` with fake register handlers, and the
  replies come back through the master's parser.
- AGENTS.md: the policy test gate becomes `pio test -e native` (with
  MinGW on `PATH`), replacing the build-only command and the "no host
  compiler" note.

### Copied code

- `lib/modbus-rtu/` holds `modbus_rtu.c` and `modbus_rtu.h` copied
  unchanged from iHeater-Standalone-Firmware `Lib/ModbusRtu/` at commit
  `d1da01a`, with a README naming the source and commit. Changes go to
  the iHeater repo first, then are copied again.

## Calls I made (check these)

1. Poll every 200 ms with a 200 ms reply timeout, never backing off.
2. Output change applies at boot only, with a restart on save.
3. After an exception 0x04 on a clear, stop auto-clearing that fault
   until it changes or the Remote restarts.
4. Until the iHeater's range has been read, only target 0 is written.
5. A protocol version other than 1 means read and display only.
6. Poller on the ESP task watchdog, so a stall resets the Remote.
7. Manual resume also applies in pulse mode (same record, no cost).
8. The policy tests move to the PC; the build-only gate is dropped.
9. An empty effective range means always off, never heat above the
   user's Setup maximum.
10. A lost clear reply with clears left down and no restart counts as an
    erase failure.

## Left to the builder

- Task priority and stack size, and the exact watchdog timeout.
- UART driver details (buffer sizes, RX timeout) and the inter-frame gap.
- File names and how the output interface is shaped.
- The UI card's exact markup and wording.
- Bench check that a web OTA update (flash erases with the cache off)
  never stalls the poller for more than about 1 s. This matters more now
  that heat stays on through an update in UART mode.
