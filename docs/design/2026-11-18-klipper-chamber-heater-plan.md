# Klipper Chamber Heater Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the iHeater, driven by iHeater Remote over Moonraker, behave as a native Klipper chamber heater (`M141`/`M191`, Mainsail target box, `TURN_OFF_HEATERS`), with a chamber-target cap and fail-safe off.

**Architecture:** A Klipper extras module `[heater_virtual chamber]` registers a pin-less heater and mirrors its target/temperature into `gcode_macro VIRTUAL_CHAMBER`, which idryer-core already subscribes to. The Remote firmware gets a small pure-logic library (target clamp, target-source resolution, staleness check) wired into `StandaloneApp.cpp`, plus a watchdog that restarts the Moonraker client when updates stop.

**Tech Stack:** Python 3 (Klipper extras, stdlib `unittest` with fakes), C++ / Arduino-ESP32 / PlatformIO (Unity tests on device), ArduinoJson 6.

**Spec:** [2026-11-18-klipper-chamber-heater-design.md](2026-11-18-klipper-chamber-heater-design.md)

## Global Constraints

- Pulse protocol: target in 1 °C steps, codes 0 and 10 mean off. Never send a non-off target outside the Remote's `[minTargetC, maxTargetC]`.
- Remote "iHeater target range": `minTargetC` default 45, `maxTargetC` default 85; valid `11 ≤ min ≤ max ≤ 90`; NVS keys `min-target`, `max-target` in namespace `iheater-remote`. Values describe the iHeater firmware (`PULSE_MIN_TARGET`, `MAX_AIR_TEMP`); the Remote does not and cannot change them. The iHeater ignores (keeps its previous target) any pulse count outside `45..MAX_AIR_TEMP`, and faults when air reaches `MAX_AIR_TEMP`.
- Setup card copy (exact): heading `iHeater target range`; note 1: `These values do not change any setting on the iHeater. Stock v0.5.0 firmware accepts 45–90 and faults when the air reaches 90.`; note 2: `Min must not be lower than your iHeater's minimum: lower targets are ignored by the iHeater, which keeps its previous target.`; note 3: `We recommend a Max no higher than 80 (10 °C below the iHeater's MAX_AIR_TEMP).`
- Watchdog timeout: 30 000 ms.
- Klipper module: no `min_temp`/`max_temp` options; `sensor` required; `max_target` optional, default 80, range 45–90; `mirror_macro` optional, default `VIRTUAL_CHAMBER`.
- Section name prefix must be `heater_` (Mainsail target box rule).
- No changes to idryer-core (`lib/idryer-core` is a junction to a pinned upstream checkout).
- Firmware gate: `pio run -e esp32c3-super-mini-remote` succeeds.
- Code comments/docs in Canadian English.
- Work on branch `feature/klipper-chamber-heater`, not `main`.

## Review Focus

1. `M141 S0` while the bed is still above the bed threshold → heater stays off (bed rule must not re-engage while `VIRTUAL_CHAMBER` exists). Test: `test_resolve_vc_zero_ignores_hot_bed` (Task 2).
2. Klipper `FIRMWARE_RESTART` mid-heat → Remote turns off within ~30 s and follows the new (0) target after reconnect. Test: `test_stale_after_timeout` (Task 2) + manual check in Task 3 Step 7.
3. `millis()` wraparound → watchdog neither trips early nor never. Test: `test_stale_wraparound` (Task 2).
4. Slicer `M141 S40` → accepted by Klipper, iHeater runs at 45. Tests: `test_set_temp_low_accepted` (Task 1), `test_clamp_low_to_protocol_min` (Task 2).
5. Manual mode 7 (85 °C) or bed-rule target above the range → sent at `maxTargetC`. Test: `test_clamp_above_max` (Task 2).

---

### Task 0: Branch and commit the design

- [ ] **Step 1:** `git switch -c feature/klipper-chamber-heater`
- [ ] **Step 2:** Commit `docs/design/2026-11-18-klipper-chamber-heater-design.md` and this plan.

```bash
git add docs/design/
git commit -m "docs: design and plan for Klipper chamber heater"
```

---

### Task 1: Klipper module `heater_virtual`

**Files:**
- Create: `klipper/heater_virtual.py`
- Create: `klipper/iheater_remote.cfg`
- Test: `klipper/test_heater_virtual.py`

