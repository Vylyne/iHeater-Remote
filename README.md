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
  WebSocket client and follows one `heater_iheater_remote` heater that you pair by
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
| `GPIO3` (RX) | `D−` | iHeater to Remote |
| `5V` | `VBUS` | iHeater power |
| `GND` | `GND` | common ground |

- Leave `3V3` unconnected at the iHeater.
- `GPIO3` is also the pulse output pin. Keep **Output** on UART while
  this cable is fitted: in pulse mode the Remote would drive the iHeater's
  TX line.
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
- Holding BOOT for 10 seconds while the Remote is running also resets the web
  login (see [Login and security](#login-and-security)).

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
  Add it to your `[heater_iheater_remote]` section as `remote_token`
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
- **Manual override:** while Klipper is the source, a manual press (web UI
  or BOOT button, Off included) overrides Klipper's target instead of
  switching the source. The Remote notes Klipper's target at that moment.
  - Klipper re-sending that same target, as Moonraker does on every
    reconnect, is ignored. Any other target ends the override and applies at
    once, `0` included.
  - The dashboard shows "Manual override, Klipper wants N °C" and a **Back to
    Klipper** button, which ends the override and applies Klipper's current
    target.
  - If the manual timer runs out during an override, the override ends and
    Klipper's current target applies; it does not just turn off.
  - A restart ends the override, and so does a lost pairing. The 10 minute
    link-loss cut still applies.
  - Boot, the timer and OTA never switch the source. To heat without the
    printer host, choose **Manual controls only** on the Setup page.
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

## Login and security

The Setup and firmware pages, and every action (changing a mode, saving
settings, OTA), need a login. The dashboard page and its live status
WebSocket stay readable without one.

### Set a password straight away

A new Remote has no password. Until one is set, whoever reaches the Remote
first can set it, and a malicious web page that rebinds a DNS name to the
Remote could do the same. Open the Remote after installing or updating and
set a password at once on `/login`. If someone else got there first, hold
BOOT for 10 seconds (below) and start again.

### Plain HTTP

The login never sends the password. The page sends a proof that depends on a
single-use challenge, so a listener can't replay a login. But the first
password (or a change) sends a value that is equivalent to the password for
this Remote, so someone on the network can capture enough to log in. Use
HTTPS through nginx (below), or the Remote's own access point. An access
point whose password is shorter than 8 characters is open, and anyone nearby
can read what you send.

Sessions last 30 days of the Remote's running time, up to four at once (a
fifth login replaces the oldest), and they survive a restart. They count
running hours, not wall-clock time, because the Remote has no clock. Time
while it's unplugged doesn't count, and a Remote that restarts more often
than hourly never ages its sessions. The cookie's own 30 day limit still
applies in the browser.

Five wrong passwords in a row lock that client out for 30 s. The Remote
tracks up to eight clients.

### BOOT reset

Hold BOOT for 10 seconds while the Remote is running. Heat still goes off at
2 seconds, as before. The reset clears:

- the password and all sessions;
- header login and proxy-only mode.

It keeps Wi-Fi, the Klipper pairing, the trusted proxies and the automatic
Moonraker entry. The LED stays on for 3 seconds to confirm. No restart is
needed, and releasing the button afterwards does nothing more.

### HTTPS with nginx

The Remote doesn't speak HTTPS itself. Put nginx in front of it. On a printer
host with nginx, `./install.sh` offers an HTTPS site for the Remote as its
last step. It asks for the server name, the TLS certificate and key paths,
and the Remote's IP address or name, fills them into
[`scripts/nginx-site.conf`](scripts/nginx-site.conf), writes
`sites-available/iheater-remote` and its link in `sites-enabled`, runs
`nginx -t` and reloads nginx. If the test fails, it rolls the site back. It
never edits `nginx.conf` or other sites. `./uninstall.sh` offers to remove
that site and its link, keeping a backup, and puts them back if `nginx -t`
fails without them. A site it didn't write is left alone. It skips the step when the host has
no `sites-available` or `sites-enabled` directory; those hosts can use the
template by hand, replacing `@SERVER_NAME@`, `@CERT@`, `@KEY@` and
`@REMOTE@`. Its server block:

```nginx
server {
    listen 443 ssl;
    server_name @SERVER_NAME@;

    ssl_certificate @CERT@;
    ssl_certificate_key @KEY@;

    client_max_body_size 4m;   # the OTA image is about 1.4 MB

    location / {
        proxy_pass http://@REMOTE@;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Host $http_host;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-authentik-username "";
        proxy_set_header X-authentik-groups "";
    }

    location /ws {
        proxy_pass http://@REMOTE@:81/;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Host $http_host;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-authentik-username "";
        proxy_set_header X-authentik-groups "";
    }
}
```

Each location repeats the whole header list because nginx drops inherited
`proxy_set_header` lines once a location sets its own. The two authentik
headers are blanked, so a client can never send them itself.

### Trusted proxies

The Remote believes `X-Forwarded-*` headers, and header login, only from a
trusted proxy, judged by the TCP peer address alone. The **Security** card on
the Setup page holds up to four IPv4 addresses. Add nginx's address there.

- **Automatic Moonraker entry:** while the source is Klipper, the address of
  the Remote's latest successful Moonraker connection is trusted too, so
  nginx on the printer host works with no setup. It is on by default, and a
  checkbox turns it off. It is trusted only while the source is Klipper, and
  the Remote clears it if it can't resolve the Moonraker host. Proxy-only
  mode doesn't count this entry: type the proxy's address into the trusted
  list for that.
- **nginx in Docker:** the peer is then the Docker bridge's gateway address,
  not the container's. Add that address as a trusted proxy.
- **IPv6 clients:** behind a proxy, a client whose `X-Forwarded-For` entry is
  IPv6 is counted by the proxy's address. All such clients then share one
  lockout slot, so five wrong passwords from one lock them all out for 30 s.
  The same holds for clients that reach the Remote directly over IPv6: the
  Remote reads IPv4 peer addresses only, so an IPv6 peer is never trusted as
  a proxy and shares that one slot.

Trust covers the whole host: anything on a trusted address can send forwarded
headers and, with header login on, log in.

### Header login (authentik)

Header login lets a trusted proxy log a user in from a header, such as the
one authentik sets. It is off by default. Turn it on in the Security card,
and set the user header (default `X-authentik-username`), an optional list of
allowed users, the groups header (default `X-authentik-groups`) and an
optional list of allowed groups. An empty list means no check.

**Warning:** anyone who can reach a trusted proxy can be logged in by it, the
automatic Moonraker entry included. Turn it on only if every trusted proxy
sits behind your sign-in. It sets no cookie, so logging out is authentik's
job. A valid session cookie works either way.

The template blanks both headers by default. To use authentik, uncomment its
`auth_request` block and lines in both locations, and give the two headers
the `$authentik_*` variables instead of `""`. The commented block has no
sign-in redirect (`error_page 401`). The
[authentik proxy provider docs](https://docs.goauthentik.io/add-secure-apps/providers/proxy/)
cover the redirect.

### Proxy-only mode

**Only allow actions through the HTTPS proxy** refuses protected pages, login
endpoints and actions unless the peer is a trusted proxy that sent
`X-Forwarded-Proto: https`. The dashboard page and the status WebSocket stay
open. It is off by default.

- You can turn it on only from the HTTPS address, so you know it works.
- A save that leaves it on must have the address of the proxy you're using in
  the trusted proxy list. The automatic Moonraker entry doesn't count,
  because it disappears when the source switches to Manual, when Moonraker is
  down at boot or when the host can't be resolved, and that would lock you
  out. The Remote refuses the save otherwise.
- The Remote's own access point ignores this setting, as if it were off. It
  opens only when the Remote can't join Wi-Fi at start-up, so it isn't a
  dependable way back in while Wi-Fi works. If a proxy breaks, hold BOOT for
  10 s to turn proxy-only off again. That reset also clears the password.

## Updating the iHeater's firmware

With the output set to UART, the **/fw** page has an **iHeater firmware**
card that updates the iHeater (STM32) over the link, through the iHeater's own
link bootloader. It does not appear in pulse output.

The iHeater must already hold that bootloader. Install it the first time from
a PC with `link_flash.py --enter` (or with an ST-Link), not from the Remote.

1. Choose the iHeater's UART app build `.bin`. The Remote checks it before it
   touches the iHeater. It accepts only `build_type` 2 and refuses a file that
   is under 8 B or over 28,672 B, has a bad vector table, or has a marker
   record with the wrong build type, length or CRC, or an unsupported
   protocol (a pulse build, a marker image cut short or a changed byte all
   fail this way). It also refuses an older image with no build
   record, because that file can't be checked.
2. Press **Update iHeater**. The update refuses to start while heat is
   wanted: Manual on, or a Klipper target above 0.
3. **Cool-down.** The Remote sets the air target to 0 and waits for the heater
   to read 50 C or lower. It gives up after 30 minutes, and stops if heat
   becomes wanted.
4. The Remote then asks the running firmware to enter the bootloader, erases
   the application, writes the image in 128-byte chunks, and boots it. The
   page shows the phase and a progress bar. **Cancel** works until the erase
   is sent; after that the update runs to its end.

From the erase until the iHeater boots the new firmware, the iHeater has no
application and its heater and fan stay off. Keep the Remote and the iHeater
powered. Setup, Wi-Fi and integration saves and Remote OTA return 409 while an
update runs.

If an update fails, the page says why, and whether the iHeater still has its
firmware. **Retry** reruns the whole job. The bootloader also says why it
could not finish (its error code and the application check), and the page
shows both.

**Recovery.** A board left in its bootloader, whether by a failed update, a
power loss or a link loss, shows on the page: "The iHeater is in its
bootloader with no valid firmware". Upload the app build and press **Update
iHeater**; the job starts from the bootloader and skips the cool-down. A
board that was only asked to enter (why 1) and not yet erased returns to its
firmware by itself within the time the page shows, so wait for that before
starting. Once an erase has been sent, the bootloader stops that timer and
waits for firmware for as long as it takes, even if the Remote restarts in
between; the page then shows the "no valid firmware" text and offers
**Update iHeater**.

While an update runs, the Remote reports the link as `updating`, and Klipper
shows it. Update the host's Klipper and Moonraker parts first (run
`./install.sh` again). Until then, they refuse the reports during an update,
which is harmless.

## Updating from 2.0

Settings from 2.0 (idryer-core) carry over once: the Remote reads them at
the first boot, saves them in its own namespace and erases the old ones. A
Moonraker setup from 2.0 becomes source Klipper with the same host, port,
TLS flag and key.

On the Klipper side, remove the old heater section and the `VIRTUAL_CHAMBER`
macro, then install the Klipper module with `./install.sh` and set
`remote_token` (shown on the Setup page) in its
`[heater_iheater_remote chamber]` section. Until you do, the Remote shows "not
paired". See [klipper/README.md](klipper/README.md).

## Notes

- Do not use `erase_flash` if you want to preserve NVS data from another
  firmware build.
- ESP32 Wi-Fi is 2.4 GHz only.
- The iHeater controller should remain responsible for heater safety and
  fault handling.
