# heater_iheater_remote Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the Klipper heater `heater_virtual` to `heater_iheater_remote`
everywhere, with a one-run upgrade for existing installs.

**Architecture:** The install helper learns both section headers first, so it
can read a version 1 file. Then the Klipper module, its cfg (version 2), the
Moonraker endpoint and the installer's link names move together. Next the
installer cleans up the old link, then the firmware switches its discovery
prefix. A final sweep fixes the remaining docs and checks that the old name
survives only where it must.

**Tech Stack:** Python 3.7+ (Klipper module, Moonraker component, install
helper), bash (install scripts), C++ on Arduino-ESP32 2.0.17 with Unity tests
(firmware policy).

**Spec:** docs/design/2026-11-21-rename-heater-iheater-remote-design.md

## Global Constraints

- New names: module file `klipper/heater_iheater_remote.py`, section
  `[heater_iheater_remote <name>]`, object `heater_iheater_remote <name>`,
  webhooks endpoint `heater_iheater_remote/report`.
- The firmware matches only the new name. There is no backward compatibility
  in the firmware.
- The install helper reads both `[heater_virtual <name>]` and
  `[heater_iheater_remote <name>]`.
- `iheater_remote.cfg` is `CONFIG_VERSION` 2.
- Dated documents in `docs/design/` are not edited.
- Never remove an `extras/heater_virtual.py` that is not a link into
  `INSTALL_PATH`.
- Write Canadian English in comments, messages and docs.
- Policy files in `lib/remote-policy/` use CRLF line endings. Keep each file's
  existing line endings.
- Never flash or upload to a device.
- Commit messages end with a blank line, then a `Co-Authored-By` trailer
  naming the model that wrote the change.

## Review Focus

1. **The user declines the version 2 upgrade.** Their cfg keeps
   `[heater_virtual chamber]`, which Klipper can no longer load. Expect a
   plain warning and a Klipper restart that defaults to no (Task 2:
   `test_upgrade_declined_warns_old_section`).
2. **The user renamed their heater** (`[heater_virtual hood]`). The carry keeps
   their token and sensor and reports the rename (Task 1:
   `test_carry_from_old_header_reports_rename`).
3. **The repo was pulled but `./install.sh` wasn't re-run,** so
   `extras/heater_virtual.py` dangles. Install removes it, and uninstall does
   too (Task 3).
4. **`extras/heater_virtual.py` belongs to someone else.** It is left alone
   with a warning (Task 3: `test_install_leaves_foreign_old_link`).
5. **An old manual install** (`config/iheater_remote.cfg` with
   `[heater_virtual chamber]`) is moved, upgraded to version 2 with its token
   carried, and ends up with the new header (Task 2: update
   `test_install_migrates_old_layout`).

---

### Task 1: The install helper reads both section headers

**Files:**
- Modify: `scripts/iheater_config.py` (`_SECTION_RE`, docstrings, the
  "no [heater_virtual ...] section" messages)
- Test: `scripts/test_iheater_config.py`

**Interfaces:**
- Produces: `get_key`, `set_key` and `carry` find the first section headed
  `[heater_iheater_remote <name>]` or `[heater_virtual <name>]`. The error
  text becomes `no [heater_iheater_remote ...] section found`.

- [ ] **Step 1: Write the failing tests.**
  - `test_get_key_new_header`: `get_key("[heater_iheater_remote chamber]\nsensor: a\n", "sensor") == "a"`.
  - `test_set_key_new_header`: setting `remote_token` to `7K3M-9QX2` in
    `"[heater_iheater_remote chamber]\n"` gives
    `"[heater_iheater_remote chamber]\nremote_token: 7K3M-9QX2\n"`.
  - `test_carry_old_header_into_new`: old
    `"[heater_virtual chamber]\nsensor: s1\nremote_token: 7K3M-9QX2\n"`, new
    is the shipped-style `"[heater_iheater_remote chamber]\nsensor: x\n#remote_token: XXXX-XXXX\n"`.
    The output contains `[heater_iheater_remote chamber]`, `sensor: s1` and
    `remote_token: 7K3M-9QX2`, and does not contain `heater_virtual`.
    `carried == ["sensor", "remote_token"]` (in `_CARRIED_KEYS` order) and
    `renamed is None`.
  - `test_carry_from_old_header_reports_rename`: old
    `"[heater_virtual hood]\nremote_token: 7K3M-9QX2\n"` gives `renamed == "hood"`.
  - `test_carry_old_header_crlf`: the same carry with `\r\n` line endings in
    both inputs keeps `\r\n` throughout the output.
  - `test_set_key_no_section_message`: `set_key("[other]\n", "sensor", "x")`
    raises `ValueError` with `heater_iheater_remote` in its message.
  - Existing tests that use `[heater_virtual ...]` fixtures stay unchanged.
    They now prove the legacy header still works.
- [ ] **Step 2: Run them and watch the new ones fail.**
  Run: `python -m unittest discover -s scripts -p "test_iheater_config.py" -v`
  Expected: the new-header tests and the message test fail; the rest pass.