**Interfaces:**
- Produces (Klipper object `heater_virtual <name>`):
  - `HeaterVirtual(config)`; module-level `load_config_prefix(config) -> HeaterVirtual`.
  - `set_temp(degrees: float) -> None` — raises `printer.command_error` if `degrees < 0` or `> max_target`.
  - `get_temp(eventtime) -> (float, float)` — `(sensor temp, target)`.
  - `check_busy(eventtime) -> bool` — `target > 0 and temp < target`.
  - `get_status(eventtime) -> {'temperature': float, 'target': float, 'power': 1.0|0.0}`.
  - Mirrors into `gcode_macro <mirror_macro>.variables`: `target`, `temperature`, `has_sensor=1`.
- Klipper hooks used (stock `klippy/extras/heaters.py`): `printer.load_object(config, 'heaters')`, `pheaters.heaters[short]`, `pheaters.available_heaters.append(full)`, `pheaters.register_sensor(config, self)`, `gcode.register_mux_command('SET_HEATER_TEMPERATURE', 'HEATER', short, handler)` where the handler calls `pheaters.set_temperature(self, gcmd.get_float('TARGET', 0.))`.
- Events: `klippy:connect` (resolve sensor + macro; missing → `config.error` naming the option), `klippy:ready` (start 1 s reactor timer that mirrors temperature), `klippy:shutdown` (target → 0, mirror).

- [ ] **Step 1: Write failing tests** in `klipper/test_heater_virtual.py` using in-file fakes (`FakePrinter`, `FakeConfig`, `FakeHeaters`, `FakeGcode`, `FakeSensor(temp)`, `FakeMacro(variables)`, `FakeReactor`). Import the module via `sys.path` insert of `klipper/`. Tests:

```python
def test_registers_as_heater(self):
    hv = make(sensor_temp=21.5)
    self.assertIs(self.heaters.heaters['chamber'], hv)
    self.assertIn('heater_virtual chamber', self.heaters.available_heaters)
    self.assertIn('heater_virtual chamber', self.heaters.registered_sensors)
    self.assertIn(('SET_HEATER_TEMPERATURE', 'HEATER', 'chamber'), self.gcode.mux)

def test_set_temp_mirrors_to_macro(self):
    hv = make_connected(sensor_temp=30.0)
    hv.set_temp(60.)
    self.assertEqual(self.macro.variables['target'], 60.)
    self.assertEqual(self.macro.variables['temperature'], 30.0)
    self.assertEqual(self.macro.variables['has_sensor'], 1)
    self.assertEqual(hv.get_status(0.)['power'], 1.0)

def test_set_temp_low_accepted(self):      # Review Focus 4
    hv = make_connected(); hv.set_temp(40.)
    self.assertEqual(hv.get_status(0.)['target'], 40.)

def test_set_temp_above_max_target_rejected(self):
    hv = make_connected(max_target=70.)
    with self.assertRaises(CommandError): hv.set_temp(71.)
    self.assertEqual(hv.get_status(0.)['target'], 0.)

def test_set_temp_negative_rejected(self): ...   # -1 → CommandError

def test_max_target_above_90_is_config_error(self): ...  # max_target=91 → ConfigError
def test_max_target_below_45_is_config_error(self): ...  # max_target=44 → ConfigError
def test_missing_sensor_option_is_config_error(self): ...
def test_missing_macro_on_connect_is_config_error(self):
    # error message contains 'VIRTUAL_CHAMBER'
def test_missing_sensor_object_on_connect_is_config_error(self): ...
def test_duplicate_heater_name_is_config_error(self): ...
def test_shutdown_zeroes_and_mirrors(self):
    hv = make_connected(); hv.set_temp(60.); self.printer.fire('klippy:shutdown')
    self.assertEqual(self.macro.variables['target'], 0.)
def test_timer_mirrors_temperature(self):
    hv = make_ready(sensor_temp=25.0); self.sensor.temp = 26.5
    nxt = self.reactor.timers[0](100.)
    self.assertEqual(self.macro.variables['temperature'], 26.5)
    self.assertEqual(nxt, 101.)
def test_get_temp_and_check_busy(self):
    hv = make_connected(sensor_temp=40.); hv.set_temp(50.)
    self.assertEqual(hv.get_temp(0.), (40., 50.))
    self.assertTrue(hv.check_busy(0.)); self.sensor.temp = 50.
    self.assertFalse(hv.check_busy(0.))
```

