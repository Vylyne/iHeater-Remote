# Klipper chamber heater for iHeater Remote — design

Status: draft for approval · 2026-11-18

## Intent

Make the iHeater behave like a native Klipper chamber heater when driven by
iHeater Remote over Moonraker:

- Slicer `M141` / `M191` work.
- Mainsail shows a "chamber" heater row with a settable target, like any
  other heater.
- `TURN_OFF_HEATERS` and the Mainsail heater off button turn it off.
- The iHeater keeps regulating from its own probe; the Remote only tells it
  the chamber target.

Why: the Remote's current Moonraker mode ignores `VIRTUAL_CHAMBER` and only
heats from a bed-setpoint threshold, so `M141`/`M191` do nothing.

## Goals

1. A Klipper extras module, `[heater_virtual chamber]`, that registers as a
   Klipper heater (target, temperature from an existing sensor, power).
2. Remote firmware follows the Klipper chamber target.
3. Limits on the chamber *target* (not the element): Klipper `max_target`,
   and a Remote "iHeater target range" that records the iHeater firmware's
   accepted range (default 45–85, recommend Max ≤ 80).
4. Fail safe: a Klipper shutdown/crash or Moonraker loss turns heating off.

## Non-goals

- `start_offset`, `delta_temp`, `min_heater_temp`, `air_*_delta`, and the
  `_iHEATER_CONTROL` loop from the native (USB-MCU) macros. Start-offset
  warm-up lives in the user's `START_PRINT`; the rest only applies when
  Klipper drives the heating element directly.
- Limiting the heating *element* temperature from Klipper or the Remote.
  The pulse protocol carries only a chamber target (45–80 °C, 1 °C steps) or
  off. In Remote mode the iHeater's STM32 (iHeater-Standalone-Firmware,
  pulse build, v0.5.0) runs a cascaded loop: an air PID (its own probe)
  produces an element setpoint capped at `MAX_TEMP`, and an inner element PID
  drives PWM. Stock v0.5.0 `Core/Inc/config.h`: `MAX_TEMP 110` (element
  setpoint ceiling; fault-reset at `MAX_TEMP + 10` = 120 °C),
  `MAX_AIR_TEMP 90` (pulse targets accepted 45..90; air fault at 90 °C).
  `MAX_TEMP` is the only real element/housing cap; the README documents
  rebuilding with e.g. `MAX_TEMP 100` for ABS housings. No Klipper or Remote
  option pretends to enforce it.
- Changes to idryer-core (pinned upstream dependency).
- Fluidd/KlipperScreen-specific work (expected to work via
  `available_heaters`, not verified).

## Anti-goals

- The Remote heating while Klipper is shut down, restarting, or unreachable.
- Klipper's `verify_heater` able to abort a print because a chamber warms
  slowly.
- The bed-threshold rule re-enabling heat after `M141 S0`.

## Approaches considered

- **A. Klipper module + mirror into `VIRTUAL_CHAMBER` (chosen).** Native
  heater UX; no spare pin; idryer-core already subscribes to
  `gcode_macro VIRTUAL_CHAMBER`, so the wire format stays the same.
- B. `[heater_generic]` on a spare MCU pin. No Python, but wastes a pin,
  moves `chamber_hexa` into the heater section, keeps `verify_heater` risk,
  and still needs a polling macro to mirror the target. Rejected.
- C. Remote subscribes to the heater object directly. Cleaner wire, but
  requires forking idryer-core. Rejected.

## Design

### Klipper side (new: `klipper/heater_virtual.py`, `klipper/iheater_remote.cfg`)

Config:

```ini
[heater_virtual chamber]
sensor: temperature_sensor chamber_hexa   # required; existing sensor object
# max_target: 80                          # optional chamber-target cap, 45..90
# mirror_macro: VIRTUAL_CHAMBER           # optional, default VIRTUAL_CHAMBER
```

No `min_temp`/`max_temp`: in Klipper those are sensor-range shutdown
thresholds, and the referenced sensor already enforces its own.

The section name must start with `heater_`: Mainsail only offers a target box
for objects named `extruder*` or `heater_*`
(`TemperaturePanelListItem.vue: command`).

Behaviour:

- Registers with `heaters` so it appears in `available_heaters` and
  `available_sensors`, under short name `chamber`, so
  `SET_HEATER_TEMPERATURE HEATER=chamber TARGET=n` and
  `TEMPERATURE_WAIT SENSOR="heater_virtual chamber" MINIMUM=n` work and
  `TURN_OFF_HEATERS` zeroes it.
