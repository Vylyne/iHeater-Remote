# Install and uninstall scripts: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `./install.sh` and `./uninstall.sh` set up and remove the Klipper
and Moonraker parts of the iHeater Remote on a printer host, and the
installer offers a safe upgrade of `iheater_remote.cfg` when its
`CONFIG_VERSION` is older than the shipped one.

**Architecture:** Bash scripts handle paths, prompts, symlinks and services.
They call a small, stdlib-only Python helper for every config-file read or
edit. The helper's logic is pure functions on text, with unit tests. A smoke
test runs both scripts end to end against a fake printer tree, using stub
`systemctl`, `sudo` and `curl`.

**Tech Stack:** bash, Python 3.7+ standard library, `unittest`.

**Spec:** [docs/design/2026-11-21-install-scripts-design.md](2026-11-21-install-scripts-design.md)

## Global Constraints

- Bash scripts run as the normal user, never root. `sudo` is used only for
  `systemctl restart`.
- The helper uses only the Python standard library and runs on Python 3.7 or
  later. Avoid newer features such as the walrus operator,
  `str.removeprefix`, `dict | dict` and `list[int]` annotations at runtime.
- Write Canadian English in comments, output and docs.
- Follow mcu-updater's output style (`/c/git/github/mcu-updater/install.sh`,
  lines 60–147):
  - status words `ok`, `skip`, `warn` and `error` sit in an 8-column gutter;
  - `note` indents to the same gutter;
  - `section` headers;
  - colour only on a TTY, and never when `NO_COLOR` is set;
  - `set -eu`.
- `ask PROMPT DEFAULT` takes `y` or `n` as DEFAULT. It reads one line from
  stdin, and an empty answer or end of input takes the default.
- Path defaults:
  - `KLIPPER_PATH` is `~/klipper`.
  - `MOONRAKER_PATH` is `~/moonraker`.
  - `PRINTER_DATA` is `~/printer_data`.
  - `INSTALL_PATH` is the directory holding the script.
  - `MOONRAKER_PORT` is `7125`.
  - `PYTHON_BIN` is `$(command -v python3)`.
- Derived paths:
  - `CONFIG_DIR=$PRINTER_DATA/config`
  - `ADDON_CFG=$CONFIG_DIR/addons/iheater_remote.cfg`
  - `PRINTER_CFG=$CONFIG_DIR/printer.cfg`
  - `MOONRAKER_CONF=$CONFIG_DIR/moonraker.conf`
- The include line is exactly `[include addons/iheater_remote.cfg]`.
- Backups are named `<file>.bak-<YYYYMMDD-HHMMSS>`. One stamp is taken per
  run and copied with `cp -p`.
- The token is valid when, after removing hyphens and upper-casing, it
  matches `^[0-9A-HJKMNP-TV-Z]{8}$`. It is written as typed.
- Helper files:
  - read and write with `open(path, encoding='utf-8', newline='')`;
  - write in place, never by replacing the file, so that symlinks and
    permissions survive;
  - an edit uses the line ending the file already has (`\r\n` if the file
    contains one, else `\n`).
- Helper CLI:
  - On success, output goes to stdout with exit 0.
  - On failure, a message goes to stderr with exit 1.
  - The predicates `has-include`, `has-section` and `check-token` exit 0
    for yes and 1 for no.
- Commit messages end with a blank line, then
  `Co-Authored-By: <model> <noreply@anthropic.com>`.
- Gate commands (all five must pass before each commit; the fifth is new):
  ```bash
  pio run -e esp32c3-super-mini-remote
  export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native
  python -m unittest discover -s klipper -p "test_*.py"
  python -m unittest discover -s moonraker -p "test_*.py"
  python -m unittest discover -s scripts -p "test_*.py"
  ```
  This plan touches no firmware. The two `pio` gates only need to run once,
  at the end. Every task runs the three Python gates.

## Review Focus

1. **A `printer.cfg` that already has the include under another spelling,**
   such as `[include ./addons/iheater_remote.cfg]`, an absolute path, or
   `addons/*.cfg`. Expect `skip`, with no second include added (Task 3:
   `test_has_include_dot_slash`, `test_has_include_absolute`,
   `test_has_include_glob`).