- [ ] **Step 2: Run, expect failure** — `python -m unittest discover -s klipper -p "test_*.py" -v` → ImportError / failures.

- [ ] **Step 3: Implement `klipper/heater_virtual.py`.** Read options with `config.get('sensor')`, `config.getfloat('max_target', 80., minval=45., maxval=90.)`, `config.get('mirror_macro', 'VIRTUAL_CHAMBER')`. Mirror by replacing the dict (`macro.variables = dict(macro.variables, target=..., temperature=..., has_sensor=1)`), never via gcode, so it is safe from event handlers. Sensor temperature via `sensor.get_temp(eventtime)[0]`. Licence header: GPLv3 (Klipper extras are loaded into a GPLv3 process).

- [ ] **Step 4: Run tests, expect all pass.**

- [ ] **Step 5: Write `klipper/iheater_remote.cfg`:**

```ini
[heater_virtual chamber]
sensor: temperature_sensor chamber_hexa
#max_target: 80

[gcode_macro VIRTUAL_CHAMBER]
variable_target: 0
variable_temperature: -1
variable_has_sensor: 0
gcode:

[gcode_macro M141]
gcode:
  SET_HEATER_TEMPERATURE HEATER=chamber TARGET={params.S|default(0)|float}

[gcode_macro M191]
gcode:
  {% set t = params.S|default(0)|float %}
  M141 S{t}
  {% if t > 0 %}
    TEMPERATURE_WAIT SENSOR="heater_virtual chamber" MINIMUM={t}
  {% endif %}
```

- [ ] **Step 6: Commit** `git add klipper/ && git commit -m "feat(klipper): heater_virtual chamber heater module"`

---

### Task 2: Remote target policy library

**Files:**
- Create: `lib/remote-policy/src/RemotePolicy.h`, `lib/remote-policy/src/RemotePolicy.cpp`, `lib/remote-policy/library.json` (`{"name": "remote-policy", "version": "1.0.0"}`)
- Test: `test/test_remote_policy/test_main.cpp`
- Modify: `.gitignore` — replace the `test/` line with `test/*` and `!test/test_remote_policy/`

**Interfaces (namespace `iheaterlink::policy`, no Arduino dependency):**
- `constexpr uint8_t kRangeFloorC = 11; constexpr uint8_t kRangeCeilC = 90; constexpr uint8_t kDefaultMinC = 45; constexpr uint8_t kDefaultMaxC = 85; constexpr uint32_t kMoonrakerStaleMs = 30000;`
- `struct TargetRange { uint8_t minC; uint8_t maxC; };`
- `bool validRange(int minC, int maxC);` — `kRangeFloorC ≤ minC ≤ maxC ≤ kRangeCeilC`.
- `float clampTargetC(float requestedC, TargetRange r);` — `requestedC <= 0` → `0`; else clamp to `[r.minC, r.maxC]`.
- `struct MoonrakerInputs { bool virtualChamberAvailable; float chamberTargetC; float bedTargetC; };`
- `float resolveMoonrakerTargetC(const MoonrakerInputs &in, float autoTargetC, uint8_t bedThresholdC, TargetRange r);` — VC available → `clampTargetC(chamberTargetC, r)`; else `bedTargetC >= bedThresholdC` → `clampTargetC(autoTargetC, r)`; else `0`.
- `bool moonrakerStale(uint32_t nowMs, uint32_t lastUpdateMs, uint32_t timeoutMs);` — `(uint32_t)(nowMs - lastUpdateMs) >= timeoutMs`.

- [ ] **Step 1: Write failing Unity tests** (`setup()` runs `UNITY_BEGIN()`, all `RUN_TEST`s, `UNITY_END()`; empty `loop()`):

