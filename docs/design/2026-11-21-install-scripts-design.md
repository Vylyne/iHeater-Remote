# Install and uninstall scripts — design

Status: approved · 2026-11-21

## Intent

Installing the Klipper and Moonraker parts of the iHeater Remote is four
manual steps spread over two READMEs: two symlinks, a config copy that must be
hand-edited, an `[include]` in `printer.cfg`, a section in `moonraker.conf`,
and two service restarts. The user wants one script that does this, one that
undoes it, and a safe way to update `iheater_remote.cfg` when a later release
changes it.

The format follows the user's own `mcu-updater` `install.sh`: environment
overrides for paths, preflight checks, sectioned `ok` / `skip` / `warn` output,
idempotent re-runs, `ask` prompts with stated defaults, an `[update_manager]`
block appended from a template in `scripts/`, and a closing list of next
steps.

## Goals

1. `./install.sh` on the printer host installs both components, sets up
   `iheater_remote.cfg`, wires the includes and sections, and restarts what
   needs restarting. Re-running it is safe.
2. `./uninstall.sh` removes exactly what the installer added.
3. `iheater_remote.cfg` carries `# CONFIG_VERSION: <n>`. When the shipped
   version is newer than the installed one, the installer lists the breaking
   changes in between and offers to replace the file, keeping the user's
   pairing token and sensor.
4. The config-editing logic has unit tests in the gate.

## Non-goals

- Flashing or building anything. The scripts never touch the Remote.
- Removing or rewriting the user's own `M141` / `M191` macros. The installer
  only warns about them.
- Running the installer from Moonraker's update manager (`install_script` is
  deprecated). After an update the user re-runs `./install.sh` if the
  changelog says the config changed.
- Supporting more than one Klipper instance per host (KIAUH multi-instance).
  Paths can be overridden by environment variable, which covers a single
  non-default instance.
- Windows. The scripts run on the Linux printer host.

## Anti-goals

- Never lose the user's edits. No overwrite of `iheater_remote.cfg` without a
  "yes" and a dated backup. No edit to `printer.cfg` without a "yes".
- Never restart Klipper during a print. If the print state is printing or
  paused, or cannot be read, Klipper is not restarted automatically.
- Never leave Moonraker unable to start. `[iheater_remote]` is only added
  after the component is linked, and is removed before the link is removed.
- Uninstall never deletes a file it did not create, and never removes a
  symlink that points somewhere other than this repo.

## Constraints

- Bash for the scripts, run as the normal user (not root); `sudo` only for
  `systemctl restart`.
- The helper is Python 3 standard library only, run with the system
  `python3`, minimum 3.7 (oldest Python Klipper hosts still ship).
- Canadian English in comments, output and docs.
- Add the helper's tests to the gate commands in AGENTS.md:
  `python -m unittest discover -s scripts -p "test_*.py"`.

## Approaches

1. **Two bash scripts with `grep`/`sed` for the config edits.** Closest to
   `mcu-updater`, but nothing tests the edits, which are the risky part.
2. **Bash scripts plus a small Python helper for the config edits (chosen).**
   The scripts handle paths, prompts, symlinks and services; the helper reads
   versions, finds includes and sections, and carries values over, with unit
   tests. `mcu-updater` does the same with its `mcu_updater.seed` helper.
3. **One Python installer.** Fully testable, but further from the
   `mcu-updater` format, and service restarts and `sudo` are clumsier.

The user chose 2.

## Design

### Files

| File | Role |
|---|---|
| `install.sh` | Installer, at the repo root. |
| `uninstall.sh` | Uninstaller, at the repo root. |
| `scripts/common.sh` | Sourced by both: path defaults, output helpers (`section`, `ok`, `skip`, `warn`, `err`, `note`, `ask`), preflight checks, the restart step. |
| `scripts/iheater_config.py` | The config helper (subcommands below). |
| `scripts/test_iheater_config.py` | Its unit tests. |
| `scripts/moonraker-update-manager.conf` | The `[update_manager iheater-remote]` block. |
| `klipper/CONFIG_CHANGES.md` | Config versions and their breaking changes. |
| `klipper/iheater_remote.cfg` | Gains `# CONFIG_VERSION: 1`. |

### Paths (environment overrides)

| Variable | Default |
|---|---|
| `KLIPPER_PATH` | `~/klipper` |
| `MOONRAKER_PATH` | `~/moonraker` |
| `PRINTER_DATA` | `~/printer_data` |
| `INSTALL_PATH` | the directory holding `install.sh` |

Derived: `CONFIG_DIR=$PRINTER_DATA/config`,
`ADDON_CFG=$CONFIG_DIR/addons/iheater_remote.cfg`,
`PRINTER_CFG=$CONFIG_DIR/printer.cfg`,
`MOONRAKER_CONF=$CONFIG_DIR/moonraker.conf`.

