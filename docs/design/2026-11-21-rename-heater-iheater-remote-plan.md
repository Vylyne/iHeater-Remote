# heater_iheater_remote Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the Klipper heater `heater_virtual` to `heater_iheater_remote`
everywhere. This is a clean rename with no upgrade path from the old name.

**Architecture:** The Klipper module, its cfg, the Moonraker endpoint, the
install helper's section matcher and the installer's link names move together
in one task. Then the firmware switches its discovery prefix. A final sweep
fixes the remaining docs and checks that the old name survives only where it
must.

**Tech Stack:** Python 3.7+ (Klipper module, Moonraker component, install
helper), bash (install scripts), C++ on Arduino-ESP32 2.0.17 with Unity tests
(firmware policy).

**Spec:** docs/design/2026-11-21-rename-heater-iheater-remote-design.md

## Global Constraints

- New names: module file `klipper/heater_iheater_remote.py`, section
  `[heater_iheater_remote <name>]`, object `heater_iheater_remote <name>`,
  webhooks endpoint `heater_iheater_remote/report`.
- No backward compatibility anywhere: the firmware, the install helper and
  the install scripts know only the new name. There is no old-link cleanup
  and no old-section warning.
- `iheater_remote.cfg` stays `CONFIG_VERSION` 1, and `CONFIG_CHANGES.md` gets
  no new entry.
- Dated documents in `docs/design/` are not edited, except this plan and its
  design.
- Write Canadian English in comments, messages and docs.
- Policy files in `lib/remote-policy/` use CRLF line endings. Keep each file's
  existing line endings.
- Never flash or upload to a device.
- Commit messages end with a blank line, then a `Co-Authored-By` trailer
  naming the model that wrote the change.

## Review Focus

1. **An old `[heater_virtual chamber]` file reaches the helper.** `set-key`
   and `get-key` fail with a plain `no [heater_iheater_remote ...] section`
   message rather than a traceback (Task 1: `test_set_key_old_header_rejected`).
2. **A user renamed their heater** (`[heater_iheater_remote hood]`). The carry
   still keeps their token and sensor and reports the rename (the existing
   carry tests, now with the new header).
3. **The shipped cfg and the module agree.** The cfg's section, its M191
   `SENSOR=` and the module's object name all use `heater_iheater_remote`
   (Task 1: the install smoke tests set a key in the shipped cfg).
4. **The Remote sees the new name only.** `heater_virtual chamber` is not
   discovered (Task 2: negative assertion).

---

### Task 1: Rename the Klipper module, cfg, Moonraker endpoint, helper and installer paths

**Files:**
- Rename (`git mv`): `klipper/heater_virtual.py` → `klipper/heater_iheater_remote.py`,
  `klipper/test_heater_virtual.py` → `klipper/test_heater_iheater_remote.py`
- Modify: `klipper/heater_iheater_remote.py`, `klipper/test_heater_iheater_remote.py`,
  `klipper/iheater_remote.cfg`, `moonraker/iheater_remote.py`,
  `moonraker/test_iheater_remote.py`, `scripts/iheater_config.py`,
  `scripts/test_iheater_config.py`, `install.sh`, `uninstall.sh`,
  `scripts/test_install.py`, `klipper/README.md`, `moonraker/README.md`

**Interfaces:**
- Produces:
  - `_REPORT_PATH = 'heater_iheater_remote/report'` and
    `_ROUTER_NAME = 'heater_iheater_remote_router'` in the module.
  - `_KLIPPY_ENDPOINT = 'heater_iheater_remote/report'` in the Moonraker
    component.
  - The heater class is renamed `HeaterIheaterRemote`.
    `IheaterRemoteSensor` and the sensor type `iheater_remote` are unchanged.
  - The sensor's lookup is `printer.lookup_object('heater_iheater_remote ' + name, None)`.
  - The helper's `_SECTION_RE` is
    `r"^\[heater_iheater_remote\s+([^\]\s]+)\s*\]"`. Its error text is
    `no [heater_iheater_remote ...] section found` (and `... section in <file>`
    from the CLI).
  - Install links `${INSTALL_PATH}/klipper/heater_iheater_remote.py` at
    `${KLIPPER_PATH}/klippy/extras/heater_iheater_remote.py`.
  - Uninstall unlinks that path.