```cpp
// R = {45,90} unless stated
test_valid_range:             validRange(45,90); validRange(11,11); !validRange(10,90); !validRange(60,50); !validRange(45,91)
test_clamp_off:               clampTargetC(0,R)==0; clampTargetC(-5,R)==0
test_clamp_low_to_min:        clampTargetC(40,R)==45; clampTargetC(40,{50,90})==50   // Review Focus 4
test_clamp_passthrough:       clampTargetC(60,R)==60; clampTargetC(85,R)==85
test_clamp_above_max:         clampTargetC(95,R)==90; clampTargetC(85,{45,70})==70   // Review Focus 5
test_resolve_vc_target:       {true,60,0} auto 60 thr 100 R → 60
test_resolve_vc_zero_ignores_hot_bed: {true,0,110} → 0      // Review Focus 1
test_resolve_vc_clamped:      {true,78,0} {45,70} → 70
test_resolve_bed_rule_on:     {false,0,110} auto 60 thr 100 → 60
test_resolve_bed_rule_off:    {false,0,90}  auto 60 thr 100 → 0
test_stale_before_timeout:    moonrakerStale(29999,0,30000)==false
test_stale_after_timeout:     moonrakerStale(30000,0,30000)==true   // Review Focus 2
test_stale_wraparound:        moonrakerStale(5,0xFFFFFFF0u,30000)==false; moonrakerStale(30000,0xFFFFFFF0u,30000)==true  // Review Focus 3
```

Use `TEST_ASSERT_EQUAL_FLOAT` / `TEST_ASSERT_TRUE` / `TEST_ASSERT_FALSE`.

- [ ] **Step 2: Build tests, expect failure** — `pio test -e esp32c3-super-mini-remote -f test_remote_policy --without-uploading --without-testing` → compile error (missing header).

- [ ] **Step 3: Implement** `RemotePolicy.h/.cpp` per the interface block.

- [ ] **Step 4: Build tests, expect success** (same command). If an ESP32-C3 Super Mini is connected, also run on device: `pio test -e esp32c3-super-mini-remote -f test_remote_policy --upload-port <COMx> --test-port <COMx>` → all PASS. If no device, record "built, not run on device" in the task report.

- [ ] **Step 5: Commit** `git add lib/remote-policy test/test_remote_policy .gitignore && git commit -m "feat(remote): target policy library with tests"`

---

### Task 3: Wire policy, cap and watchdog into the Remote

**Files:**
- Modify: `src/standalone/StandaloneApp.cpp` (settings ~60–118, `applyIntegrationTarget` ~120, `onMoonrakerStatus` ~145, `onBambuPrinterStatus` ~152, `applyMode` ~177, `loadAutoSettings`/`saveAutoSettings` ~288–316, setup page ~378–395, status JSON ~408–436, `applySetupForm` ~550–560, `loop` ~1190)

**Interfaces:**
- Consumes: everything in Task 2's interface block (`#include <RemotePolicy.h>`).
- Produces: status JSON fields `minTargetC`, `maxTargetC` (int) and `targetSource` (`"manual" | "klipper" | "bed-rule" | "bambu" | "watchdog"`); setup form fields `minTargetC`, `maxTargetC`.

- [ ] **Step 1: Target range setting.** Add keys `kMinTargetKey = "min-target"`, `kMaxTargetKey = "max-target"` and `policy::TargetRange s_range{policy::kDefaultMinC, policy::kDefaultMaxC};`. `loadAutoSettings` reads both (`getUChar`, defaults 45/85) and accepts them only if `policy::validRange`; it accepts a stored auto target only if it lies within the range. Change `saveAutoSettings(float target, int threshold, int minTarget, int maxTarget)` to require `validRange(minTarget, maxTarget)`, `minTarget ≤ target ≤ maxTarget`, threshold 40–120, and persist all four. In `applySetupForm`, read `minTargetC`/`maxTargetC` (blank → keep current); error text: `"Range must satisfy 11 ≤ min ≤ max ≤ 90, auto target must be inside it, bed threshold 40–120 C."`. Setup page: add a new card **before** "Auto control" with the exact heading and three notes from Global Constraints and inputs `<label>Min target (C)<input name=minTargetC type=number value="__MIN_TARGET__" min=11 max=90></label>` and `<label>Max target (C)<input name=maxTargetC type=number value="__MAX_TARGET__" min=11 max=90></label>`, plus the matching `page.replace` calls. In "Auto control", change the target label to `iHeater target (within range)` with `min`/`max` attributes removed (server validates), and the note to: "Klipper's chamber heater (VIRTUAL_CHAMBER) takes priority; otherwise heat engages when the bed setpoint reaches this threshold."

- [ ] **Step 2: Clamp everywhere.** Construct `s_output` with an `RmtOutputConfig` whose `minTempC = policy::kRangeFloorC` and `maxTempC = policy::kRangeCeilC` so the adapter no longer caps at 80 (the policy does the clamping). `applyIntegrationTarget(float targetTempC, const char *source)` applies `policy::clampTargetC(targetTempC, s_range)` before building the command; `applyMode` uses `policy::clampTargetC(kModeTempsC[mode], s_range)` for modes 1–7. Track `String s_targetSource` set by each caller (`applyMode` → `"manual"`).

