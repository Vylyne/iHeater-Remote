# Wi-Fi resilience, TX power and link-loss safety — design

Status: approved in conversation · 2026-11-19

## Intent

The Remote loses its network and does not always come back, and heat must
never continue on a stale target when the printer cannot be read.

## Goals

1. **Wi-Fi supervisor.** In station mode, if Wi-Fi is down, call
   `WiFi.reconnect()` every 30 s until it returns. In AP fallback with saved
   credentials, keep the AP up (setup stays reachable) and retry the saved
   network every 2 min in AP+STA mode; once joined, drop the AP and captive
   DNS.
2. **TX power selection.** Setup dropdown: 8.5 (default, as today), 11, 13,
   15, 17, 19.5 dBm. Stored in NVS, applied at boot; a change restarts the
   device. Status JSON also reports `wifiRssi` so the effect can be judged.
3. **Link-loss safety with a 10 min grace.** While Moonraker or Bambu is the
   active source and the Remote is heating from it, keep the last target
   through a link outage, and turn heat off once the link has been down for
   10 min. "Down" means the client is not Connected, or (Moonraker) it is
   Connected but silent for 30 s. idryer-core's immediate zero on
   disconnect is ignored (recognised as a Moonraker status with an empty
   `printerState`, or a Bambu status while the client is not Connected), so
   short Wi-Fi drops do not cool the chamber mid-print. The 30 s Moonraker
   silence watchdog still restarts the client, but no longer turns heat
   off. A real target from the printer (including 0) is always applied
   immediately.

## Non-goals

- Ethernet, multiple IPs (separate project: ESP32-S3-ETH port).
- Repo cleanup (separate project).
- idryer-core changes.

## Anti-goals

- Heat continuing for more than 10 min while the printer link is down.
- A short Wi-Fi drop turning heat off mid-print.
- The Remote stuck in AP mode after a power cut because the router booted
  slower than the ESP32.

## Approaches

- **Connection-state safety with grace (chosen).** Off once the active
  printer link has been down for 10 min. Independent of update cadence.
- Silence-only rule (no update for N in any state). Rejected: Bambu
  reports only on change, so a steady print could trip it and heat would
  stay off until a value changes.
- Immediate off on disconnect (today's idryer-core behaviour). Rejected by
  the user: a Wi-Fi blip would cool the chamber mid-print.
- For AP recovery, periodic reboot. Rejected: drops setup sessions and the
  pulse output resets; AP+STA retry keeps both.

## Calls I made (check these)

1. Safety uses connection state, not a silence timer, for disconnected
   links; grace 10 min (user's call), fixed constant, not a Setup option.
   Worst case: Klipper crashes while the network is also down → the
   iHeater holds its last target up to ~10.5 min (its own MAX_AIR_TEMP /
   MAX_TEMP protections still apply).
2. Each AP-mode retry briefly disrupts devices connected to the AP (channel
   scan).
3. `wifiRssi` added to the status JSON and shown on the Setup page.