2. **An install followed by an uninstall** should leave `printer.cfg` and
   `moonraker.conf` byte-for-byte as they were, including on CRLF files
   (Task 3 round-trip tests; Task 5 `test_uninstall_restores_files`).
3. **Running the installer with no terminal** (piped or from a script)
   should take every default and never hang or crash under `set -eu`
   (Task 4: every smoke test runs with stdin from a pipe).
4. **A printer that is printing when the installer runs:** Moonraker still
   restarts, Klipper does not, and `FIRMWARE_RESTART` is advised (Task 4
   `test_install_while_printing`).
5. **A host with no git `origin`:** the `[update_manager]` block is skipped
   with a warning, and the rest of the install completes (Task 4
   `test_install_without_origin`).

---

### Task 1: Helper skeleton, config versions and the changelog

**Files:**
- Create: `scripts/iheater_config.py`, `scripts/test_iheater_config.py`,
  `klipper/CONFIG_CHANGES.md`
- Modify: `klipper/iheater_remote.cfg` (header), `AGENTS.md` (gate and
  layout)

**Interfaces:**
- Produces, in `scripts/iheater_config.py`:
  - `config_version(text: str) -> int`
  - `parse_changes(text: str) -> dict` (version to a list of lines)
  - `format_changes(text: str, frm: int, to: int) -> str`
  - `main(argv: list) -> int`: dispatches subcommands, catches `ValueError`
    and `OSError` into stderr with exit 1, and returns the exit code.
  - `_read(path) -> str` and `_write(path, text) -> None`, which follow the
    newline rules.
  - `_eol(text) -> str`
- Produces on the CLI: `version FILE` and `changes CHANGES_MD FROM TO`.
- Tests import the module the same way `klipper/test_heater_virtual.py`
  does: `sys.path.insert(0, dirname(abspath(__file__)))`.

- [ ] **Step 1: Write the failing tests** in
  `scripts/test_iheater_config.py`, class `VersionTests`:
  - `test_version_present`: `config_version("# t\n# CONFIG_VERSION: 3\n[x]\n") == 3`.
  - `test_version_absent_is_zero`: `config_version("[heater_virtual chamber]\n") == 0`.
  - `test_version_malformed_is_zero`: `config_version("# CONFIG_VERSION: abc\n") == 0`.
  - `test_version_crlf`: `config_version("# CONFIG_VERSION: 2\r\n") == 2`.

  Class `ChangesTests`, using this fixture:
  ```
  # Config changes

  ## 3
  - Rename `foo` to `bar`.
    Edit your copy.

  ## 2
  - Drop `baz`.

  ## 1
  - First versioned config. No changes to make.
  ```
  Tests:
  - `test_parse_changes`: the result is
    `{3: ["- Rename `foo` to `bar`.", "  Edit your copy."], 2: ["- Drop `baz`."], 1: [...]}`.
  - `test_format_between`: `format_changes(md, 1, 3)` gives
    `"version 2\n- Drop `baz`.\nversion 3\n- Rename `foo` to `bar`.\n  Edit your copy.\n"`,
    oldest first.
  - `test_format_from_zero_includes_1`: `format_changes(md, 0, 1)` starts
    with `"version 1\n"`.
  - `test_format_nothing_when_not_older`: `format_changes(md, 3, 3) == ""`
    and `format_changes(md, 4, 3) == ""`.
  - `test_format_missing_version_prints_nothing`: with `## 2` deleted from
    the fixture, `format_changes(md, 1, 3)` holds only version 3.
  - `test_text_outside_bullets_ignored`: a paragraph line under `## 2` is
    left out.
  - `test_cli_version`: write a temp file, call
    `main(["version", path])`, and capture stdout (`contextlib.redirect_stdout`).
    Expect `"1\n"` and return value 0.
  - `test_cli_missing_file_exits_1`: `main(["version", "/no/such"]) == 1`,
    with a message on stderr.
  - `test_shipped_cfg_has_version_1`:
    `config_version(<repo>/klipper/iheater_remote.cfg) == 1`.
  - `test_shipped_changes_cover_shipped_version`:
    `config_version(shipped cfg)` is a key of
    `parse_changes(<repo>/klipper/CONFIG_CHANGES.md)`.

