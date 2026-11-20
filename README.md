# iHeater Remote

iHeater Remote is a local-only ESP32-C3 companion firmware for the
iHeater controller. It drives the iHeater pulse input over one GPIO,
exposes a small local web UI, and recreates the standalone iHeater mode
button behavior on an ESP32-C3 Super Mini.

It is local-only: there is no cloud, portal or Internet dependency. It
started as a downstream fork of iHeater Link and no longer carries any
upstream code.

## Credits

This project is based on:

- [pavluchenkor/iHeater-Link](https://github.com/pavluchenkor/iHeater-Link)
- [plastikman/DragonBreath v0.8.0-rc3](https://github.com/plastikman/DragonBreath/releases/tag/v0.8.0-rc3)
  for the local control-source and setup UX model.

The iHeater controller firmware and safety behavior remain the
controller's responsibility. iHeater Remote only sends the setpoint pulse
train.

## What the Local Remote Does

- Runs without any cloud, portal MQTT or Home Assistant.
- Has one control source at a time, chosen on the Setup page:
  **Manual**, or **Klipper (Moonraker)**. The Remote runs its own Moonraker
  WebSocket client and follows one `heater_virtual` heater that you pair by
  token. Settings and the API key live in ESP32 NVS.
- With Klipper, appears as a native chamber heater (`M141`/`M191`, Mainsail
  target box) via the bundled [Klipper module](klipper/README.md).
- Serves a local web UI for selecting the heater mode.
- Creates a captive-portal access point if it cannot join configured
  Wi-Fi.
- Supports local OTA updates from the web UI.
- Reuses the ESP32-C3 Super Mini BOOT button for standalone-style mode
  selection.
- Uses the ESP32-C3 Super Mini blue LED to indicate the selected mode by
  flashing the mode number in quick bursts.

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

### UART link

Instead of the pulse, the Remote can talk to an iHeater that runs the UART
build over a Modbus RTU link. Choose it with the **Output** setting on the
Setup page (switching restarts the Remote; the iHeater must run the matching
build). The dashboard then shows the iHeater's live state.

| ESP32-C3 Super Mini | iHeater USB-C pin | Purpose |
| --- | --- | --- |
| `GPIO4` (TX) | `D+` | Remote to iHeater |
| `GPIO5` (RX) | `D−` | iHeater to Remote |
| `5V` | `VBUS` | iHeater power |
| `GND` | `GND` | common ground |

- Leave `3V3` unconnected at the iHeater.
- Never connect the iHeater's JST 5 V connector at the same time as the
  Remote's 5 V: the board has no 5 V regulator.
- Do not connect or disconnect the cable with power applied.

See the "Working with iHeater Remote (UART build)" section of the
[iHeater user guide](https://github.com/Vylyne/iHeater-Standalone-Firmware/blob/main/docs/en/README.md)
for the wiring, the faults and what the link can clear.

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
- **Setup** has a **Control source** choice: Manual or Klipper (Moonraker).
  For Klipper, enter the host and port, and tick **TLS** to use a secure
  connection (the port switches between 7125 and 443; TLS on port 7125 is
  rejected). The API key is write-only: a blank field keeps the stored key.
  Wi-Fi and source settings share the page without overwriting each other.
- **Pairing:** the Pairing card shows this Remote's token (`XXXX-XXXX`).
  Add it to your `[heater_virtual]` section as `remote_token`
  ([klipper/README.md](klipper/README.md)); the Remote follows only the
  heater with that token. **Generate a new token on save** replaces it. To
  keep an old token after a wipe, enter it manually in the Pairing card.
  Until a heater carries the token, the dashboard shows "not paired" with
  the token, the Remote never heats from Klipper, and manual heat still
  works.
- **Status:** the status JSON on the WebSocket includes `source`,
  `moonrakerState` (`disconnected`, `connecting`, `klippy-not-ready` or
  `ready`), `heaterObject` and `moonrakerError` (what failed, such as a TLS
  error or "not paired").
- **TLS:** the certificate is verified against embedded Let's Encrypt roots
  (ISRG Root X1, X2, YE and YR) with a host-name check. There is no
  skip-verify option. A server using another CA needs that root added in
  `src/standalone/MoonrakerCa.h`.
- **Source switching:** a manual press (web UI or BOOT button) switches the
  source to Manual. Boot, the timer and OTA do not.
- **iHeater target range** (Setup page, default Min 45 / Max 85): records
  the range your iHeater firmware accepts. It does not change any setting
  on the iHeater. Every target the Remote sends (Klipper or manual modes)
  is clamped to it. Stock v0.5.0 iHeater firmware accepts 45–90, ignores
  other targets, and faults when the air reaches its `MAX_AIR_TEMP` (90),
  so we recommend a Max no higher than 80.
- **Liveness:** the Remote sends a WebSocket ping every 10 s and reconnects
  after 2 missed pongs. It also queries the heater every 30 s. Reconnects
  back off from 2 s, doubling to 30 s. After a Klipper restart, the Remote
  rediscovers the heater and resubscribes.
- **Link loss:** while Klipper is the source and the Remote is heating from
  it, a lost link holds the last target, so a short Wi-Fi drop does not
  cool the chamber mid-print. If there is no healthy link for 10 minutes,
  printer-commanded heat turns off. Any real target from the printer,
  including `0`, applies immediately. Manual heat is never affected.
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

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/wildtang3nt)

## Updating from 2.0

Settings from 2.0 (idryer-core) carry over once: the Remote reads them at
the first boot, saves them in its own namespace and erases the old ones. A
Moonraker setup from 2.0 becomes source Klipper with the same host, port,
TLS flag and key.

On the Klipper side, add `remote_token` (shown on the Setup page) to your
`[heater_virtual]` section and remove the old `VIRTUAL_CHAMBER` macro. Until
you do, the Remote shows "not paired". See [klipper/README.md](klipper/README.md).

## Notes

- Do not use `erase_flash` if you want to preserve NVS data from another
  firmware build.
- ESP32 Wi-Fi is 2.4 GHz only.
- The iHeater controller should remain responsible for heater safety and
  fault handling.
