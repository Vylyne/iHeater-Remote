# iheater_remote Moonraker component

The iHeater Remote reports its readings (air and element temperature, power,
fan, state, fault) to Klipper through Moonraker. This component receives each
report and forwards it to the `heater_iheater_remote` module.

## What it does

The Remote calls the JSON-RPC method `server.iheater_remote.report` on its
existing Moonraker WebSocket. The component checks the report with the same
rules as `klipper/heater_iheater_remote.py`, and checks that `remote_token` is eight
characters from `0-9A-HJKMNP-TV-Z`. It then forwards the report, unknown
fields included, to the Klippy webhooks endpoint `heater_iheater_remote/report`.
Klipper picks the `heater_iheater_remote` section by `remote_token`.

Errors:

- A bad report gives a 400 error, and nothing reaches Klipper.
- When Klippy is not connected, it gives a 503 error ("Klippy Host not
  connected").
- Klipper's own errors, such as an unknown `remote_token`, pass through
  unchanged.

## Install

`./install.sh` in the repo does this. By hand:

```bash
ln -s ~/iHeater-Remote/moonraker/iheater_remote.py ~/moonraker/moonraker/components/
```

Add this to `moonraker.conf`, then restart Moonraker:

```ini
[iheater_remote]
```

## How to tell it works

The Remote's diagnostics page shows "Klipper plugin: found".

## Tests

```bash
python -m unittest discover -s moonraker -p "test_*.py"
```

The tests load the component under a fake Moonraker package, so they need
no Moonraker install.

## API check

The interfaces used here (`register_endpoint`, `RequestType`,
`WebRequest(endpoint, args, ...)`, `klippy_connection.is_connected()` and
`async request()`, and the "Klippy Host not connected" 503 error) were
checked against Moonraker master at commit
`b559b9a81161f958a43c2f527b6ad96f73310d5d` on 2026-11-20.
