# Arduino-ESP32 3.x upgrade — design

Status: draft · 2026-11-21

## Intent

The Remote is moving to a Waveshare ESP32-S3-ETH, so it can use wired
Ethernet. On the C3 Super Mini, Wi-Fi measured 26 % TX retries at −69 dBm.
Arduino-ESP32 3.x has the W5500 in its `ETH` library. The user chose a
current framework over adding ESP-IDF's W5500 driver to 2.0.17. This first
step only upgrades the framework, on the C3, so that any breakage can be
blamed on the framework change alone. The S3-ETH board is the next design.

## Goals

- The C3 build (`esp32c3-super-mini-remote`) runs on Arduino-ESP32 3.x and
  behaves as it does on 2.0.17:
  - the pulse train;
  - the UART link;
  - Wi-Fi and the access point;
  - the web UI and OTA update;
  - the Moonraker client and pairing.
- The pulse train is identical to the 2.0.17 one, checked by capture:
  - 120 ms SYNC LOW, then 2 ms HIGH and 2 ms LOW per count;
  - a frame every 1000 ms;
  - idle low.
- Settings in NVS (Wi-Fi, source, Moonraker, pairing token) survive the
  update from 2.0.17.

## Non-goals

- The S3-ETH board, Ethernet and the W5500. That is the next design.
- Arduino-ESP32 4.0. It was released two days ago and is too new.
- Rewriting working code that 3.x still accepts.

## Anti-goals

- A pulse train that differs from 2.0.17 in any width or period. The iHeater's
  STM32 reads it, and no iHeater is on the bench to catch a mistake.
- A firmware that aborts at boot because the old and new RMT drivers are both
  linked.
- An update that wipes the user's settings or pairing token.

## Constraints

- PlatformIO's official `espressif32` platform stops at Arduino 2.0.17. 3.x
  needs the pioarduino platform, pinned to a release at least two weeks old:
  `55.03.312-1` (Arduino 3.3.12, ESP-IDF 5.5.5, 22 Sep).
- The C3 app partition is 1.25 MB, and the 2.0.17 build already uses 82 %
  of it. 3.x builds are larger.
- Test hardware:
  - a spare C3 Super Mini on COM23, flashed with 2.0.17 and on Wi-Fi;
  - the S3-ETH on COM24, not flashed.

  Neither is connected to an iHeater. The user allows flashing both. The
  Remote on the printer is not touched.
- Klipper/Moonraker test host: elpis, free to use.
- The user has a Saleae logic analyser (Logic 2) for the pulse capture.

## Approaches

1. **Upgrade the framework on 2.0.17 code, changing only what 3.x refuses**
   (chosen). Pin pioarduino, port the RMT output to the new driver, and fix
   the task-watchdog call and whatever else breaks the build.
2. **Upgrade and add the S3-ETH together.** One step fewer, but a breakage
   could come from either change. Rejected for that reason.
3. **Keep 2.0.17 and bolt ESP-IDF's W5500 driver on.** Rejected earlier by the
   user, who prefers a current framework.

## Design

- `platformio.ini`: `platform =` the pioarduino `55.03.312-1` release URL.
  Keep the libraries (ArduinoJson 6, WebSockets 2.7.3) unless they fail to
  build. If one fails, move it to the oldest version that builds on 3.x.
- `RmtOutputAdapter`: move from the legacy `driver/rmt.h` to the ESP-IDF 5 RMT
  transmit API, with:
  - 1 MHz resolution (1 tick = 1 µs);
  - output idle low;
  - the same item splitting for durations over 32 767 µs;
  - the same dedicated FreeRTOS task, frame period and non-blocking send.

  Its public interface (`begin`, `apply`, `getLastPulseCode`, `isEnabled`) is
  unchanged.
- `UartLink`: `esp_task_wdt_init` takes the ESP-IDF 5 config struct, with the
  same timeout and panic behaviour.
- Partitions: if the 3.x build does not fit 1.25 MB, switch to
  `min_spiffs.csv` (two 1.875 MB app slots). It keeps NVS at the same offset,
  so settings survive. It moves LittleFS, so that partition must be
  re-uploaded once over USB (it holds only `logo.png`). This change cannot go
  over OTA; the first 3.x install is a USB flash.

## Verification

1. **Baseline:** on COM23, which already runs 2.0.17, the user captures the
   pulse pin (GPIO3) for about 5 s with Logic 2, in Off and at one heating
   target, and exports the digital transitions as CSV.
2. Flash the 3.x build to COM23 over USB and capture again under the same
   conditions.
3. A script compares the two exports:
   - every pulse width within ±10 µs;
   - the frame period within ±5 ms;
   - the same pulse count per frame.
4. After the update:
   - the C3 rejoins Wi-Fi with its stored settings;
   - the web UI, the setup page and the logo load;
   - an OTA upload of the same build through the web UI works.
5. On elpis: pair the C3 by token with a `[heater_iheater_remote]` section,
   and check that M141 reaches the Remote and the reports reach Klipper.
6. UART output mode: with no iHeater connected, the link reports `no-reply`
   and the Remote does not crash or reset.
7. All five gates pass.

## Left to the builder

- Whether to use ESP-IDF's `rmt_tx` API or Arduino 3.x's `rmtWrite` wrapper
  for the RMT port, as long as the waveform and task model stay the same.
- Any other 3.x build fixes.