- [ ] **Step 3: Moonraker target source.** `onMoonrakerStatus`: set `s_lastMoonrakerUpdateMs = millis()`; compute `policy::resolveMoonrakerTargetC({status.virtualChamberAvailable, status.chamberTarget, status.bedTarget}, s_autoTargetC, s_autoBedThresholdC, s_range)`; source `"klipper"` when VC available else `"bed-rule"`. `onBambuPrinterStatus` keeps the bed rule, source `"bambu"`. Leave `onMoonrakerVirtualChamber` as a no-op but update its comment.

- [ ] **Step 4: Watchdog.** Add `void updateMoonrakerWatchdog()` called from `loop()` after `s_integrations.loop()`:
  - Only when `getActive() == Moonraker` and `moonrakerState() == MoonrakerConnectionState::Connected`.
  - Track the transition into Connected: on entering Connected, set `s_lastMoonrakerUpdateMs = millis()` (so a fresh connection gets a full 30 s).
  - If `policy::moonrakerStale(millis(), s_lastMoonrakerUpdateMs, policy::kMoonrakerStaleMs)`: `applyIntegrationTarget(0, "moonraker stale")` with source `"watchdog"`, log `[REMOTE] moonraker stale, restarting client`, then `s_integrations.setActive(None); s_integrations.setActive(Moonraker);` and reset `s_lastMoonrakerUpdateMs = millis()`.

- [ ] **Step 5: Status JSON.** In the status builder add `doc["minTargetC"] = s_range.minC; doc["maxTargetC"] = s_range.maxC; doc["targetSource"] = s_targetSource;`.

- [ ] **Step 6: Build.** `pio run -e esp32c3-super-mini-remote` → SUCCESS; note flash % (was 84.1 %).

- [ ] **Step 7: On-printer check (user, with hardware).** With the Task 1 module installed:
  1. `M141 S60` → Remote dashboard target 60, `targetSource: klipper`.
  2. `M141 S0` with bed target ≥ threshold → stays off.
  3. `M141 S90` with default `max_target: 80` → Klipper error; `M141 S75` with Remote range 45–70 → pulse target 70; Setup page shows the range card with both notes.
  4. `M141 S60` then `FIRMWARE_RESTART` → Remote off within ~30 s, reconnects, target 0.
  5. `M141 S60`, then trigger a Klipper shutdown (e.g. `M112`) → Remote off immediately.

- [ ] **Step 8: Commit** `git commit -am "feat(remote): follow Klipper chamber target with cap and watchdog"`

---

### Task 4: Documentation

**Files:**
- Modify: `README.md` (section "What the Local Remote Does" ~lines 26–40 and the Setup/auto bullets ~150–161)
- Create: `klipper/README.md`

- [ ] **Step 1: `klipper/README.md`** covering: install (`ln -s ~/iHeater-Remote/klipper/heater_virtual.py ~/klipper/klippy/extras/`), `[include iheater_remote.cfg]`, delete old `M141`/`M191`/`CLEAR_VIRTUAL_CHAMBER` macros, options table (`sensor`, `max_target`, `mirror_macro`), `START_PRINT` start-offset pattern:

```jinja
{% set c = params.CHAMBER_TEMP|default(0)|int %}
{% set offset = 10 %}
{% if c > 0 %}
  M191 S{[c - offset, 0]|max}
  M141 S{c}
{% endif %}
```

  and a "Housing safety" note: the element cap is the iHeater's own `MAX_TEMP` in iHeater-Standalone-Firmware `Core/Inc/config.h` (stock v0.5.0: 110, fault at 120); rebuild with e.g. 100 for ABS housings. Neither Klipper nor the Remote can enforce it.

- [ ] **Step 2: Root `README.md`:** replace the bed-rule bullet with the priority rule (Klipper chamber heater first, bed rule as fallback), describe the "iHeater target range" card (records the iHeater firmware's range, default 45–85, does not change the iHeater; recommend Max ≤ 80; the iHeater ignores out-of-range targets and faults at `MAX_AIR_TEMP`), mention the watchdog, and link `klipper/README.md`.

- [ ] **Step 3: Commit** `git add README.md klipper/README.md && git commit -m "docs: Klipper chamber heater setup"`