- [ ] **Step 2: Run the tests to watch them fail.**
  Run: `python -m unittest discover -s scripts -p "test_*.py"`
  Expected: an ImportError or errors, because `iheater_config` does not
  exist yet.

- [ ] **Step 3: Implement.**
  - `config_version` matches the first line of
    `^#\s*CONFIG_VERSION:\s*(\d+)\s*$` (multiline).
  - `parse_changes`:
    - `^## (\d+)\s*$` starts a version;
    - a line starting `- ` starts a bullet;
    - a non-blank line starting with whitespace continues the previous
      bullet and is kept verbatim;
    - every other line is ignored.
  - `format_changes` walks the versions `frm+1` up to `to`.
  - The CLI uses `argparse` subparsers. `changes` takes FROM and TO as
    integers.
  - In `klipper/iheater_remote.cfg`, insert these two lines after the first
    header line:
    ```
    # CONFIG_VERSION: 1
    # ./install.sh compares this number to offer updates. Do not edit it.
    ```
  - `klipper/CONFIG_CHANGES.md`:
    - a `# iheater_remote.cfg changes` title;
    - two sentences: each version lists what a user must change in their
      copy, and `./install.sh` shows the entries between the installed and
      shipped versions;
    - `## 1` with `- First versioned config. No changes to make.`
  - In `AGENTS.md`:
    - add the fifth gate line;
    - add Layout bullets for `install.sh` / `uninstall.sh` (repo root) and
      `scripts/` (installer helper and tests);
    - change "Run all four" to "Run all five".

- [ ] **Step 4: Run the tests to watch them pass.**
  Run: the three Python gates.
  Expected: all OK.

- [ ] **Step 5: Commit** with the message
  `feat(install): config version helper and changelog`.

---

### Task 2: Token, get-key, set-key and carry-over

**Files:**
- Modify: `scripts/iheater_config.py`, `scripts/test_iheater_config.py`

**Interfaces:**
- Consumes `_read`, `_write`, `_eol` and `main` from Task 1.
- Produces:
  - `check_token(token: str) -> bool`
  - `get_key(text: str, key: str) -> Optional[str]`: the active value in
    the first `[heater_virtual ...]` section, or None.
  - `set_key(text: str, key: str, value: str) -> str`
  - `carry(old: str, new: str) -> Tuple[str, List[str], Optional[str]]`:
    returns the new text, the keys carried (in order `remote_token`,
    `sensor`, `max_target`), and OLD's section name if it is not `chamber`
    (else None).
- CLI:
  - `check-token TOKEN`
  - `get-key FILE KEY`: prints the value, exit 0; exit 1 with no message
    when the key is not set.
  - `set-key FILE KEY VALUE`: rewrites FILE.
  - `carry OLD NEW`: rewrites NEW, prints `carried <key>` for each key,
    then `renamed <name>` when the name differs.
- Line rules:
  - The section header matches `^\[heater_virtual\s+([^\]\s]+)\s*\]`.
  - A section runs to the next line starting with `[`.
  - An active key line matches `^KEY\s*[:=]` at column 0,
    case-insensitively. A leading space would make the line a continuation
    in Klipper's parser.
  - A commented key line matches `^#\s*KEY\s*[:=]`.
  - A value is everything after the first `:` or `=`, stripped, and is
    carried verbatim, inline comment included.
  - A written line is `KEY: VALUE`.

