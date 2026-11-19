# iHeater Remote

iHeater Remote is a local-only ESP32-C3 companion firmware for the
iHeater controller. It drives the iHeater pulse input over one GPIO,
exposes a small local web UI, and recreates the standalone iHeater mode
button behavior on an ESP32-C3 Super Mini.

This is a downstream fork of iHeater Link. It intentionally removes the
cloud/portal workflow from the active firmware build while keeping the
upstream source layout close enough to make future syncs practical.

## Credits

This project is based on:

- [pavluchenkor/iHeater-Link](https://github.com/pavluchenkor/iHeater-Link)
- [plastikman/DragonBreath v0.8.0-rc3](https://github.com/plastikman/DragonBreath/releases/tag/v0.8.0-rc3)
  for the local control-source and setup UX model.

The iHeater controller firmware and safety behavior remain the
controller's responsibility. iHeater Remote only sends the setpoint pulse
train.

## What the Local Remote Does

- Runs without iDryer cloud, portal MQTT, or Home Assistant.
- Can bind directly to one local printer source: Bambu Lab LAN MQTT or
  Moonraker/Klipper. The selected source and its credentials live in ESP32
  NVS; no portal account, cloud broker, or Internet connection is involved.
- With Klipper, can appear as a native chamber heater (`M141`/`M191`,
  Mainsail target box) via the bundled
  [Klipper module](klipper/README.md).
- Serves a local web UI for selecting the heater mode.
- Creates a captive-portal access point if it cannot join configured
  Wi-Fi.
- Supports local OTA updates from the web UI.
- Reuses the ESP32-C3 Super Mini BOOT button for standalone-style mode
  selection.
- Uses the ESP32-C3 Super Mini blue LED to indicate the selected mode by
  flashing the mode number in quick bursts.
- Keeps the upstream iHeater Link code present behind its original build
  environments so upstream changes can still be pulled and reviewed.

## Hardware

Tested target:

| Board | Status |
| --- | --- |
| ESP32-C3 Super Mini | Supported |

The firmware assumes:

| ESP32-C3 Super Mini | iHeater | Purpose |
| --- | --- | --- |
| `5V` | `5V` | controller power |
| `GND` | `GND` | common ground |
| `GPIO3` | signal input | pulse setpoint |

The default BOOT button input is `GPIO9`, and the blue status LED is
`GPIO8` active-low.

## Build

The downstream build target is:

```bash
pio run -e esp32c3-super-mini-remote
```

To flash a connected ESP32-C3 Super Mini:

```bash
pio run -e esp32c3-super-mini-remote -t upload --upload-port /dev/tty.usbmodem101
```

Replace the upload port with the port for your board.

## Releases

Pushing a `v*` tag builds the public AP-fallback firmware and publishes a
GitHub Release:

```bash
git tag v1.1.0
git push origin v1.1.0
```

Release assets include the app firmware image, a merged ESP32-C3 factory
image, and a flash bundle with offsets for each binary part.

Use the `*-firmware.bin` release asset for web OTA updates. The
`*-factory.bin` image is for full serial flashing at offset `0x0`.

## Wi-Fi

Release firmware does not need Wi-Fi credentials at build time. If the
device cannot join a configured network, it starts an access point:

```text
iHeater Remote XXXX
```

Open the web UI, enter the Wi-Fi SSID and password, and save. The device
stores those credentials in ESP32 NVS, reboots, and then joins the LAN.

The web UI is served at:

```text
http://192.168.4.1/
```

Captive portal DNS is enabled in AP mode, so most phones and laptops
should offer to open the UI automatically.

Staying connected:

- If Wi-Fi drops, the Remote calls a reconnect every 30 s until it
  returns.
- If it fell back to the access point (for example, after a power cut
  where the router booted slower than the Remote) and has saved
  credentials, it keeps the access point up and retries the saved network
  every 2 minutes. Once it joins, it shuts down the access point and
  captive DNS. Each retry briefly disturbs devices connected to the access
  point.
- **Transmit power** (Setup page): 8.5 dBm (default), 11, 13, 15, 17 or
  19.5 dBm. Higher power can help a weak signal, but some ESP32-C3 Super
  Mini boards fail to connect above 8.5 dBm. Changing it restarts the
  device. The Setup page shows the current signal strength (RSSI) to judge
  the effect, and the status socket reports it as `wifiRssi`.

For local development, `include/secrets.h` is still supported as a
fallback when no credentials have been saved in NVS:

```cpp
#pragma once

#define WIFI_SSID "Your 2.4 GHz SSID"
#define WIFI_PASSWORD "Your password"
```

`include/secrets.h` is intentionally ignored by git.

## Controls

The mode table follows the standalone iHeater firmware behavior:

| Mode | Target |
| --- | --- |
| `MODE_TEMP_0` | Off |
| `MODE_TEMP_1` | 55 C |
| `MODE_TEMP_2` | 60 C |
| `MODE_TEMP_3` | 65 C |
| `MODE_TEMP_4` | 70 C |
| `MODE_TEMP_5` | 75 C |
| `MODE_TEMP_6` | 80 C |
| `MODE_TEMP_7` | 85 C |

BOOT button:

- 1 to 7 quick presses selects `MODE_TEMP_1` through `MODE_TEMP_7`.
- Two quick presses selects `MODE_TEMP_2` / 60 C.
- Holding BOOT for about 2 seconds selects `MODE_TEMP_0` / Off.

Web UI:

- Open the device IP, or `http://192.168.4.1/` in AP mode.
- The touch-first dashboard has Home, Heat, Timer, and Settings views, with
  compact cards and a navigation rail inspired by DragonBreath's local SPA.
- Live dashboard state is pushed over a local WebSocket at
  `ws://<device-ip>:81/`; the dashboard does not poll a status HTTP endpoint.
- **Setup** follows DragonBreath RC3’s local-source model: choose Manual,
  Bambu LAN, or Moonraker; only one source is active. Non-secret fields are
  prefilled, while a blank access code/API key preserves the stored secret.
  Wi-Fi changes and source changes share the same page without requiring an
  Internet connection or overwriting Wi-Fi accidentally.
- **Moonraker:** with the Klipper chamber heater installed
  ([klipper/README.md](klipper/README.md)), the Klipper chamber target
  (`M141`/`M191`, Mainsail) is followed directly, and `0` means off even if
  the bed is hot. Without it, the bed rule below applies.
- **Bed rule** (Bambu, and Moonraker without the chamber heater): the iHeater
  applies the configured auto target only while the printer bed **setpoint**
  meets the configured threshold (40–120 °C). Bambu is read-only: no printer
  control commands are sent.
- **iHeater target range** (Setup page, default Min 45 / Max 85): records
  the range your iHeater firmware accepts. It does not change any setting
  on the iHeater. Every target the Remote sends (Klipper, bed rule, manual
  modes) is clamped to it. Stock v0.5.0 iHeater firmware accepts 45–90,
  ignores other targets, and faults when the air reaches its
  `MAX_AIR_TEMP` (90), so we recommend a Max no higher than 80.
- **Moonraker watchdog:** if no Moonraker update arrives for 30 s while
  connected (e.g. after a Klipper restart, which drops subscriptions), the
  Remote reconnects to resubscribe. Heat is not turned off at this point;
  the link-loss rule below decides.
- **Link loss:** while Moonraker or Bambu is the source and the Remote is
  heating from it, a lost printer link (disconnected, or Moonraker silent
  for 30 s) holds the last target, so a short Wi-Fi drop does not cool the
  chamber mid-print. If the link is still down after 10 minutes, heat
  turns off. Any real target from the printer, including `0`, applies
  immediately. Manual heat is never affected.
- Tap a mode button or Stop, then choose a timer preset or a custom duration.
  `0` means Until Off.
- Selecting a mode starts the current timer from that moment. Changing
  the timer while heating restarts the countdown from that moment.
- Upload a `*-firmware.bin` release asset to update over the air. The
  device turns heating off before writing the update, then reboots.
- The UI shows only directly knowable state: commanded target, timer,
  pulse code, and network status. It deliberately does not claim to display
  a current temperature or safety-chain state because this controller has
  neither local temperature sensing nor an independently attestable safety
  chain.

LED:

- Off mode leaves the blue LED off.
- Active modes flash the mode number, pause, then repeat.

## Keeping Sync With Upstream

This fork keeps the upstream project shape mostly intact:

- Original integration firmware remains in `src/main.cpp` behind the
  normal upstream build environments.
- iHeater Remote is selected by the `IHEATER_REMOTE_LOCAL` build flag.
- The local-only implementation lives under `src/standalone/`.
- The downstream PlatformIO environment is isolated as
  `esp32c3-super-mini-remote`.

Suggested sync workflow:

```bash
git remote add upstream https://github.com/pavluchenkor/iHeater-Link.git
git fetch upstream
git merge upstream/main
```

Expect most downstream conflicts to be limited to `README.md`,
`platformio.ini`, `src/main.cpp`, and `src/standalone/`.

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/wildtang3nt)

## Notes

- Do not use `erase_flash` if you want to preserve NVS data from another
  firmware build.
- ESP32 Wi-Fi is 2.4 GHz only.
- The iHeater controller should remain responsible for heater safety and
  fault handling.