- [ ] **Step 1: Update the tests first.**
  - In `klipper/test_heater_iheater_remote.py`:
    - import `heater_iheater_remote`;
    - change every `heater_virtual <name>` object/section name to
      `heater_iheater_remote <name>`;
    - change the expected error text to the new name.
  - In `moonraker/test_iheater_remote.py`:
    - `self.assertEqual(req.endpoint, 'heater_iheater_remote/report')`;
    - the pass-through error string becomes
      `"no heater_iheater_remote has remote_token 7K3P-9QXM"`.
  - In `scripts/test_iheater_config.py`:
    - change every `[heater_virtual <name>]` fixture to
      `[heater_iheater_remote <name>]`;
    - add `test_set_key_old_header_rejected`:
      `set_key("[heater_virtual chamber]\n", "sensor", "x")` raises
      `ValueError` whose message contains `heater_iheater_remote`;
    - add `test_get_key_old_header_ignored`:
      `get_key("[heater_virtual chamber]\nsensor: a\n", "sensor")` is `None`.
  - In `scripts/test_install.py`:
    - every `extras / "heater_virtual.py"` becomes
      `extras / "heater_iheater_remote.py"`;
    - the link source becomes `REPO / "klipper" / "heater_iheater_remote.py"`;
    - the dangling-link fixture's target becomes
      `"gone" / "deeper" / "heater_iheater_remote.py"`.
- [ ] **Step 2: Run them and watch them fail.**
  Run: `python -m unittest discover -s klipper -p "test_*.py"`, the same for
  `moonraker` and `scripts`.
  Expected: an import error for the Klipper tests, an endpoint mismatch for
  Moonraker, section-not-found failures in the helper tests, and link-path
  failures in the smoke tests.
- [ ] **Step 3: Rename and edit.**
  - Run the two `git mv` commands.
  - In the module, change the constants, the class, the lookup, the error
    strings and the comments to the new name.
  - In the Moonraker component, change the endpoint constant and the header
    comment.
  - In the helper, change `_SECTION_RE`, the docstrings and both error
    messages.
  - In `iheater_remote.cfg` (keep `# CONFIG_VERSION: 1` and the working copy's
    line endings):
    - `# Requires klipper/heater_iheater_remote.py in ~/klipper/klippy/extras/.`;
    - `[heater_iheater_remote chamber]`;
    - `TEMPERATURE_WAIT SENSOR="heater_iheater_remote chamber"`.
  - In `install.sh`, update `step_link_modules` and the
    `printer["heater_iheater_remote chamber"].linked` next-steps note.
  - In `uninstall.sh`, update `step_links` to the new path.
  - Change the module name, section and object in `klipper/README.md`
    (including the manual `ln -s` line) and `moonraker/README.md`.
- [ ] **Step 4: Run the three Python gates and `bash -n install.sh uninstall.sh scripts/common.sh`.**
  Expected: all pass.
- [ ] **Step 5: Commit** `feat!: rename heater_virtual to heater_iheater_remote`.
  The body says the firmware must be updated at the same time and that an
  existing install must be uninstalled first.

---

### Task 2: The firmware discovers `heater_iheater_remote` objects

**Files:**
- Modify: `lib/remote-policy/src/RemotePolicy.h`,
  `lib/remote-policy/src/RemotePolicy.cpp`,
  `lib/remote-policy/src/ReportPolicy.h` (comment),
  `test/test_remote_policy/test_main.cpp`,
  `src/standalone/MoonrakerLink.cpp`, `src/standalone/MoonrakerLink.h`,
  `src/standalone/StandaloneApp.cpp`,
  `tools/fake_moonraker/fake_moonraker.py`,
  `tools/fake_moonraker/fake_moonraker.README.md`, `platformio.ini` (comment)