- [ ] **Step 1: Write the failing tests,** classes `TokenTests` and
  `KeyTests`.
  - Tokens:
    - `test_token_valid`: `check_token("abcd-efgh")` and
      `check_token("ABCDEFGH")` are True.
    - `test_token_invalid`: False for `"ABCD-EFGI"` (I), `"ABCDEFG"` (7
      characters), `""` and `"ABCD EFGH"`.
  - set-key, with this shipped-style fixture:
    ```
    [heater_virtual chamber]
    sensor: temperature_sensor chamber_hexa
    #max_target: 80
    # Copy from the Remote's Setup page.
    #remote_token: XXXX-XXXX

    [gcode_macro M141]
    ```
    - `test_set_key_replaces_active`: setting `sensor` to `temperature_sensor x`
      changes only line 2.
    - `test_set_key_uncomments`: setting `remote_token` to `ABCD-EFGH`
      turns line 5 into `remote_token: ABCD-EFGH`, with no line added.
    - `test_set_key_inserts_after_header`: setting `foo` to `1` puts
      `foo: 1` on line 2.
    - `test_set_key_active_wins_over_comment`: with both `#sensor: a` and
      `sensor: b`, only the active line changes.
    - `test_set_key_ignores_other_sections`: a `sensor:` line in a second
      `[heater_virtual other]` section is untouched.
    - `test_set_key_no_section_raises`: `ValueError`.
    - `test_set_key_keeps_crlf`: on the CRLF version of the fixture, every
      line still ends in `\r\n`.
    - `test_indented_key_is_not_active`: `"  sensor: x"` counts as a
      continuation, not an active key.
  - get-key:
    - `test_get_key`: `get_key(fixture, "sensor") == "temperature_sensor chamber_hexa"`.
    - `get_key(fixture, "remote_token") is None` (commented only).
  - Carry-over:
    - `test_carry_values`: OLD has an active `remote_token: abcd-efgh  # mine`,
      `sensor: temperature_sensor other` and `max_target: 70`. NEW is the
      fixture. The result has those three lines verbatim, and carried is
      `["remote_token", "sensor", "max_target"]`.
    - `test_carry_skips_commented`: OLD with only `#max_target: 60`. NEW
      keeps `#max_target: 80`, and `max_target` is not in carried.
    - `test_carry_reports_rename`: OLD's section `[heater_virtual hexa]`
      gives renamed `"hexa"`, and NEW's header is still `chamber`.
    - `test_carry_cli_output`: stdout is `carried remote_token\nrenamed hexa\n`
      for an OLD with only a token and a renamed section.

- [ ] **Step 2: Run the tests to watch them fail.**
  Run: `python -m unittest discover -s scripts -p "test_*.py"`
  Expected: the new tests fail with an AttributeError; the Task 1 tests
  pass.

- [ ] **Step 3: Implement** the four functions and the CLI commands, with
  `carry` built on `get_key` and `set_key`.

- [ ] **Step 4: Run the tests to watch them pass.**
  Run: the three Python gates.
  Expected: all OK.

- [ ] **Step 5: Commit** with the message
  `feat(install): token check and config carry-over`.

---

### Task 3: Includes, sections and the print state

**Files:**
- Modify: `scripts/iheater_config.py`, `scripts/test_iheater_config.py`

**Interfaces:**
- Produces:
  - `include_kind(printer_text: str, config_dir: str, target: str) -> Optional[str]`:
    returns `"exact"`, `"glob"` or None.
  - `add_include(printer_text: str, target: str) -> str`
  - `remove_include(printer_text: str, target: str) -> str`
  - `has_section(text: str, name: str) -> bool`
  - `remove_section(text: str, name: str) -> str`
  - `append_block(text: str, block: str) -> str`
  - `print_state(json_text: str) -> str`: raises ValueError when there is
    no state.
- CLI:
  - `has-include PRINTER_CFG CONFIG_DIR TARGET`: exit 0 and prints the
    kind, or exit 1.
  - `add-include PRINTER_CFG TARGET`
  - `remove-include PRINTER_CFG TARGET`
  - `has-section CONF NAME`
  - `remove-section CONF NAME`
  - `append-block CONF TEMPLATE [NAME=VALUE ...]`: replaces each `<NAME>`
    in TEMPLATE, then appends.
  - `print-state`: reads JSON on stdin.
