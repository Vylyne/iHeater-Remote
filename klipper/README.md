# Klipper chamber heater for iHeater Remote

`heater_virtual` makes the iHeater show up in Klipper as a normal chamber
heater:

- Slicer `M141` / `M191` work.
- Mainsail shows a "chamber" heater row with a settable target.
- `TURN_OFF_HEATERS` and the Mainsail off button turn it off.

Klipper never drives the heating element. It only publishes the chamber
target; iHeater Remote reads it over Moonraker and sends it to the iHeater,
which regulates from its own probe.

## Install

On the printer host:

```bash
cd ~
git clone https://github.com/Vylyne/iHeater-Remote.git
ln -s ~/iHeater-Remote/klipper/heater_virtual.py ~/klipper/klippy/extras/
cp ~/iHeater-Remote/klipper/iheater_remote.cfg ~/printer_data/config/
```

In `printer.cfg`:

```ini
[include iheater_remote.cfg]
```

Delete any existing `M141`, `M191`, `VIRTUAL_CHAMBER` and
`CLEAR_VIRTUAL_CHAMBER` macros: `iheater_remote.cfg` defines its own.
Edit `sensor:` in `iheater_remote.cfg` if your chamber sensor is not
`temperature_sensor chamber_hexa`, then restart Klipper.

In the Remote's Setup page, choose **Klipper (Moonraker)** as the control
source.

## Options

```ini
[heater_virtual chamber]
sensor: temperature_sensor chamber_hexa
#max_target: 80
#mirror_macro: VIRTUAL_CHAMBER
```

| Option | Default | Meaning |
| --- | --- | --- |
| `sensor` | required | Existing sensor object whose reading is the chamber temperature (used by Mainsail and `M191`). |
| `max_target` | `80` | Highest chamber target Klipper accepts (45–90). Higher requests are a gcode error. |
| `mirror_macro` | `VIRTUAL_CHAMBER` | `gcode_macro` whose `target` / `temperature` / `has_sensor` variables the Remote reads. |

The section name must start with `heater_`: Mainsail only offers a target
box for `extruder*` and `heater_*` objects.

## Commands

- `M141 S60`: set the chamber target to 60 °C (`S0` turns it off).
- `M191 S60`: set the target and wait until `sensor` reads at least 60 °C.
- `SET_HEATER_TEMPERATURE HEATER=chamber TARGET=60`: same as `M141`.

`M191` waits on `sensor`, not on the iHeater's own probe. If your chamber
sensor reads cooler than the iHeater's probe, wait for a lower value or
`M191` may never finish.

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