- [ ] **Step 3: Change `_SECTION_RE` to**
  `r"^\[(?:heater_iheater_remote|heater_virtual)\s+([^\]\s]+)\s*\]"`. Update
  the docstrings and error messages to the new name, and add a comment saying
  the old header is accepted so a version 1 install can be upgraded.
- [ ] **Step 4: Run the test file.** Expected: all pass.
- [ ] **Step 5: Commit** `feat(install): read heater_iheater_remote sections`.

---

### Task 2: Rename the Klipper module, cfg v2, Moonraker endpoint and installer paths

**Files:**
- Rename (`git mv`): `klipper/heater_virtual.py` → `klipper/heater_iheater_remote.py`,
  `klipper/test_heater_virtual.py` → `klipper/test_heater_iheater_remote.py`
- Modify: `klipper/heater_iheater_remote.py`, `klipper/test_heater_iheater_remote.py`,
  `klipper/iheater_remote.cfg`, `klipper/CONFIG_CHANGES.md`,
  `moonraker/iheater_remote.py`, `moonraker/test_iheater_remote.py`,
  `install.sh`, `uninstall.sh`, `scripts/test_install.py`,
  `klipper/README.md`, `moonraker/README.md`

**Interfaces:**
- Consumes: Task 1's helper.
- Produces:
  - `_REPORT_PATH = 'heater_iheater_remote/report'` and
    `_ROUTER_NAME = 'heater_iheater_remote_router'` in the module.
  - `_KLIPPY_ENDPOINT = 'heater_iheater_remote/report'` in the Moonraker
    component.
  - The heater class is renamed `HeaterIheaterRemote`.
    `IheaterRemoteSensor` and the sensor type `iheater_remote` are unchanged.
  - The sensor's lookup is `printer.lookup_object('heater_iheater_remote ' + name, None)`.
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
  - In `scripts/test_install.py`:
    - every `extras / "heater_virtual.py"` becomes
      `extras / "heater_iheater_remote.py"`;
    - the link source becomes `REPO / "klipper" / "heater_iheater_remote.py"`;
    - `test_install_migrates_old_layout` asserts that the moved cfg contains
      `[heater_iheater_remote chamber]` and no `[heater_virtual`. Its
      old-layout fixture is headed `[heater_virtual chamber]` with no
      `CONFIG_VERSION`.
  - Add `test_upgrade_declined_warns_old_section` to `InstallSmokeTests`:
    - the installed `addons/iheater_remote.cfg` is `# CONFIG_VERSION: 1`
      followed by `[heater_virtual chamber]` with a token;
    - answer `n` at the upgrade prompt and defaults elsewhere;
    - the file is unchanged;
    - stdout contains `Klipper will stop with a config error`;
    - the log has `restart moonraker` and no `restart klipper`.
  - Add `test_upgrade_to_v2_carries_token`:
    - the same v1 file, answered with defaults;
    - the file now has `# CONFIG_VERSION: 2`, `[heater_iheater_remote chamber]`
      and the token, and no `heater_virtual`.
- [ ] **Step 2: Run them and watch them fail.**
  Run: `python -m unittest discover -s klipper -p "test_*.py"`, the same for
  `moonraker` and `scripts`.
  Expected: an import error for the Klipper tests, an endpoint mismatch for
  Moonraker, and failures in the new and changed smoke tests.
- [ ] **Step 3: Rename and edit.**
  - Run the two `git mv` commands.
  - In the module, change the constants, the class, the lookup, the error
    strings and the comments to the new name.
  - In the Moonraker component, change the endpoint constant and the header
    comment.
  - In `iheater_remote.cfg`:
    - `# CONFIG_VERSION: 2`;
    - `# Requires klipper/heater_iheater_remote.py in ~/klipper/klippy/extras/.`;
    - `[heater_iheater_remote chamber]`;
    - `TEMPERATURE_WAIT SENSOR="heater_iheater_remote chamber"`.
    Keep the working copy's line endings.
  - Append to `CONFIG_CHANGES.md`:
    ```
    ## 2
    - The heater section is now `[heater_iheater_remote chamber]` (was
      `[heater_virtual chamber]`), and M191 waits on
      `heater_iheater_remote chamber`. Re-run `./install.sh` after pulling:
      it carries your token and sensor into the new file.
    - If you named your heater something other than `chamber`, rename the
      new section and the M191 sensor to match.
    ```
  - In `install.sh`:
    - update `step_link_modules` and the `printer["heater_iheater_remote chamber"].linked`
      next-steps note.
    - After the config-file step, if the installed cfg still has a line
      matching `^\[heater_virtual[[:space:]]` (the user declined the
      upgrade), warn
      `"${ADDON_CFG} still uses [heater_virtual ...]. Klipper will stop with a config error until you accept the update (re-run ./install.sh)."`
      and set `KLIPPER_RESTART_DEFAULT=n`.
  - In `uninstall.sh`, update `step_links` to the new path.
  - Change the module name, section and object in `klipper/README.md` and
    `moonraker/README.md`. In the Klipper README's update paragraph, say that
    version 2 renamed the section and `./install.sh` carries the settings.
