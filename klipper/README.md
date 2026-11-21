# Klipper chamber heater for iHeater Remote

`heater_virtual` makes the iHeater show up in Klipper as a normal chamber
heater:

- Slicer `M141` / `M191` work.
- Mainsail shows a "chamber" heater row with a settable target.
- `TURN_OFF_HEATERS` and the Mainsail off button turn it off.

Klipper never drives the heating element. It only publishes the chamber
target; iHeater Remote reads the heater object over Moonraker, matches it by
`remote_token`, and sends the target to the iHeater, which regulates from its
own probe.

With the Moonraker component installed the link is two-way. The Remote also
sends the iHeater's readings back, so Klipper shows the chamber air
temperature, the heater power, the fan, the iHeater state and any fault, and
`M191` waits on the air reading. If the Remote stops reporting, the heater
falls back to `sensor:` (or to the last air reading when `sensor:` is not
set) and `linked` turns false.

## Install

On the printer host:

```bash
cd ~
git clone https://github.com/Vylyne/iHeater-Remote.git
cd ~/iHeater-Remote && ./install.sh
```

The script links `heater_virtual.py` into Klipper and the Moonraker component
into Moonraker. It copies `iheater_remote.cfg` to
`~/printer_data/config/addons/`, adds `[include addons/iheater_remote.cfg]` to
`printer.cfg`, and adds the `[iheater_remote]` and
`[update_manager iheater-remote]` sections to `moonraker.conf`. It asks for the
pairing token shown on the Remote's Setup page and for your chamber sensor
(both can stay blank), and asks before replacing a link, upgrading an older
config file, adding the include or restarting Klipper. It warns about any other
`M141` or `M191` macro it finds. It backs up every file it edits as
`<file>.bak-<date>-<time>`, and it never restarts Klipper during a print.
Running it again is safe: steps that are already done say `skip`.

These environment variables change where it looks:

| Variable         | Default             |
| ---------------- | ------------------- |
| `KLIPPER_PATH`   | `~/klipper`         |
| `MOONRAKER_PATH` | `~/moonraker`       |
| `PRINTER_DATA`   | `~/printer_data`    |
| `MOONRAKER_PORT` | `7125`              |
| `PYTHON_BIN`     | `python3` on `PATH` |

To remove it, run `./uninstall.sh`. It takes out the `moonraker.conf`
sections, the include and the two links, and asks before deleting
`addons/iheater_remote.cfg` (a backup is kept).

To update, run `git pull` (or use the update manager in Mainsail), then re-run
`./install.sh` when [`CONFIG_CHANGES.md`](CONFIG_CHANGES.md) lists a new
version.

In the Remote's Setup page, choose **Klipper (Moonraker)** as the control
source.

### Manual install

```bash
ln -s ~/iHeater-Remote/klipper/heater_virtual.py ~/klipper/klippy/extras/
mkdir -p ~/printer_data/config/addons
cp ~/iHeater-Remote/klipper/iheater_remote.cfg ~/printer_data/config/addons/
```

In `printer.cfg`:

```ini
[include addons/iheater_remote.cfg]
```

Delete or rename any existing `M141` and `M191` macros: `iheater_remote.cfg`
defines its own. Klipper merges duplicate sections, so both definitions would
combine and the one loaded last would silently replace the other.
Edit `sensor:` in `iheater_remote.cfg` if your chamber sensor is not
`temperature_sensor chamber_hexa`, set `remote_token` to the value shown on
the Remote's Setup page, then restart Klipper.

Install the Moonraker component that carries the Remote's reports to Klipper;
see [`moonraker/README.md`](../moonraker/README.md), then choose
**Klipper (Moonraker)** as the control source as above.

## Options

```ini
[heater_virtual chamber]
sensor: temperature_sensor chamber_hexa
#max_target: 80
#remote_token: XXXX-XXXX
```

