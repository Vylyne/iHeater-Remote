# fake_moonraker.py

A fake Moonraker WebSocket server for bench-testing iHeater Remote without
a real Klipper printer. It speaks the same JSON-RPC the Remote uses and
lets you force the awkward cases: a Klipper restart, a frozen link, an
unpaired Remote, noisy traffic and a large object list.

## What it does

- Listens on `ws://0.0.0.0:7125/websocket` (change with `--port`).
- Answers `server.connection.identify`, `printer.objects.list`,
  `printer.objects.query` and `printer.objects.subscribe`. List, query and
  subscribe return a JSON-RPC error only during the start-up window after
  `restart`. After `shutdown` they still answer, with `webhooks.state`
  `shutdown`, as real Moonraker does.
- Simulates `heater_virtual chamber`, which carries the `--token` value as
  its `remote_token`. With `--heaters N`, extra heaters get other tokens.
- Sends `notify_proc_stat_update` every second as noise, like a real
  Moonraker.
- Pads `printer.objects.list` with `--objects` extra objects (default 150).

## Run

```bash
pip install websockets
python tools/fake_moonraker/fake_moonraker.py --token ABCD-2345
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--port` | `7125` | Listening port |
| `--token XXXX-XXXX` | none | `remote_token` of the paired heater. No token means the unpaired case. |
| `--heaters N` | `1` | Number of `heater_virtual` objects; extras get other tokens |
| `--objects N` | `150` | Padding objects in the object list |

Type these commands on stdin, then Enter:

| Command | Effect |
| --- | --- |
| `t <C>` | Set the paired heater's target and notify |
| `restart` | Send `notify_klippy_disconnected`, set the target to 0, then send `notify_klippy_ready` after 2 s |
| `shutdown` | Set the target to 0, send `notify_status_update`, then `notify_klippy_shutdown`. Sticky: Klipper stays shut down until `restart` |
| `freeze` | Stop answering everything, pings included, until `thaw` |
| `thaw` | Resume after `freeze` |
| `drop` | Close all sockets |

## Set up the Remote

Open `http://<iHeater-IP>/setup`, set **Control source** to **Klipper
(Moonraker)** and enter:

| Field | Value |
| --- | --- |
| Host | the IP of the computer running the fake |
| Port | `7125` |
| TLS | off |

The Pairing card shows the Remote's token (`XXXX-XXXX`). Start the fake
with that token. The Remote and the computer must be on the same Wi-Fi
network.

## Bench checklist

Each item matches a Review Focus scenario in
`docs/design/2026-11-19-local-only-cleanup-plan.md`. Watch the Serial log
(`[MOON]` lines) and the dashboard.

1. **Restart while heating.** Start with the Remote's token, enter
   `t 60`, wait for the Remote to heat, then enter `restart`.
   - The Remote holds, rediscovers and resubscribes.
   - The new target (0) applies within seconds, not after 10 min.
2. **Frozen link (half-open socket).** Enter `t 60`, then `freeze`.
   - Missed pongs force a reconnect (about 20 s: pings every 10 s, two
     missed).
   - Reconnect backoff grows from 2 s to the 30 s cap.
   - If you leave it frozen, heat cuts 10 min after the link was last
     healthy.
   - `thaw` lets the Remote reconnect.
3. **Unpaired.** Start without `--token`, or with a different one.
   - The Remote never heats from Klipper. If it was heating from the
     printer (for example after you changed the token and restarted the
     fake), it commands the target off as soon as discovery finds no match.
   - The dashboard shows the "not paired" text with the token.
   - Manual heat still works.
4. **Noise and a large list.** Run with the defaults.
   - `notify_proc_stat_update` arrives at about 1 Hz and is skipped.
   - The 150-object list is handled without a crash or heap growth.
5. **Shutdown.** Enter `t 60`, then `shutdown`.
   - The target 0 applies at once, so heat stops.
   - The Remote drops to "not subscribed" and rediscovers on its 30 s tick.
     The fake still answers list, query and subscribe, with `webhooks.state`
     `shutdown`, as real Klipper does. The Remote resubscribes and reads
     the target (0), but treats Klipper as not ready: the link is not
     healthy, the dashboard shows `klippy-not-ready`, and the 10 min
     link-loss rule applies to any heat the printer had commanded.
   - Enter `restart` to recover; the Remote rediscovers on
     `notify_klippy_ready` and the link turns healthy.
6. **Several heaters.** Run with `--heaters 3`. The Remote pairs only with
   the heater whose `remote_token` matches.

Also check the real server (`wss://hestia.brunton.ca`, TLS on port 443)
with the real `remote_token`.

## Troubleshooting

- The Remote cannot connect: check the computer's firewall allows
  incoming connections for Python on the port.
- It connects but never heats: the Setup page token must match
  `--token`. The `moonrakerError` field says why (for example "not paired").
- `freeze` relies on pausing the socket transport. If your `websockets`
  version has no `transport` attribute on the connection, the log shows a
  warning and pongs keep flowing.
