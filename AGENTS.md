# AGENTS.md

iHeater Remote is local-only ESP32-C3 firmware (PlatformIO, Arduino-ESP32
3.3.12 via pioarduino 55.03.312-1) that drives an iHeater controller over one
GPIO pulse train, plus a Klipper Python module. The Remote has a small local
web UI and one control source at a time: Manual, or one
`heater_iheater_remote` Klipper heater that it follows through its own
Moonraker WebSocket client, paired by `remote_token`. There is no cloud or
portal.

## Gate commands

Run all five before committing. Redirect long `pio` output to a file and
read its tail.

```bash
pio run -e esp32c3-super-mini-remote
export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native
python -m unittest discover -s klipper -p "test_*.py"
python -m unittest discover -s moonraker -p "test_*.py"
python -m unittest discover -s scripts -p "test_*.py"
```

Policy tests run on the PC with MinGW from `~/scoop/apps/mingw`.

The pioarduino platform needs Python 3.10 to 3.14 in `~/.platformio/penv`.
If PlatformIO runs from a different Python, pioarduino deletes and rebuilds
that penv. ESP-IDF's tools refuse to run when `MSYSTEM` is set, as it is in
Git Bash, so run `pio run` from PowerShell or `cmd`, or from a shell without
`MSYSTEM`. The first build downloads the toolchain and framework.

## Layout

- `src/standalone/`: firmware (web UI, settings and migration,
  `MoonrakerLink`, TLS roots in `MoonrakerCa.h`).
- `lib/remote-policy/`: pure decision logic, with tests in
  `test/test_remote_policy/`.
- `lib/modbus-rtu/`: copy of the iHeater's Modbus slave core; do not edit here.
- `klipper/`: the `heater_iheater_remote` module, its config and unit tests.
- `moonraker/`: the iheater_remote Moonraker component and its unit tests.
- `install.sh`, `uninstall.sh` (repo root): install and remove the Klipper and
  Moonraker parts on a printer host.
- `scripts/`: the installer's Python config helper and its tests.
- `tools/fake_moonraker/`: fake Moonraker server for bench testing.
- `docs/design/`: design and plan documents.

## Rules

- Write Canadian English in code comments and documentation.
- Put pure decisions in `lib/remote-policy/` and cover them with tests.
- Never flash or upload to a device without asking first.
- Use LF line endings (`.gitattributes` enforces it). The vendored
  `lib/modbus-rtu/` copy is left as it came.
- Put design and plan documents in `docs/design/`.
- iHeater flashing: never mass erase or erase above page 29, never send
  write/read-protect or option-byte commands, and send `0xB007` at most once
  per job.
- Commit messages end with a blank line, then a `Co-Authored-By` trailer
  naming the AI model that wrote the change.
