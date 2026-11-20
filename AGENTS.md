# AGENTS.md

iHeater Remote is local-only ESP32-C3 firmware (PlatformIO, Arduino-ESP32
2.0.17) that drives an iHeater controller over one GPIO pulse train, plus a
Klipper Python module. The Remote has a small local web UI and one control
source at a time: Manual, or one `heater_virtual` Klipper heater that it
follows through its own Moonraker WebSocket client, paired by
`remote_token`. There is no cloud or portal.

## Gate commands

Run all three before committing. Redirect long `pio` output to a file and
read its tail.

```bash
pio run -e esp32c3-super-mini-remote
pio test -e esp32c3-super-mini-remote -f test_remote_policy --without-uploading --without-testing
python -m unittest discover -s klipper -p "test_*.py"
```

There is no host C++ compiler, so the policy tests are build-only: a
compile failure is a failing test, but the tests are not run on a host.

## Layout

- `src/standalone/`: firmware (web UI, settings and migration,
  `MoonrakerLink`, TLS roots in `MoonrakerCa.h`).
- `lib/remote-policy/`: pure decision logic, with tests in
  `test/test_remote_policy/`.
- `klipper/`: the `heater_virtual` module, its config and unit tests.
- `tools/fake_moonraker/`: fake Moonraker server for bench testing.
- `docs/design/`: design and plan documents.

## Rules

- Write Canadian English in code comments and documentation.
- Put pure decisions in `lib/remote-policy/` and cover them with tests.
- Never flash or upload to a device without asking first.
- Policy files in `lib/remote-policy/` use CRLF line endings. Keep each
  file's existing line endings.
- Put design and plan documents in `docs/design/`.
- Commit messages end with a blank line, then a `Co-Authored-By` trailer
  naming the AI model that wrote the change.
