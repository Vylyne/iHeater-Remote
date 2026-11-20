# Local-only cleanup and own Moonraker client — design

Status: draft for approval · 2026-11-19

## Intent

The Remote is now one link in a chain the user owns end to end: their own
iHeater firmware, this Remote, and their own Klipper (and soon Moonraker)
plugins. Compatibility with the upstream iDryer / iHeater Link ecosystem is
no longer a goal. Everything that exists only for it goes, and the one
piece of idryer-core the Remote still needs, the Moonraker client, is
replaced with our own, which fixes the two quirks we currently work around.

Why: a smaller codebase to add the UART client to next, flash headroom on
the C3 (84.5 % used today), and a Moonraker link that recovers from a
Klipper restart in about a second instead of 30.

## Goals

1. **Remove upstream code and targets.** The `lib/idryer-core` junction,
   `src/main.cpp` and its `heater/` and `menu/` trees, every env except
   `esp32c3-super-mini-remote`, Bambu, Home Assistant and portal code,
   `include/wifi_tx_power.h`, and their docs, images and tools.
2. **Own Moonraker client** (approach A below), Moonraker only.
3. **Control sources: Manual or Klipper.** No Bambu, no bed rule. The
   auto target and bed threshold settings and their Setup fields go.
4. **Klipper module talks to the Remote directly through
   `heater_virtual`.** The `mirror_macro` option and the
   `[gcode_macro VIRTUAL_CHAMBER]` block are removed.
5. **Existing settings carry over.** Moonraker host, port, API key and the
   selected source are migrated once from idryer-core's NVS namespaces;
   nothing has to be re-entered after the OTA.
6. **Behaviour kept:** web UI and dashboard, OTA, Wi-Fi supervisor and TX
   power, the 10 min printer link-loss rule, the target range, BOOT button
   and LED, pulse output.
7. **`AGENTS.md`** with the gate commands and repo conventions (the repo
   predates that standard).

## Non-goals

- The UART link to the iHeater and a separate UART build (next project).
- The Moonraker plugin that carries iHeater data into `heater_virtual`
  (decided below, built with the UART project).
- The ESP32-S3-ETH port.
- Home Assistant. If wanted, Moonraker's own `[mqtt]` section can publish
  Klipper objects once iHeater data is in Klipper.

## Anti-goals

- Heat from Klipper continuing after Klipper sets the target to 0.
- A Klipper restart leaving the Remote deaf until a timeout.
- The user having to re-enter Moonraker settings after updating.
- Losing any behaviour listed in goal 6.

## Approaches

- **A. Own WebSocket client (chosen by the user).** Built on the
  `links2004/WebSockets` library the dashboard socket already uses.
  Resubscribes when Moonraker reports Klipper ready, reports clear link
  health, and can send JSON-RPC calls as well as receive.
- B. Copy idryer-core's Moonraker client into the repo. Least work, but
  keeps both quirks (no resubscribe, zeroed status on disconnect), both
  workarounds, and the manager and store layers around it. Rejected.
- C. Poll Moonraker's HTTP API. Simplest code, but trades push updates for
  polling delay. Rejected.

## Design

### Moonraker client

- WebSocket to `ws://<host>:<port>/websocket`. No TLS (local network only).
- On connect: `server.connection.identify` (client name `iheater-remote`,
  type `other`, `api_key` when one is set), then `printer.objects.list`.
- **Heater discovery:** the first object whose name starts with
  `heater_virtual ` is the heater. More than one: use the first in sorted
  order and log the others. None: link is up but there is nothing to
  follow; the Remote never heats from Klipper.
- **Subscribe** to `{"<heater>": ["target"], "webhooks": ["state"]}`.
  The initial response and every `notify_status_update` carry the target;
  it is applied through the existing clamp to the target range. `0` means
  off and applies immediately.
- **Klipper restarts:** `notify_klippy_disconnected` and
  `notify_klippy_shutdown` mark the link unhealthy and hold the target.
  `notify_klippy_ready` re-runs list and subscribe, so the new target
  (normally 0 after a restart) arrives within about a second. If list or
  subscribe fails because Klipper is not ready yet, the client waits for
  `notify_klippy_ready` (the 30 s query below retries as a backstop).
- A target update is applied whatever the Klipper state; only the
  zero-out on a lost link is replaced by the hold.
- **Liveness:** WebSocket ping every 10 s, reconnect after two missed
  pongs. Every 30 s, `printer.objects.query` for the same objects; no
  reply within 5 s → reconnect. The reply also re-syncs the target and the
  Klipper state in case a notification was missed. This replaces the 30 s
  silence watchdog: with a target-only subscription, silence is normal.
- **Health** for the link-loss rule: healthy only when the socket is up,
  subscribed, and `webhooks.state` is `ready`. Everything else (Wi-Fi down,
  Moonraker down, Klipper not ready, query timeout) is unhealthy and
  counts toward the 10 min cut, exactly as today.