### Preflight (both scripts)

- Not root, else stop.
- `python3` present and at least 3.7, else stop.
- `$KLIPPER_PATH/klippy/extras` and `$MOONRAKER_PATH/moonraker/components`
  exist, else stop with the variable to set.
- `$CONFIG_DIR` exists, else stop.
- `klipper.service` and `moonraker.service` exist, else warn (restarts are
  then skipped with a note).

### Install steps, in order

1. **Link the modules.** Symlink `klipper/heater_virtual.py` into
   `klippy/extras/` and `moonraker/iheater_remote.py` into
   `moonraker/components/`. A correct link already there: `skip`. A link to
   somewhere else, or a regular file: ask (default yes) to replace it, moving
   a regular file aside as `<name>.bak-<YYYYMMDD-HHMMSS>` first.
2. **The config file.** Create `$CONFIG_DIR/addons/` if missing.
   - **Not installed:** copy the shipped file. Ask for the pairing token
     ("from the Remote's Setup page, blank to set it later"). A token is
     accepted if, with hyphens removed and upper-cased, it is 8 characters of
     `0-9A-HJKMNP-TV-Z`; it is written as typed. Blank leaves the line
     commented. Ask for the chamber sensor (default: the shipped
     `temperature_sensor chamber_hexa`, blank keeps it).
   - **Installed, same or newer version:** `skip`, and show the version.
   - **Installed, older version** (no tag counts as 0): list each breaking
     change from `CONFIG_CHANGES.md` above the installed version, up to the
     shipped one, then ask (default **no**) to replace it. On yes: back up as
     `iheater_remote.cfg.bak-<YYYYMMDD-HHMMSS>`, copy the shipped file, then
     carry over the values (below). On no: `warn` that the file is out of
     date and point at `CONFIG_CHANGES.md`.