- [ ] **Step 4: Run the three Python gates.** Expected: all pass.
- [ ] **Step 5: Commit** `feat!: rename heater_virtual to heater_iheater_remote`.
  The body says the firmware must be updated at the same time.

---

### Task 3: Install and uninstall clean up the old `heater_virtual.py` link

**Files:**
- Modify: `scripts/common.sh`, `install.sh`, `uninstall.sh`,
  `scripts/test_install.py`

**Interfaces:**
- Consumes: `link_target` (common.sh), plus `inside_install` and
  `unlink_module` (currently in uninstall.sh).
- Produces:
  - `inside_install PATH` and `unlink_module DEST` move to `scripts/common.sh`
    unchanged.
  - install.sh gains `step_old_link`, which calls
    `unlink_module "${KLIPPER_PATH}/klippy/extras/heater_virtual.py"` after
    `step_link_modules`.
  - uninstall.sh's `step_links` unlinks both
    `heater_iheater_remote.py` and `heater_virtual.py`.
  - When nothing is at the old path, the install output prints nothing extra.
    This needs a quiet mode or a pre-check, so a fresh install doesn't print
    `heater_virtual.py not present`.

- [ ] **Step 1: Write the failing smoke tests.**
  - `test_install_removes_dangling_old_link`:
    - before install, `extras/heater_virtual.py` is a link to
      `REPO / "klipper" / "heater_virtual.py"`, which no longer exists;
    - after install with defaults the old link is gone, the new link resolves
      to `REPO / "klipper" / "heater_iheater_remote.py"`, and stdout names
      `heater_virtual.py`.
  - `test_install_leaves_foreign_old_link`: `extras/heater_virtual.py` is a
    link to a file outside the repo. After install it is still that link, and
    stdout contains `left it`.
  - `test_fresh_install_no_old_link_noise`: on a fresh fixture, stdout doesn't
    contain `heater_virtual`.
  - `test_uninstall_removes_old_link`:
    - after install, recreate the dangling old link;
    - uninstall with defaults removes both links.
- [ ] **Step 2: Run them and watch them fail.**
  Run: `python -m unittest discover -s scripts -p "test_install.py" -v`
  Expected: the four new tests fail; the rest pass.
- [ ] **Step 3: Implement.**
  - Move the two functions into `common.sh`.
  - Add `step_old_link` to install.sh, run only when the old path is a link
    or exists.
  - Add the second `unlink_module` call to uninstall.sh.
  - A removed old link sets `CHANGED_LINKS=1`, so Klipper's restart is
    offered.
- [ ] **Step 4: Run the scripts gate and `bash -n install.sh uninstall.sh scripts/common.sh`.**
  Expected: all pass.
- [ ] **Step 5: Commit** `feat(install): remove the old heater_virtual.py link`.

---

### Task 4: The firmware discovers `heater_iheater_remote` objects

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

### Task 5: Docs sweep and leftover check

**Files:**
- Modify: `README.md`, `AGENTS.md`, and any file the check below finds
  outside the allowed list.

- [ ] **Step 1: Rename in docs.**
  - `README.md`:
    - lines 29, 202 and 261 use the new section and object name;
    - add a sentence to the Klipper part: "Older installs used
      `[heater_virtual]`; update the Remote's firmware and re-run
      `./install.sh` together."
  - `AGENTS.md` lines 6 and 32: change `heater_virtual` to
    `heater_iheater_remote`.
- [ ] **Step 2: Run the leftover check.**
  Run: `git grep -n "heater_virtual" -- . ':!docs/design/2026-11-1*' ':!docs/design/2026-11-20*' ':!docs/design/2026-11-21-install-scripts-*'`
  Expected: hits only in:
  - `scripts/iheater_config.py`: the legacy matcher and its comment;
  - `scripts/test_iheater_config.py`: the legacy-header tests;
  - `install.sh`: the old-link path and the declined-upgrade warning;
  - `uninstall.sh`: the old-link path;
  - `scripts/test_install.py`: the old-link and old-layout tests;
  - `klipper/CONFIG_CHANGES.md`: the version 2 entry;
  - `README.md`: the upgrade sentence;
  - `klipper/README.md`: the upgrade note;
  - `test/test_remote_policy/test_main.cpp`: the negative test;
  - this plan and its design.
  Fix any other hit.
- [ ] **Step 3: Run all five gates** (both `pio` gates once, with output
  redirected). Expected: everything passes.
- [ ] **Step 4: Commit** `docs: heater_iheater_remote in README and AGENTS.md`.

---

## Bench check (the user, on hestia)

1. Flash the new firmware to the Remote (the user does this).
2. `git pull`, then `./install.sh`. Accept the version 2 update. The old
   `heater_virtual.py` link is removed and Klipper restarts.
3. Mainsail shows `heater_iheater_remote chamber` with a target box, and the
   Remote pairs.
4. Run `./install.sh` again: every step says `skip`.