**Interfaces:**
- Produces: `bool isRemoteHeaterObject(const char *name)` replaces
  `isHeaterVirtualObject`, with prefix `"heater_iheater_remote "`. Both
  `MoonrakerLink.cpp` call sites use it.

- [ ] **Step 1: Write the failing test.**
  Replace the `isHeaterVirtualObject` assertions in `test_main.cpp` with:
  ```cpp
  TEST_ASSERT_TRUE(isRemoteHeaterObject("heater_iheater_remote chamber"));
  TEST_ASSERT_FALSE(isRemoteHeaterObject("heater_iheater_remote"));
  TEST_ASSERT_FALSE(isRemoteHeaterObject("heater_iheater_remote "));
  TEST_ASSERT_FALSE(isRemoteHeaterObject("heater_virtual chamber"));
  TEST_ASSERT_FALSE(isRemoteHeaterObject("heater_generic chamber"));
  TEST_ASSERT_FALSE(isRemoteHeaterObject("heater_iheater_remotex a"));
  TEST_ASSERT_FALSE(isRemoteHeaterObject(nullptr));
  ```
  Rename the test function to match.
- [ ] **Step 2: Run it and watch it fail.**
  Run: `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native > <file> 2>&1`
  and read the tail.
  Expected: a compile error, because `isRemoteHeaterObject` is undeclared.
- [ ] **Step 3: Rename the function and its prefix.**
  - Keep CRLF in the policy files.
  - Update both call sites.
  - Change these user-visible strings to `heater_iheater_remote`:
    - `goUnpaired("no heater_iheater_remote in Klipper")`;
    - the `[MOON]` log lines;
    - `" to your [heater_iheater_remote] section"`;
    - the setup page note `Put this in your [heater_iheater_remote] section as remote_token: …`.
  - Update the comments in `MoonrakerLink.h`, `StandaloneApp.cpp`,
    `ReportPolicy.h` and `RemotePolicy.h`.
  - In fake_moonraker:
    - `PAIRED = "heater_iheater_remote chamber"`;
    - extras named `heater_iheater_remote extra{i}`;
    - the error message, the help text and the README.
- [ ] **Step 4: Run both `pio` gates.**
  Run `pio run -e esp32c3-super-mini-remote` and the native tests, each
  redirected to a file. Expected: the build succeeds and every native test
  passes.
- [ ] **Step 5: Commit** `feat(firmware)!: discover heater_iheater_remote objects`.
  The body says the Remote must be reflashed together with the Klipper update.

---

### Task 3: Docs sweep and leftover check

**Files:**
- Modify: `README.md`, `AGENTS.md`, and any file the check below finds
  outside the allowed list.

- [ ] **Step 1: Rename in docs.**
  - `README.md`: lines 29, 202 and 261 use the new section and object name.
  - `AGENTS.md` lines 6 and 32: change `heater_virtual` to
    `heater_iheater_remote`.
- [ ] **Step 2: Run the leftover check.**
  Run: `git grep -n "heater_virtual" -- . ':!docs/design/2026-11-1*' ':!docs/design/2026-11-20*' ':!docs/design/2026-11-21-install-scripts-*'`
  Expected: hits only in:
  - `scripts/test_iheater_config.py`: the two old-header rejection tests;
  - `test/test_remote_policy/test_main.cpp`: the negative test;
  - this plan and its design.
  Fix any other hit.
- [ ] **Step 3: Run all five gates** (both `pio` gates once, with output
  redirected). Expected: everything passes.
- [ ] **Step 4: Commit** `docs: heater_iheater_remote in README and AGENTS.md`.

---

## Bench check (the user, on hestia)

The old install was already removed from hestia.

1. Flash the new firmware to the Remote (the user does this).
2. `git pull`, then `./install.sh`, and set the token.
3. Mainsail shows `heater_iheater_remote chamber` with a target box, and the
   Remote pairs.
4. Run `./install.sh` again: every step says `skip`.