- Rules:
  - **Active include line:** matches `^\[include\s+(.+?)\s*\](\s*[#;].*)?$`,
    with `include` case-insensitive.
  - **`include_kind`:**
    - Join each spec onto `config_dir` with `posixpath.join`, so an
      absolute spec wins, as in Klipper.
    - `posixpath.normpath` both sides and split on `/`.
    - A spec covers the target when the component counts are equal and
      each pair matches under `fnmatch.fnmatchcase`. `*` must not cross
      `/`.
    - The kind is `"exact"` when the spec text, stripped, equals the target
      string. Otherwise it is `"glob"`, which also covers a path that
      normalises to the target.
  - **`add_include`:**
    - Insert `[include TARGET]` on the line after the last active include
      line, or as line 1 when there is none.
    - Add only that one line, with nothing around it. If the line before is
      the last line and has no ending, give it one.
    - A file whose only content is a SAVE_CONFIG block (`#*#` lines) gets
      the include at line 1.
  - **`remove_include`:** removes every active line whose spec, stripped,
    equals TARGET.
  - **Sections:**
    - A header is a line whose stripped text is `[NAME]`, after collapsing
      runs of spaces inside the brackets.
    - `remove_section` removes the header, every line up to the next line
      starting with `[`, and the blank lines directly above the header.
  - **`append_block`:**
    - First make sure TEXT ends with a line ending (unless empty).
    - Then add one blank line and BLOCK, with BLOCK's line endings
      converted to TEXT's.
    - So `remove_section(append_block(t, b), name)` must equal `t` for a
      `t` that ends in a line ending.
  - **`print_state`:** `json.loads(s)["result"]["status"]["print_stats"]["state"]`.

- [ ] **Step 1: Write the failing tests,** classes `IncludeTests`,
  `SectionTests` and `PrintStateTests`. Use `CONFIG_DIR = "/home/pi/printer_data/config"`
  and `TARGET = "addons/iheater_remote.cfg"`.
  - Includes:
    - `test_has_include_exact`: kind `"exact"`.
    - `test_has_include_glob`: `[include addons/*.cfg]` gives `"glob"`.
    - `test_glob_does_not_cross_dirs`: `[include *.cfg]` gives None.
    - `test_has_include_dot_slash`: `[include ./addons/iheater_remote.cfg]`
      gives `"glob"`.
    - `test_has_include_absolute`:
      `[include /home/pi/printer_data/config/addons/iheater_remote.cfg]`
      gives `"glob"`.
    - `test_commented_include_ignored`:
      `#[include addons/iheater_remote.cfg]` gives None.
    - `test_include_with_trailing_comment`:
      `[include addons/iheater_remote.cfg]  # chamber` gives `"exact"`.
  - add-include:
    - `test_add_after_last_include`: given
      `"[include a.cfg]\n[include b.cfg]\n\n[printer]\n"`, the result's
      line 3 is the new include and the rest is unchanged.
    - `test_add_at_top_when_none`: `"[printer]\nkinematics: corexy\n"`
      becomes `"[include addons/iheater_remote.cfg]\n[printer]\n..."`.
    - `test_add_above_save_config`: a file of `[printer]` plus a SAVE_CONFIG
      block (`#*# <---------------------- SAVE_CONFIG ---------------------->`
      and `#*# [probe]`) gets the include as line 1.
    - `test_add_last_line_no_newline`: `"[include a.cfg]"` becomes
      `"[include a.cfg]\n[include addons/iheater_remote.cfg]\n"`.
    - `test_add_keeps_crlf`.
    - `test_add_remove_round_trip`: for each fixture above,
      `remove_include(add_include(t, T), T) == t`. Skip the no-newline case,
      where the added ending stays.
  - Sections:
    - `test_has_section`: `[iheater_remote]` is True; `[iheater_remote_x]`
      is False; `[update_manager  iheater-remote]` (two spaces) matches
      `update_manager iheater-remote`.
    - `test_remove_section_middle`: the section between `[server]` and
      `[authorization]` is gone, and both neighbours are intact.
    - `test_remove_section_last`: a section at the end of the file is
      removed.
    - `test_append_remove_round_trip`: for LF and CRLF fixtures and the
      empty string.
  - Print state:
    - `test_print_state`: `'{"result":{"status":{"print_stats":{"state":"printing"}}}}'`
      gives `"printing"`.
    - `test_print_state_bad`: invalid JSON or a missing key raises
      ValueError.
    - The CLI exits 1 in both of those cases.
  - `test_cli_append_block_substitutes`: a template line
    `path: <INSTALL_PATH>`, given `INSTALL_PATH=/home/pi/iHeater-Remote`,
    is written as `path: /home/pi/iHeater-Remote`. A value containing `&`
    or `|` is written verbatim.

