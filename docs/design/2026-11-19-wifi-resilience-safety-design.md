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
3. **Link-loss safety.** While Moonraker or Bambu is the active source and
   the Remote is heating from it, turn heat off as soon as that client is not
   Connected. The existing 30 s Moonraker silence watchdog (Connected but no
   updates → off + client restart) stays.

## Non-goals

- Ethernet, multiple IPs (separate project: ESP32-S3-ETH port).
- Repo cleanup (separate project).
- idryer-core changes.

## Anti-goals

- Heat continuing while the printer link is down.
- The Remote stuck in AP mode after a power cut because the router booted
  slower than the ESP32.

## Approaches

- **Connection-state safety (chosen).** Off whenever the active printer
  client is not Connected. Independent of update cadence.
- Silence-only rule (no update for 30 s in any state). Rejected: Bambu
  reports only on change, so a steady print could trip it and heat would
  stay off until a value changes.
- For AP recovery, periodic reboot. Rejected: drops setup sessions and the
  pulse output resets; AP+STA retry keeps both.

## Calls I made (check these)

1. Safety uses connection state, not a silence timer, for disconnected
   links (deviation from "30 s without data" as discussed; reason above).
   Heat resumes automatically when the client reconnects and reports.
2. Each AP-mode retry briefly disrupts devices connected to the AP (channel
   scan).
3. `wifiRssi` added to the status JSON and shown on the Setup page.