- No `verify_heater`, no PWM, no MCU pin.
- `get_status`: `temperature` (from the sensor), `target`, `power`
  (1.0 when target > 0, else 0.0).
- Target validation: 0..`max_target`; above it is a gcode error.
- Mirroring: writes `target`, `temperature`, and `has_sensor=1` into the
  `gcode_macro VIRTUAL_CHAMBER` object's variables directly in Python (same
  effect as `SET_GCODE_VARIABLE`, but safe outside gcode context), on every
  target change and on a ~1 s timer for temperature. Missing macro is a
  config error with a hint.
- On `klippy:shutdown`: target → 0 and mirrored immediately.

Shipped macros (`iheater_remote.cfg`), replacing the user's current
`M141`/`M191`/`CLEAR_VIRTUAL_CHAMBER`:

- `[gcode_macro VIRTUAL_CHAMBER]` with the existing variables.
- `M141 S<t>` → `SET_HEATER_TEMPERATURE HEATER=chamber TARGET=<t>`.
- `M191 S<t>` → `M141 S<t>`, then if `t > 0`:
  `TEMPERATURE_WAIT SENSOR="heater_virtual chamber" MINIMUM=<t>`.

### Remote firmware (`src/standalone/StandaloneApp.cpp`)

- **Target source.** In Moonraker mode, when `VIRTUAL_CHAMBER` is available,
  its `target` is authoritative (0 → off). The bed-threshold rule is used only
  when `VIRTUAL_CHAMBER` is not available. Bambu mode unchanged.
- **iHeater target range.** New Setup card "iHeater target range" with
  *Min target* (default 45) and *Max target* (default 85), stored in NVS.
  These describe what the iHeater's own firmware accepts
  (`PULSE_MIN_TARGET`, `MAX_AIR_TEMP` in its `config.h`) and double as the
  Remote's limits. The card states plainly: *this does not change any setting
  on the iHeater; enter the values your iHeater firmware was built with.*
  It also warns: *the iHeater faults if the air reaches its MAX_AIR_TEMP, so
  a target at the top of the range will trip it; set Max lower for headroom.*
  Every non-off target the Remote sends (Klipper, bed-rule auto target,
  manual modes) is clamped to `[min, max]`. Validation: `11 ≤ min ≤ max ≤ 90`
  (codes 0 and 10 mean off on the wire). The auto target must lie within the
  range. The RMT adapter's fixed 45–80 window is widened for the Remote build
  so the policy, not the adapter, does the clamping.
- **Low targets.** Klipper targets between 0 and *min* are sent as *min*.
  *(Builder's call; see below.)*
- **Staleness watchdog.** Moonraker drops every client subscription when
  Klippy disconnects (`klippy_connection.py: _on_connection_closed`), and
  idryer-core never resubscribes, so after any Klipper restart the Remote
  would go deaf. If Moonraker mode is active, the client reports Connected,
  and no status update has arrived for 30 s: send off, then restart the
  Moonraker client (`setActive(None)` → `setActive(Moonraker)`), which
  resubscribes and re-applies the current Klipper target. A spurious trip on
  an idle printer is harmless (brief reconnect, target was already 0).
- Dashboard/status JSON: expose the target range and where the current
  target came from.

### Docs

- README: Klipper setup section (copy/symlink module into
  `klippy/extras`, include `iheater_remote.cfg`, remove old
  `M141`/`M191`), the target cap, the element cap living in the iHeater's
  `config.h` `MAX_TEMP`, and the `START_PRINT` start-offset pattern:
  `M191 S{target - offset}` then `M141 S{target}`.

## Constraints

- Assumed stock Klipper (unconfirmed; Kalico would need a check). Heaters API (`PrinterHeaters`,
  `lookup_heater`, `set_temperature`, `register_sensor`,
  `turn_off_all_heaters`) per current Klipper `klippy/extras/heaters.py`.
- Pulse protocol: 45–80 °C, 1 °C steps, or off; STM32 drops to off after
  ≥3 s without valid frames.
- `pio run -e esp32c3-super-mini-remote` must build; flash currently 84 %.

## Calls I made (check these)

1. Section name `heater_virtual` (forced by Mainsail's `heater_` prefix rule).
2. `sensor` is required; `M191` would otherwise wait forever.
3. The Remote's target range applies to manual modes too.
4. Targets below *min* run at *min* rather than erroring (a slicer
   `M141 S40` shouldn't abort a print).
4a. Klipper `max_target` stays default 80 (allowed 45–90); the Remote range
   is the second line of defence.
5. Watchdog 30 s; relies on temperature updates flowing about every second
   while Klipper is alive.

## Left to the builder

- Firmware version bump at release time.