- [ ] **Step 2: Run the tests to watch them fail.**
  Run: `python -m unittest discover -s scripts -p "test_*.py"`
  Expected: the new tests fail; the earlier ones pass.

- [ ] **Step 3: Implement** the functions and the CLI commands. Catch
  `KeyError`, `TypeError` and `json.JSONDecodeError` in `print_state` and
  raise ValueError.

- [ ] **Step 4: Run the tests to watch them pass.**
  Run: the three Python gates.
  Expected: all OK.

- [ ] **Step 5: Commit** with the message
  `feat(install): include and section editing`.

---

### Task 4: `common.sh`, `install.sh` and the install smoke tests

**Files:**
- Create:
  - `scripts/common.sh`
  - `install.sh` (mode 755: `git update-index --chmod=+x`)
  - `scripts/moonraker-component.conf`
  - `scripts/moonraker-update-manager.conf`
  - `scripts/test_install.py`

**Interfaces:**
- Consumes every helper CLI command from Tasks 1–3, run as
  `"$PYTHON_BIN" "$INSTALL_PATH/scripts/iheater_config.py" ...` through a
  shell function `helper`.
- Produces in `scripts/common.sh`, which `install.sh` and `uninstall.sh`
  source:
  - the path variables from Global Constraints;
  - `STAMP`;
  - the output helpers `section`, `ok`, `skip`, `warn`, `err`, `step` and
    `note`;
  - `ask PROMPT DEFAULT`;
  - `ask_value PROMPT`: prints the line read, or empty at end of input;
  - `helper ARGS...`;
  - `backup FILE`: `cp -p` to `FILE.bak-$STAMP`, once per file per run;
  - `preflight`;
  - change flags `CHANGED_LINKS`, `CHANGED_MOONRAKER` and `CHANGED_KLIPPER`,
    each 0 or 1;
  - `restart_services`;
  - `link_target LINK`: prints the `readlink -f` of an existing symlink.
- The templates:
  - `scripts/moonraker-component.conf`: `[iheater_remote]` on line 1, then
    comment lines only. Comments after the header are removed with the
    section.
  - `scripts/moonraker-update-manager.conf`: the update manager block from
    the spec, with `path: <INSTALL_PATH>` and `origin: <ORIGIN>`, and any
    comments placed after the header.
- Shell details:
  - **Service check:**
    `systemctl list-unit-files --no-legend klipper.service 2>/dev/null | grep -q klipper.service`
    (the same for moonraker). A missing unit warns once and marks that
    restart as skipped.
  - **Print state:**
    `curl -s -m 5 "http://localhost:${MOONRAKER_PORT}/printer/objects/query?print_stats" | helper print-state`.
    A failure gives an empty state.
  - **Restarts:** `sudo systemctl restart moonraker`, then
    `sudo systemctl restart klipper`, following the spec's Restart rules.
    The Klipper prompt text is "Restart Klipper now?".
  - **The origin:**
    `git -C "$INSTALL_PATH" remote get-url origin 2>/dev/null`. Empty means
    skip the update manager block with a warn.
  - **Under `set -eu`:** every predicate call goes in an `if` or uses
    `|| true`, so an exit 1 never ends the script.
  - **Missing `printer.cfg` or `moonraker.conf`:** `warn`, then skip that
    step.
  - **The macro check:**
    `grep -rniE --include='*.cfg' '^\[gcode_macro (M141|M191)\]' "$CONFIG_DIR"`.
    Drop the lines for `$ADDON_CFG` and for `*.bak-*` files; report each
    remaining one as a `warn` with `file:line`.
  - **The token prompt:** repeat until the input is blank or
    `helper check-token` passes. At end of input, take blank.
  - **The old-config prompt:** the text is "Replace it with version N? Your
    token, sensor and max_target are kept." The default is `n`.

The smoke test (`scripts/test_install.py`, class `InstallSmokeTests`):

- **Setup:**
  - `setUpClass` skips the class unless `shutil.which("bash")` exists and
    `os.symlink` works in a temp directory.
  - Each test builds, under a `tempfile.TemporaryDirectory`:
    - `klipper/klippy/extras/` and `moonraker/moonraker/components/`;
    - `printer_data/config/printer.cfg`, containing `[printer]\n`, then a
      SAVE_CONFIG block of two `#*#` lines;
    - `printer_data/config/moonraker.conf`, containing `[server]\nhost: 0.0.0.0\n`;
    - a `bin/` directory of stub scripts, written with LF endings and made
      executable.