- **Outbound calls:** a `call(method, params)` that sends JSON-RPC and
  matches replies by id, used now for identify, list, subscribe and query,
  and later for `iheater.report`.
- Reconnect backoff: 2 s, doubling to a 30 s cap, reset on success.
- Pure decisions (heater discovery from an object list, health from
  socket/subscribed/Klipper state, backoff steps) go in
  `lib/remote-policy` with Unity tests, like the existing policy.

### Settings and Setup page

- Stored in the Remote's own `iheater-remote` namespace: `source`
  (`manual` / `klipper`), `moon-host`, `moon-port` (default 7125),
  `moon-key`.
- **Migration** on first boot when `source` is absent: read `li_common`
  `active` (3 = Moonraker) and `li_moon` `host` / `port` / `apiKey`, write
  them to the new keys, then erase the `li_common`, `li_moon`, `li_bambu`
  and `li_ha` namespaces. Old auto-target and bed-threshold keys are
  removed from `iheater-remote`.
- Setup page: source is Manual or Klipper (Moonraker); host, port and API
  key (write-only, blank keeps it). Auto target and bed threshold fields
  are removed. Status JSON drops `bambuState`, `bambuFilament`,
  `bambuPrintState` and `integration`, and gains `source`,
  `moonrakerState` (`disconnected` / `connecting` / `klippy-not-ready` /
  `ready`) and `heaterObject`.
- `/api/integration` keeps its URL so the dashboard keeps working, with
  only the Moonraker fields.

### Klipper module

- Remove `mirror_macro` and its macro writes from `heater_virtual.py`.
  Klipper rejects unknown options, so an old config that still sets it
  gets a clear error; the update note in `klipper/README.md` says to
  delete `mirror_macro` and the `VIRTUAL_CHAMBER` macro.
- Remove the macro from `klipper/iheater_remote.cfg`; tests updated.

### Feeding iHeater data back into Klipper (decided, built later)

G-code is not used for this: scripts from Moonraker queue behind running
commands, and `M191` would deadlock waiting on a temperature that can only
arrive through the queue it is blocking. Instead, with the UART project:

- A small **Moonraker plugin** (`moonraker/iheater.py`, installed next to
  the Klipper module) registers a JSON-RPC method, e.g. `iheater.report`.
- The Remote calls it over its existing Moonraker socket whenever iHeater
  values change (air and element temperature, PWM, state, fault).
- The plugin forwards to an endpoint `heater_virtual` registers with
  Klipper's webhooks, which runs immediately, outside the G-code queue.
- Moonraker has no public pass-through to Klipper endpoints, so the plugin
  uses Moonraker's internal request call. It checks for it at startup and
  logs a clear error if a Moonraker update removes it.

### Removals

- `lib/idryer-core` (junction), `lib/idryer-bambu`, `src/main.cpp`,
  `src/heater/`, `src/menu/`, `include/wifi_tx_power.h`,
  `BAMBU_MQTT_FIELDS.md`, `docs/<lang>/` (upstream product docs),
  `img/`, `imgweb/`, `tools/fake_bambu/`, `tools/mock_portal.py`.
- `platformio.ini`: only `[env:esp32c3-super-mini-remote]` and what it
  uses; drop the cloud placeholder flags and `PubSubClient` /
  Improv-WiFi deps if nothing else needs them.
- `lib/remote-policy`: drop `MoonrakerInputs`, `resolveMoonrakerTargetC`,
  `moonrakerStatusIsReset` and `kMoonrakerStaleMs` with their tests.
- README: remove the upstream sync section and Bambu / bed rule text;
  keep Credits.
- `tools/fake_moonraker/` stays, updated to serve `heater_virtual` and the
  Klipper ready/disconnect notifications for bench testing.

## Calls I made (check these)

1. No TLS option for Moonraker (local network only).
2. Settings migrate once, then idryer-core's old NVS namespaces are erased.
3. Subscription is just the heater target and Klipper state; the silence
   watchdog becomes a WebSocket ping plus a 30 s query.
4. Klipper not ready counts as link down (10 min hold), same as today. A
   Klipper shutdown still turns heat off at once, because
   `heater_virtual` sets its target to 0 on shutdown and the Remote applies
   target updates whatever the Klipper state.
5. Heater found automatically; with several, the first in sorted order.
6. The upstream multilingual docs under `docs/<lang>/` are removed because
   they document a product this repo no longer builds; the README stays in
   English (Canadian).
7. The env keeps its name `esp32c3-super-mini-remote` so the release
   workflow and OTA file names do not change.

## Left to the builder

- Module layout for the client (`lib/moonraker-link` or `src/remote/`).
- Exact JSON-RPC id handling and buffer sizes.
- Whether `extra_scripts/` and `package.json` are still used by the
  remaining env; remove what is not.
- `AGENTS.md` content beyond the gate commands:
  `pio run -e esp32c3-super-mini-remote`, the policy test build, and
  `python -m unittest discover -s klipper -p "test_*.py"`.
