# Rename heater_virtual to heater_iheater_remote — design

Status: approved · 2026-11-21 · revised 2026-11-21: no upgrade path

## Intent

The Klipper module is the only part still called `heater_virtual`. The
Moonraker component, the cfg and the installer all say `iheater_remote`. The
user wants one name throughout. The new name keeps the `heater_` prefix,
because Mainsail only offers a target box for `extruder*` and `heater_*`
objects. It is not `heater_iheater`, in case upstream iHeater ever claims that
name.

## Goals

- The Klipper module file is `klipper/heater_iheater_remote.py`, its section is
  `[heater_iheater_remote <name>]`, and its object is
  `printer["heater_iheater_remote <name>"]`.
- The Klippy webhooks endpoint becomes `heater_iheater_remote/report`, in both
  the module and the Moonraker component.
- The Remote firmware discovers `heater_iheater_remote <name>` objects and says
  the new name in its log, messages and setup page.
- `iheater_remote.cfg` keeps `CONFIG_VERSION` 1. The only install (hestia)
  was removed before the rename, so nothing needs upgrading.

## Non-goals

- Backward compatibility in the firmware. The user decided: the firmware
  matches only the new name, and they reflash at the same time as they update
  the Klipper side. Pairing is down between the two steps.
- Rewriting the dated design and plan documents in `docs/design/`. They record
  what was decided at the time.
- Any upgrade path for the old name. The user removed the only existing install
  from hestia before the rename, so the install helper, `install.sh` and
  `uninstall.sh` know only the new name: no old-header matching, no old-link
  cleanup, no old-section warning.

## Anti-goals

- Leftover `heater_virtual` names in code, messages or current docs.

## Approaches

1. **Firmware matches both names for a release** (my first proposal). Either
   update order works. Rejected by the user: they update both together, so the
   extra matcher line and test buy nothing.
2. **Firmware matches only the new name** (chosen). One prefix, no transition
   code to remove later.

## Design

- Rename the module, its unit test file, the section prefix, the object name,
  the endpoint and every user-facing string across `klipper/`, `moonraker/`,
  `src/standalone/`, `lib/remote-policy/` (the discovery prefix and its tests),
  `tools/fake_moonraker/`, `install.sh`, `uninstall.sh`, `scripts/`, the
  READMEs and AGENTS.md.
- The install helper's section matcher accepts only
  `[heater_iheater_remote <name>]`.
- `install.sh` links `heater_iheater_remote.py`, and `uninstall.sh` removes
  that link.

## Left to the builder

- Wording of the renamed messages.