- **The stubs:**
  - `systemctl`: `list-unit-files` echoes the unit asked for; `restart X`
    appends `restart X` to `$STUB_LOG`.
  - `sudo`: runs `exec "$@"`.
  - `curl`: echoes `$STUB_PRINT_JSON` when it is set, else exits 7.
- **Running a script,** with `subprocess.run(["bash", script], input=answers, ...)`
  and the environment:
  - `KLIPPER_PATH`, `MOONRAKER_PATH` and `PRINTER_DATA` as POSIX paths
    (`Path.as_posix()`);
  - `PYTHON_BIN=Path(sys.executable).as_posix()`;
  - `PATH=<bin>:` followed by the current PATH;
  - `MSYS=winsymlinks:nativestrict`;
  - `NO_COLOR=1`;
  - `STUB_LOG`.
  `answers` defaults to `""`, which takes every default.
- **Tests:**
  - `test_fresh_install`, with `STUB_PRINT_JSON` set to the state
    `standby`. Expect:
    - exit 0;
    - both links are symlinks whose `realpath` is the repo file;
    - `addons/iheater_remote.cfg` exists with `config_version == 1` and a
      commented `#remote_token:`;
    - `printer.cfg` line 1 is the include;
    - `moonraker.conf` has `[iheater_remote]` and
      `[update_manager iheater-remote]`, with `path:` equal to the repo's
      POSIX path and `origin:` equal to the output of
      `git remote get-url origin`;
    - the log is `restart moonraker\nrestart klipper\n`.
  - `test_rerun_changes_nothing`: install twice. The second run leaves
    every file byte-identical and adds nothing to the log.
  - `test_install_sets_token`: answers `"abcd-efgh\n\n"` give
    `remote_token: abcd-efgh` in the cfg.
  - `test_install_rejects_bad_token_then_accepts_blank`: answers
    `"bad\n\n\n"` leave the token commented, and the output contains
    `warn`.
  - `test_install_while_printing`: with the state `printing`, the log is
    `restart moonraker\n` only, and stdout contains `FIRMWARE_RESTART`.
  - `test_install_state_unreadable`: with `STUB_PRINT_JSON` unset, the log
    has no `restart klipper` (the default is no).
  - `test_existing_macro_blocks_include`: with `[gcode_macro M141]\ngcode:\n`
    appended to `printer.cfg`, there is no include line, and stdout
    contains `printer.cfg:` with a line number.
  - `test_upgrade_old_config`:
    - First install, then take the version line out of the installed cfg
      and set `remote_token: ABCD-EFGH` and `sensor: temperature_sensor other`.
    - Re-run with answers `"y\n\n"`.
    - The cfg has version 1 and both values, and a
      `iheater_remote.cfg.bak-*` holds the edited file.
  - `test_upgrade_declined_keeps_file`: the same, with answers `"\n"`. The
    file is unchanged, and stdout contains `CONFIG_CHANGES.md`.
  - `test_install_without_origin`: add a `git` stub to `bin/` that exits 1.
    There is no `[update_manager iheater-remote]` section, `[iheater_remote]`
    is present, and the exit is 0.
  - `test_regular_file_in_extras_is_moved_aside`: a regular
    `heater_virtual.py` in extras is replaced by the link, and a
    `heater_virtual.py.bak-*` exists.

- [ ] **Step 1: Write the smoke tests** above, plus the two templates. No
  script exists yet.
- [ ] **Step 2: Run the tests to watch them fail.**
  Run: `python -m unittest discover -s scripts -p "test_install.py" -v`
  Expected: failures (no `install.sh`), not skips. If the class skips, fix
  the symlink probe before going on.
- [ ] **Step 3: Write `scripts/common.sh` and `install.sh`,** following the
  spec's install steps 1–7 in order. `install.sh` starts:
  `INSTALL_PATH="${INSTALL_PATH:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"; source "$INSTALL_PATH/scripts/common.sh"`.
