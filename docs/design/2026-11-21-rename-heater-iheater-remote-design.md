# Rename heater_virtual to heater_iheater_remote — design

Status: approved · 2026-11-21

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
- `iheater_remote.cfg` moves to `CONFIG_VERSION` 2, with a `CONFIG_CHANGES.md`
  entry that tells users to re-run `./install.sh` after pulling.
- An existing install upgrades with one `./install.sh` run, keeping the user's
  token, sensor and heater name.

## Non-goals

- Backward compatibility in the firmware. The user decided: the firmware
  matches only the new name, and they reflash at the same time as they update
  the Klipper side. Pairing is down between the two steps.
- Rewriting the dated design and plan documents in `docs/design/`. They record
  what was decided at the time.

## Anti-goals

- An upgrade that leaves a dangling `extras/heater_virtual.py` link or a stale
  `[heater_virtual ...]` section that stops Klipper.
- Removing an `extras/heater_virtual.py` that doesn't point into this repo.

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
- The install helper's section matcher accepts both `[heater_virtual <name>]`
  and `[heater_iheater_remote <name>]`, so `carry`, `get-key` and `set-key`
  work on an installed version 1 file. The carried result uses the new header
  from the shipped file and keeps the user's heater name.
- `install.sh` links `heater_iheater_remote.py`. If
  `extras/heater_virtual.py` is a link into `INSTALL_PATH` (it dangles once the
  file is renamed), install removes it and says so; a foreign file or link is
  left with a warning. `uninstall.sh` removes both link names under the same
  rule.
- The version 2 upgrade goes through the existing upgrade prompt, which lists
  the `CONFIG_CHANGES.md` entry and carries the user's values.

## Left to the builder

- Wording of the new `CONFIG_CHANGES.md` entry and the installer's messages.
- Whether to keep `heater_virtual` in the helper's matcher as a named constant
  or a regex alternative.
