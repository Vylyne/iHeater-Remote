# Two-way Klipper link — design

Status: approved · 2026-11-20

## Intent

Today the link runs one way. The Remote follows one `heater_virtual`
through its own Moonraker WebSocket client, paired by `remote_token`, and
reads that heater's target. Klipper's chamber temperature comes from a
separate `sensor:`, and the heater's `power` is invented (1 whenever a
target is set).

This change sends what the iHeater reports back into Klipper, so Mainsail
and Fluidd (and any UI that reads Klipper objects) show the truth:

- the chamber temperature is the iHeater's air temperature;
- `power` is the iHeater's real heater power;
- the heating element's temperature is available as its own temperature
  sensor, which graphs in degrees;
- `printer["heater_virtual <name>"].linked` says whether the Remote and the
  iHeater are both reachable right now.

Why: the user wants Klipper's view of the chamber heater to be accurate, and
wants their own `START_PRINT` to be able to refuse to start when the heater
cannot be reached.

## Goals

1. The Remote, in UART mode, pushes iHeater readings to Klipper.
2. `heater_virtual` uses the air reading as its temperature and the real
   power as its `power`, and publishes `linked`.
3. A sensor type that exposes the element (or the air) reading as an
   ordinary `[temperature_sensor ...]`.
4. Light traffic when nothing changes, especially when idle.

## Non-goals

- Stopping, pausing or cancelling a print when the link drops mid-print.
  The user was explicit: a dropped link never kills a print.
- Link checks in the shipped `M141` / `M191`, and any `require_link`
  option. Klipper always accepts a new target, linked or not, and
  `M141`/`M191` stay as they are. The user checks `linked` in their own
  `START_PRINT`. Alerts for high-stakes prints belong in another channel,
  not in a refusal to set a target.
- HelixScreen changes. HelixScreen classifies Klipper objects by prefix and
  only offers `heater_generic` objects as the chamber heater, so
  `heater_virtual` does not appear there. A `[temperature_sensor]` using the
  new sensor type does appear in its chamber sensor list (display only).
  Making HelixScreen accept `heater_virtual` is an upstream change for
  another day.
- Moonraker update-manager integration, and installers.
- The Ethernet board port (a separate project that follows this one).
- Any change to how the Remote reads the target or pairs.

## Anti-goals

- Readings must not go through G-code (`printer.gcode.script`). G-code runs
  one command at a time, and `M191`'s `TEMPERATURE_WAIT` holds the queue, so
  the readings it waits on could never arrive.
- The Remote must keep working one-way when the Moonraker plugin is not
  installed. A missing plugin is a status message, never a failure of
  heating or pairing.
- Klipper must never reject a target, or fail a running print, because the
  link is down. A target set while unlinked is kept, and the Remote applies
  it once the link comes back (as it does today). Refusing would, for
  example, stop a print hours in because a cooldown target could not be set.
- No new port, account or credential. Reports ride the connection the
  Remote already has.

## Constraints

- Klipper side: Python, installed as `klippy/extras/heater_virtual.py`.
  Tested with `python -m unittest discover -s klipper -p "test_*.py"` using
  fakes, as `klipper/test_heater_virtual.py` does today.
- Moonraker side: a new component file, enabled by an `[iheater_remote]`
  section in `moonraker.conf`. It lives in this repo under `moonraker/`,
  with its own unit tests using fakes, run by the same kind of
  `python -m unittest` command (add it to the gate commands in AGENTS.md).
- Firmware: Arduino-ESP32 2.0.17 on the ESP32-C3 today. Pure decisions
  (when to send, what `linked` should be) go in `lib/remote-policy/` with
  native tests.
- Canadian English in comments and docs.

## Approaches considered

1. **Moonraker plugin passes reports to Klipper (chosen).** A Moonraker
   component adds one JSON-RPC method. The Remote calls it over its existing
   WebSocket. The component forwards each report to an endpoint that
   `heater_virtual` registers with Klipper's webhooks, bypassing G-code.
   Works with the existing API key and TLS. Costs one more file to install.
2. **Klipper module runs its own server for the Remote.** No Moonraker
   plugin, but a second port, a second set of connection settings in Setup,
   and its own authentication. Rejected: more moving parts for no gain.
3. **G-code commands.** Rejected: blocked behind `M191` (see Anti-goals).

The user chose 1.

## Design

### Data flow

```
iHeater --UART--> Remote --WebSocket JSON-RPC--> Moonraker [iheater_remote]
                                                     |
                                    Klippy webhooks endpoint
                                                     v
                                    heater_virtual <name> (matched by token)
                                         |                 |
                              temperature/power/linked   sensor_type iheater_remote
```

### The report

The Remote sends one report per heater it follows (there is only one). It
calls the Moonraker method `server.iheater_remote.report` with:

| Field | Meaning |
|---|---|
| `remote_token` | The pairing token, 8 characters, no hyphen. Selects the heater. |
| `link` | The Remote's iHeater link: `up`, `waiting`, `restarting` or `no-reply`. |
| `air` | Air temperature in °C (the iHeater's `airT10` / 10), or `null`. |
| `element` | Heating element temperature in °C (`heaterT10` / 10), or `null`. |
| `power` | Real heater power, 0.0–1.0 (`pwmPermille` / 1000), or `null`. |
| `fan` | `true` or `false`. |
| `state` | The iHeater's state name, as the dashboard shows it (`idle`, `heating`, `hold`, `fault`, ...). |
| `fault` | Active fault code, 0 for none. |
| `fault_name` | The fault's name, as `faultName()` gives it. |
| `next_within` | Seconds until the Remote's next report at the latest. |

`next_within` lets Klipper judge staleness without sharing constants with
the firmware.

`air`, `element` and `power` are `null` when the reading is the
`kNoReading` sentinel, when there is no snapshot yet, and whenever `link`
is not `up`: a snapshot from a link that is not up is stale, and sending it
would hide that.

The method returns `{"ok": true}` when a heater with that token took the
report, or an error when none matches or Klippy is not ready.

### When the Remote sends

A pure policy in `lib/remote-policy/` decides, each main-loop pass, whether
to send now. It sends:

- when a reading changes meaningfully, at most once per second.
  "Meaningfully" is a change of at least 0.1 °C in air or element, any
  change in power (to 1 %), fan, state, fault or link;
- when the target the Remote follows changes, since that can change the
  heartbeat interval;
- otherwise, a heartbeat every 5 s while the commanded target is above 0
  or the iHeater reports heating or hold, and every 15 s otherwise.

`next_within` is the heartbeat interval that applies after this report,
worked out from the state the report carries and the current target.

The Remote sends only while it is subscribed to its paired heater (Klipper
ready and paired). It sends only in UART mode. In pulse mode there are no
readings, and the Klipper side keeps using `sensor:` (see Fallback).

Each send is a JSON-RPC call through `MoonrakerLink::call()`. Only one
report is in flight at a time. If the previous one has not been answered
within its timeout, the next send waits.

### When the plugin is missing or refuses

If Moonraker answers "method not found", the Remote stops sending until the
next reconnect. The dashboard's source line gains a note: "Klipper plugin
not installed: readings are not sent to Klipper". Other errors (no matching
heater, Klippy not ready) are counted and shown in the link diagnostics,
and sending continues on the normal schedule.

### Moonraker component `iheater_remote`

- Registers `server.iheater_remote.report` (POST
  `/server/iheater_remote/report`).
- Validates the field types and the token format, then forwards the report
  unchanged to the Klippy endpoint `heater_virtual/report` and returns its
  answer.
- Holds no state and adds no config options. No rate limit: callers are
  already authenticated by Moonraker, and the Remote sends at most once a
  second.
- When Klippy is not connected, returns an error at once without queuing.

### Klipper: `heater_virtual`

New behaviour; the existing options keep their meaning except `sensor:`.

- Registers the webhooks endpoint `heater_virtual/report` once, for all
  `heater_virtual` objects. A report goes to the heater whose
  `remote_token` matches; with no match it returns an error.
- Keeps the latest report and its arrival time (reactor monotonic time).
  The report is fresh until `arrival + 2 × next_within + 2 s`.
- `linked` is true while the report is fresh and its `link` is `up`.
- Temperature: the report's `air` while fresh and not null. Because `air`
  is null whenever `link` is not up, a fresh report from a Remote that has
  lost the iHeater falls back too. Otherwise see Fallback.
- `power`: the report's `power` while linked. Otherwise the old behaviour
  (1 when a target is set, else 0), so Mainsail still shows something
  sensible when unlinked.
- `get_status` adds `linked`, `iheater_state`, `fault`, `fault_name` and
  `fan` (the last known values; `linked` tells whether they are current).

### Fallback when not linked

- `sensor:` becomes optional. If it is set, the temperature comes from that
  sensor whenever there is no fresh air reading, exactly as today.
- If it is not set, the temperature stays at the last air reading received
  (0 before the first one).
- Pulse-mode users keep `sensor:` and see no change.

### Sensor type `iheater_remote`

Registered by `heater_virtual.py` with Klipper's sensor factory, for use in
an ordinary temperature sensor section:

```
[temperature_sensor chamber_element]
sensor_type: iheater_remote
heater: chamber          # the heater_virtual's short name
reading: element         # element (default) or air
```

It reports the chosen reading from the heater's latest report. With no
fresh reading it reports the last value received. It does not shut Klipper
down on its own. It finds its heater at `klippy:connect`. If the sensor
factory must be registered before the `[temperature_sensor]` section is
read, the README says to put `[heater_virtual]` first.

### Checking the link in START_PRINT

The README shows a snippet for the user's own `START_PRINT`, for example:

```
{% if not printer["heater_virtual chamber"].linked %}
  {action_raise_error("Chamber heater not linked: check the iHeater Remote")}
{% endif %}
```

`klipper/iheater_remote.cfg` keeps its current `M141` and `M191`.

### Dashboard

The Remote's status JSON gains, under `iheater.diag`, the count of reports
sent, refused and failed, and whether the plugin was found. The dashboard
shows the plugin-missing note described above. No other UI change.

## Testing

- Policy: native tests for the send schedule (change threshold, 1 s floor,
  5 s and 15 s heartbeats, idle versus heating) and for the
  method-not-found back-off.
- Klipper module: unit tests for routing by token, freshness from
  `next_within`, `linked`, temperature and power fallbacks, `get_status`
  fields, and the sensor type.
- Moonraker component: unit tests for validation, forwarding, and the
  Klippy-not-connected error.
- Bench: with the fake Moonraker (`tools/fake_moonraker/`) extended to
  accept the new method, with a switch to answer "method not found" so
  the back-off can be seen, then on hestia with real Moonraker and Klipper.

## Left to the builder

- The exact Moonraker and Klipper API calls for registering and forwarding
  the endpoint, checked against current Moonraker and Klipper sources.
- Report JSON size and buffer choices in the firmware.
- How the fake Moonraker models the new method.
- Wording of errors and of the README install steps.