| Option | Default | Meaning |
| --- | --- | --- |
| `sensor` | optional | Existing sensor object whose reading is the chamber temperature whenever there is no fresh air reading from the Remote (used by Mainsail and `M191`). Without it, the temperature stays at the last air reading received (0 before the first). Keep it in pulse mode, where the Remote sends no readings. If set, the object must exist at connect. |
| `max_target` | `80` | Highest chamber target Klipper accepts (45–90). Higher requests are a gcode error. Also published as `max_temp` so Mainsail/Fluidd limit their target box to it; do not set `max_temp` yourself. |
| `remote_token` | empty | Pairing token from the Remote's Setup page: 8 characters from `0123456789ABCDEFGHJKMNPQRSTVWXYZ`, written `XXXX-XXXX`; case and hyphens are ignored. Two `heater_virtual` sections cannot share a token. |

The section name must start with `heater_`: Mainsail only offers a target
box for `extruder*` and `heater_*` objects.

## Element temperature sensor

The `iheater_remote` sensor type exposes a reading from the Remote's latest
report as an ordinary temperature sensor:

```ini
[temperature_sensor chamber_element]
sensor_type: iheater_remote
heater: chamber          # the heater_virtual's short name
reading: element         # element (default) or air
```

The `iheater_remote` sensor type is for `[temperature_sensor]` sections only,
not for heaters or temperature fans. It reports on the host clock.

With no fresh reading it reports the last value received (0 before the
first). The `[heater_virtual]` section must come before this
`[temperature_sensor]` section in your configuration, because the sensor type
is registered by `heater_virtual.py`. A `heater` that does not exist, or a
`reading` other than `element` or `air`, is a configuration error.

## Updating from the macro version

Delete `mirror_macro` from the `[heater_virtual]` section and delete the
`VIRTUAL_CHAMBER` macro, then add `remote_token`. Until you do, the Remote
shows "not paired" and does not heat from Klipper.

## Commands

- `M141 S60`: set the chamber target to 60 °C (`S0` turns it off).
- `M191 S60`: set the target and wait until the chamber temperature reads at
  least 60 °C. While linked, that is the iHeater's air reading; otherwise it
  is `sensor`.
- `SET_HEATER_TEMPERATURE HEATER=chamber TARGET=60`: same as `M141`.

While the Remote is not linked, `M191` waits on `sensor`, not on the
iHeater's own probe. If your chamber sensor reads cooler than the iHeater's
probe, wait for a lower value or `M191` may never finish.

## Start-offset warm-up in `START_PRINT`

To start printing before the chamber is fully warm, wait for a lower value
first, then set the full target:

```jinja
{% set c = params.CHAMBER_TEMP|default(0)|int %}
{% set offset = 10 %}
{% if c > 0 %}
  M191 S{[c - offset, 0]|max}
  M141 S{c}
{% endif %}
```

## Checking the link in `START_PRINT`

`linked` is true while the Remote's reports are arriving and the iHeater
answers. To stop a print that would otherwise wait for a heater nobody is
driving, add this to your own `START_PRINT`:

```jinja
{% if not printer["heater_virtual chamber"].linked %}
  {action_raise_error("Chamber heater not linked: check the iHeater Remote")}
{% endif %}
```

## Limits

- **Chamber target:** Klipper's `max_target`, then the Remote's
  "iHeater target range" (Setup page, default 45–85). Targets below the
  Remote's Min run at Min.
- **Housing safety:** the heating element's temperature cap is the
  iHeater's own `MAX_TEMP` in iHeater-Standalone-Firmware
  `Core/Inc/config.h` (stock v0.5.0: 110 °C, fault at 120 °C). Rebuild the
  iHeater firmware with a lower value, e.g. 100 for ABS housings. Neither
  Klipper nor the Remote can enforce it: the pulse link only carries a
  chamber target.

## Fail-safe

- A Klipper shutdown sets the target to 0 immediately.
- If the Remote hears nothing from Moonraker for 30 s while connected (for
  example after `FIRMWARE_RESTART`), it turns heat off and reconnects.