3. **Macro check, before the include.** Search `$CONFIG_DIR` recursively
   (`*.cfg`) for `[gcode_macro M141]` and `[gcode_macro M191]`,
   case-insensitive, outside our own file. Our file defines both, and Klipper
   refuses duplicate sections, so including it next to another definition
   stops Klipper at the next restart. For each one found, `warn` with the
   file and line. If any were found, the include in step 4 is offered with
   default **no**, and the note says to delete or rename those macros and
   re-run. The installer never edits them (builder's call, see "Calls made
   after review").
4. **`printer.cfg` include.** If any `[include ...]` in `printer.cfg` already
   matches `addons/iheater_remote.cfg` (exact path or a glob such as
   `addons/*.cfg`, matched relative to `$CONFIG_DIR`), `skip`. Otherwise ask
   (default yes) to add `[include addons/iheater_remote.cfg]`. It goes after
   the last existing `[include ...]` line, or at the top of the file when
   there is none, and always above the `#*# <--- SAVE_CONFIG --->` block.
   Back up `printer.cfg` before the edit.
5. **`moonraker.conf`.** Append `[iheater_remote]` if no such section exists.
   Append the block from `scripts/moonraker-update-manager.conf`, with `path`
   set to `$INSTALL_PATH` and `origin` to the repo's `origin` URL, if no
   `[update_manager iheater-remote]` section exists. If `$INSTALL_PATH` is not
   a git checkout, or has no `origin` remote, skip the block with a `warn`
   (updates then need a manual `git pull`). The URL is used as given, SSH or
   HTTPS. `primary_branch` is always `main`. Back up first if anything is
   appended.
6. **Restart.** See below.
7. **Next steps.** Print the config file path, whether a token is still
   needed, how to check the link (`printer["heater_virtual chamber"].linked`
   and the Remote's link diagnostics), and where the changelog is.

### Uninstall steps, in order

1. **`moonraker.conf`:** remove the `[iheater_remote]` and
   `[update_manager iheater-remote]` sections, each with the lines up to the
   next section header. Back up first.
2. **`printer.cfg`:** if the exact line `[include addons/iheater_remote.cfg]`
   is present, ask (default yes) to remove it. A glob include is left alone,
   with a note. Back up first.
3. **Config file:** ask (default **no**) to delete
   `addons/iheater_remote.cfg`. On no, leave it. On yes, keep a dated backup.
4. **Links:** remove each symlink only if it points into `$INSTALL_PATH`.
   Otherwise `skip` with a note.
5. **Restart**, as below.

### Restart

- Read the print state first, before restarting anything, from
  `http://localhost:7125/printer/objects/query?print_stats` (port from
  `MOONRAKER_PORT`, default 7125).
- Moonraker: restart whenever this run changed a link or `moonraker.conf`. A
  Moonraker restart does not stop a running print, so it is not gated on the
  print state.
- Klipper: needed whenever this run changed a link, `printer.cfg` or the
  config file. Restart it with `systemctl restart klipper`, gated on the
  state read above:
  - `printing` or `paused`: do not restart; `warn` to run `FIRMWARE_RESTART`
    after the print.
  - State unreadable: ask (default **no**).
  - Otherwise: ask (default yes).
- Restart Moonraker first, then Klipper. A Klipper restart reconnects to
  Moonraker by itself.

### The config helper

`python3 scripts/iheater_config.py <command> ...`. Each command prints its
result on stdout and exits 0, or prints a message on stderr and exits 1.
Files are read and written as UTF-8 and keep their line endings.

| Command | Does |
|---|---|
| `version FILE` | Prints the `# CONFIG_VERSION:` number, or 0 when absent. |
| `changes CHANGES_MD FROM TO` | Prints the entries for versions above FROM up to TO. |
| `carry OLD NEW` | Rewrites NEW with OLD's values (below). |
| `set-key FILE KEY VALUE` | Sets KEY in the first `[heater_virtual ...]` section, uncommenting a `#KEY:` line if that is all there is. Used for the token and sensor prompts. |
| `has-include PRINTER_CFG CONFIG_DIR TARGET` | Exit 0 if an include covers TARGET, else 1 (not an error). |
| `add-include PRINTER_CFG TARGET` | Inserts the include line as described above. |
| `remove-include PRINTER_CFG TARGET` | Removes that exact include line. |
| `has-section CONF NAME` / `remove-section CONF NAME` | For `moonraker.conf`. |
| `check-token TOKEN` | Exit 0 if valid. |

**Setting a key (`set-key`, and each carried key).** Within the first
`[heater_virtual ...]` section of the file:
- an active `key:` line is replaced in place;
- otherwise, the first commented `#key:` line is replaced in place,
  uncommented;
- otherwise, the line is inserted straight after the section header.
If the file has an active line and a commented line for the same key, only
the active line changes. A file with no `[heater_virtual ...]` section is an
error.

**Carry-over.** From the first `[heater_virtual <name>]` section in OLD,
carry the active (uncommented) values of `remote_token`, `sensor` and
`max_target` into NEW, using the rules above. A key that is only commented
in OLD is not carried, so NEW keeps its own line. The section name is **not**
carried. The shipped macros name `chamber` (`HEATER=chamber`,
`"heater_virtual chamber"`), so a renamed section in OLD is reported with a
`warn` saying the new file uses `chamber` and the backup holds the old name.
Settings not in the list above are not carried; the backup holds them. The
helper prints each key it carried.

### `CONFIG_CHANGES.md`

One `## <n>` heading per config version, where `<n>` is a positive integer,
newest first. Each is followed by a bullet list (`- `) of breaking changes
that a user must act on; an indented line continues the bullet above it.
Version 1 reads "First versioned config. No changes to make." `changes`
prints, oldest first, each version above FROM up to TO with its bullets.
Versions with no entry print nothing. Text outside headings and bullets is
ignored.

### Update manager block

```
[update_manager iheater-remote]
type: git_repo
path: <INSTALL_PATH>
origin: <origin URL>
primary_branch: main
managed_services: klipper moonraker
```

## Testing

- Unit tests for every helper command, including:
  - include globs;
  - the `SAVE_CONFIG` block;
  - a `printer.cfg` with no includes;
  - CRLF files;
  - carry-over with comments after values;
  - a renamed heater section;
  - a missing version tag;
  - removing a section that is the last one in the file.
- Bench: on hestia, run install, re-run it (all `skip`), uninstall, and
  install again. Then fake an old config by deleting its version tag and
  check the upgrade prompt and carry-over.

The helper tests run under the dev box's Python. There is no 3.7
interpreter, so the helper avoids features newer than 3.7, such as the
walrus operator and `str.removeprefix`. `scripts/` needs no `__init__.py`:
discovery uses `-s scripts`, as with `klipper/` and `moonraker/`.

## Calls made after review

These were settled by the designer after a builder read the draft. Check
them while reading.

1. With existing `M141` or `M191` macros, the include is offered with
   default no and the macros are never edited.
2. The print state is read before any restart. Moonraker restarts even
   during a print; Klipper never does.
3. Carry-over keeps the shipped section name `chamber` and warns about a
   rename.
4. The `set-key` placement rules above.
5. With no git `origin`, the update-manager block is skipped with a warning.
6. The `CONFIG_CHANGES.md` format rules above.

## Left to the builder

- The exact output wording, within the `mcu-updater` style.
- How `ask` behaves when there is no terminal: the stated default is used, as
  in `mcu-updater`.
- Whether `common.sh` or each script holds the restart step.