- [ ] **Step 4: Run the tests to watch them pass.**
  Run: `bash -n install.sh scripts/common.sh`, then the three Python gates.
  Expected: no syntax errors, and all OK.
- [ ] **Step 5: Commit** with the message
  `feat(install): install.sh for the Klipper and Moonraker parts`.

---

### Task 5: `uninstall.sh`, its smoke tests and the docs

**Files:**
- Create: `uninstall.sh` (mode 755)
- Modify: `scripts/test_install.py`, `klipper/README.md` (Install),
  `moonraker/README.md` (Install)

**Interfaces:**
- Consumes everything `scripts/common.sh` provides (Task 4), and the
  helpers `has-section`, `remove-section`, `has-include` (the printed kind)
  and `remove-include`.
- Uninstall order and prompts follow the spec's uninstall steps 1–5:
  - "Remove the include from printer.cfg?" defaults to `y`.
  - "Delete addons/iheater_remote.cfg? A backup is kept." defaults to `n`.
- A symlink is removed only when `link_target` lies under
  `readlink -f "$INSTALL_PATH"`.

- [ ] **Step 1: Write the failing smoke tests** in class
  `UninstallSmokeTests`, using the Task 4 fixture builder and runner:
  - `test_uninstall_restores_files`:
    - Snapshot `printer.cfg` and `moonraker.conf`, install, then uninstall
      with defaults.
    - Both files equal their snapshots byte-for-byte.
    - Both links are gone, and the cfg is still present.
    - The log ends with `restart moonraker\nrestart klipper\n`.
  - `test_uninstall_restores_crlf_files`: the same, with fixtures written
    with `\r\n`.
  - `test_uninstall_deletes_cfg_on_yes`: answers `"\ny\n\n"`. The cfg is
    gone and a `iheater_remote.cfg.bak-*` exists.
  - `test_uninstall_leaves_glob_include`: `printer.cfg` has
    `[include addons/*.cfg]` and no exact line. After install and
    uninstall, the glob line is still there, and stdout mentions it.
  - `test_uninstall_leaves_foreign_link`: `extras/heater_virtual.py` is a
    symlink to a file outside the repo. It is still a link to that file
    after uninstall.
  - `test_uninstall_twice`: the second run exits 0 and changes no file.
- [ ] **Step 2: Run the tests to watch them fail.**
  Run: `python -m unittest discover -s scripts -p "test_install.py" -v`
  Expected: the new tests fail (no `uninstall.sh`); the Task 4 tests pass.
- [ ] **Step 3: Write `uninstall.sh`.**
- [ ] **Step 4: Update the docs.**
  - `klipper/README.md` Install:
    - `git clone`, then `cd ~/iHeater-Remote && ./install.sh`;
    - one paragraph on what it does and asks;
    - the environment variables;
    - `./uninstall.sh`;
    - updating: `git pull` (or Mainsail's update manager), then re-run
      `./install.sh` when [`CONFIG_CHANGES.md`](CONFIG_CHANGES.md) lists a
      new version.
    - Keep the manual steps under a "Manual install" subheading, with the
      copy going to `~/printer_data/config/addons/` and the include
      `[include addons/iheater_remote.cfg]`. Replace "Delete any existing
      `M141` and `M191` macros" with the fact that Klipper refuses
      duplicate sections, so the old ones must go.
  - `moonraker/README.md` Install: "`./install.sh` in the repo does this.
    By hand:", followed by the current steps.
- [ ] **Step 5: Run the gates.**
  Run: `bash -n uninstall.sh`, then all five gates (including both `pio`
  gates, once). Redirect `pio` output to a file and read its tail.
  Expected: everything passes.
- [ ] **Step 6: Commit** with the message
  `feat(install): uninstall.sh and install docs`.

---

## Bench check (the user, on hestia)

The user runs these after the merge:

1. `./install.sh`
2. Re-run it; every step should say `skip`.
3. `./uninstall.sh`
4. `./install.sh` again.
5. Delete the `CONFIG_VERSION` line from the installed cfg, re-run, and
   check the upgrade prompt and carry-over.
6. Check Mainsail's update manager lists `iheater-remote`.
