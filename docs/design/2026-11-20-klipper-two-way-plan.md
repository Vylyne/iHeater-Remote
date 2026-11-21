# Two-way Klipper Link Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Remote, in UART mode, reports iHeater readings to Klipper through a Moonraker component, so `heater_virtual` shows the real air temperature, real power and `linked`, and a new sensor type shows the element temperature.

**Architecture:** A pure send scheduler in `lib/remote-policy/` decides when the Remote reports. `MoonrakerLink` sends `server.iheater_remote.report` over its existing WebSocket. A new Moonraker component checks each report and forwards it to the Klippy webhooks endpoint `heater_virtual/report`. `heater_virtual.py` registers that endpoint, keeps the latest report per heater, and provides the `iheater_remote` sensor type.

**Tech Stack:** Arduino-ESP32 2.0.17, ArduinoJson 6, links2004/WebSockets, Unity (native tests), Python 3 `unittest` with fakes, Klipper extras API, and the Moonraker component API.

**Spec:** [2026-11-20-klipper-two-way-design.md](2026-11-20-klipper-two-way-design.md)

## Global Constraints

- **Language and endings:** write Canadian English in code comments and docs. Keep each file's existing line endings. New files match the files around them: C++ in `lib/`, `src/` and `test/` is LF, and Python in `klipper/` and `tools/` is CRLF, so the new `moonraker/` Python is CRLF.
- **Policy code:** pure decisions go in `lib/remote-policy/`, with no Arduino includes, covered by tests in `test/test_remote_policy/test_main.cpp`.
- **Report fields:** a report carries exactly these fields:
  - `remote_token` (8 characters, no hyphen)
  - `link` (`up`/`waiting`/`restarting`/`no-reply`)
  - `air`, `element`, `power` (number or `null`)
  - `fan` (bool)
  - `state` (string)
  - `fault` (int)
  - `fault_name` (string)
  - `next_within` (seconds)
- **Null readings:** `air`, `element` and `power` are `null` on `kNoReading`, with no snapshot, and whenever `link` is not `up`.
- **Send schedule:**
  - Send on a meaningful change, at most once per 1 s. A change is any change in air or element tenths, power percent (`pwmPermille / 10`), fan, state, fault, link, or the followed target.
  - Otherwise send a heartbeat every 5 s while the target is above 0 or the state is heating or hold, and every 15 s otherwise.
  - `next_within` is the interval that applies after this report.
- **When the Remote sends:** only in UART mode, only while subscribed to the paired heater, and with one report in flight at a time.
- **Method not found:** JSON-RPC error code `-32601` stops reports until the next reconnect. The dashboard source line then shows exactly: `Klipper plugin not installed: readings are not sent to Klipper`.
- **Freshness:** a report is fresh until `arrival + 2 × next_within + 2 s`, on reactor monotonic time. `linked` = fresh and `link == "up"`.
- **Klipper never refuses a target** because of the link. `M141`/`M191` and `iheater_remote.cfg` macros stay unchanged.
- **Moonraker method:** `server.iheater_remote.report` (POST `/server/iheater_remote/report`). Klippy endpoint: `heater_virtual/report`. Success reply: `{"ok": true}`.
- **Gates:** before every commit, run:
  ```bash
  pio run -e esp32c3-super-mini-remote
  export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native
  python -m unittest discover -s klipper -p "test_*.py"
  python -m unittest discover -s moonraker -p "test_*.py"   # once Task 3 adds it
  ```
  Redirect long `pio` output to a file and read its tail.
- **No device work:** never flash or upload to a device. The user flashes.
- **Commits:** each commit message ends with a blank line, then `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

These are the failure modes most likely to bite, most likely first:

1. **A bad report shuts Klipper down.** Klipper's webhooks shut the printer down on any handler exception that is not a `command_error`. A wrong type (`"air": "hot"`, `"fault": null`, a missing field) must come back as a `web_request.error` and nothing else. Task 2 pins this with `test_malformed_report_raises_command_error_only`.
2. **Extra report fields.** A newer Remote may send a field this version doesn't know. Both the component and Klipper must ignore unknown fields, not refuse the report. Pinned in Task 2 by `test_unknown_report_field_ignored` and in Task 3 by `test_unknown_field_forwarded`.
3. **`linked` stays false after a Klipper restart.** After `FIRMWARE_RESTART` the Remote resubscribes without reconnecting. If it waited for the next heartbeat, Klipper would show "not linked" for up to 15 s, and a `START_PRINT` check could fail for no reason. The first report after each new subscription must go at once. Pinned in Task 1 by `test_report_force_next_sends_without_change`.
4. **A boolean passes as a number.** In Python, `True` is an `int`. `"fault": true` or `"next_within": true` must be refused by the component, and `"air": false` by both sides. Pinned in Task 3 by `test_bool_is_not_a_number` and in Task 2 by `test_malformed_report_raises_command_error_only`.
5. **Stale readings after the iHeater drops.** When the UART link goes from up to no-reply, the next report must carry null readings within 1 s, not at the heartbeat. Klipper must then fall back to `sensor:` while `linked` turns false. Pinned in Task 1 by `test_report_link_drop_sends_nulls` and in Task 2 by `test_link_not_up_falls_back_to_sensor`.

---

### Task 1: Report scheduler policy

**Files:**
- Create: `lib/remote-policy/src/ReportPolicy.h`
- Create: `lib/remote-policy/src/ReportPolicy.cpp`
- Modify: `lib/remote-policy/src/UartPollPolicy.h` and `UartPollPolicy.cpp` (add the link name helper)
- Modify: `lib/remote-policy/src/UartRegisters.h` and `UartRegisters.cpp` (add the state name helper)
- Test: `test/test_remote_policy/test_main.cpp` (add tests and `RUN_TEST` lines)

**Interfaces:**
- Consumes: `LinkStatus`, `IheaterState`, `kNoReading`, `faultName()` and `intervalElapsed()`, which already exist.
- Produces, in namespace `iheaterlink::policy`:
  ```cpp
  constexpr uint32_t kReportMinGapMs = 1000;
  constexpr uint32_t kReportActiveMs = 5000;
  constexpr uint32_t kReportIdleMs = 15000;

  const char *linkStatusName(LinkStatus s);      // "waiting","up","restarting","no-reply"
  const char *iheaterStateName(IheaterState s);  // "booting","idle","heating","hold","fault", else "unknown"

  enum class PluginState : uint8_t { Unknown, Found, Missing };
  const char *pluginStateName(PluginState p);    // "unknown","found","missing"

  struct ReportInputs {
    LinkStatus link;
    bool hasSnapshot;
    IheaterState state;
    int16_t airT10, heaterT10;   // may be kNoReading
    uint16_t pwmPermille;
    bool fanOn;
    uint8_t fault;
    uint16_t targetT10;          // the target the Remote follows; 0 = off
  };
  struct ReportValues {
    bool hasAir, hasElement, hasPower;
    int16_t airT10, elementT10;
    uint16_t powerPermille;
    uint32_t nextWithinS;        // 5 or 15
  };
  ReportValues reportValues(const ReportInputs &in);
  uint32_t reportIntervalMs(const ReportInputs &in);  // kReportActiveMs or kReportIdleMs

  class ReportScheduler {
   public:
    bool due(const ReportInputs &in, uint32_t nowMs) const;
    void onSent(const ReportInputs &in, uint32_t nowMs);  // sent++, in flight
    void onAccepted();        // plugin Found, not in flight
    void onRefused();         // refused++, not in flight
    void onMethodNotFound();  // plugin Missing, not in flight
    void onFailed();          // failed++, not in flight
    void forceNext();         // the next due() ignores change and heartbeat, but not the 1 s gap, in flight or Missing
    void reset();             // new connection: forget last sent, in flight and Missing; keep the counters
    PluginState plugin() const;
    bool inFlight() const;
    uint32_t sent() const, refused() const, failed() const;
  };
  ```

**Rules the tests pin:**
- Active means `targetT10 > 0`, or `hasSnapshot` with state Heating or Hold.
- `due()` returns false while in flight or while Missing.
- `due()` returns true when:
  - nothing has been sent yet;
  - `forceNext()` was called and at least 1 s has passed;
  - the inputs changed meaningfully and at least 1 s has passed;
  - or the interval promised in the last report (`nextWithinS` of the last sent) has passed.
- Comparisons go through `reportValues()`, so a reading that turns null counts as a change.
- When `hasSnapshot` is false, state, fan and fault don't count as changes.
- All time checks use `intervalElapsed()`, so they survive `millis()` wrap.

- [ ] **Step 1: Write the failing tests**

  Helper `ReportInputs upIdle()` returns:
  - link Up, `hasSnapshot` true, state Idle;
  - air 215, heater 230, pwm 0, fan false, fault 0, target 0.

  Tests, each asserting exactly what its name says:
  - `test_link_and_state_names`:
    - `linkStatusName(NoReply)` is `"no-reply"`.
    - `iheaterStateName(Hold)` is `"hold"`.
    - `iheaterStateName(static_cast<IheaterState>(9))` is `"unknown"`.
    - `pluginStateName(Missing)` is `"missing"`.
  - `test_report_values_null_when_link_not_up`: Restarting gives `hasAir`, `hasElement` and `hasPower` all false.
  - `test_report_values_null_on_no_reading`: `airT10 = kNoReading` gives `hasAir` false and `hasElement` true.
  - `test_report_values_null_without_snapshot`: all three are false.
  - `test_report_interval_idle_and_active`:
    - idle gives 15000 and `nextWithinS` 15;
    - target 600 gives 5000;
    - state Heating with target 0 gives 5000;
    - Hold gives 5000.
  - `test_report_first_send_due`: a fresh scheduler is due at 0.
  - `test_report_not_due_while_in_flight`: after `onSent(in, 0)` it is not due at 20000. After `onAccepted()` it is due at 20000.
  - `test_report_change_waits_min_gap`:
    - sent at 1000; air 215 becomes 216;
    - not due at 1999, due at 2000.
  - `test_report_power_change_by_percent`:
    - pwm 0 to 9 (still 0 %) is not due at 5000;
    - pwm 0 to 10 is due at 5000.
  - `test_report_heartbeat_idle_15s`: no change; not due at 14999 after sending at 0, due at 15000.
  - `test_report_heartbeat_active_5s`: target 600; due at 5000 after sending at 0.
  - `test_report_target_change_is_change`: target 0 to 600 is due at 1000.
  - `test_report_link_drop_sends_nulls`:
    - sent up at 0; link becomes NoReply;
    - due at 1000;
    - `reportValues()` has `hasAir` false.
  - `test_report_method_not_found_stops_until_reset`:
    - after `onSent`, `onMethodNotFound()` gives `plugin()` Missing and not due at 60000;
    - after `reset()`, due at 60000 and `plugin()` Unknown.
  - `test_report_force_next_sends_without_change`:
    - sent at 0, accepted; `forceNext()`;
    - not due at 999, due at 1000;
    - after `onSent` and `onAccepted`, with no change, not due at 2000.
  - `test_report_counters`:
    - one each of accepted, refused and failed gives `sent()` 3, `refused()` 1 and `failed()` 1;
    - `reset()` keeps them.
  - `test_report_wraparound`: sent at `0xFFFFFF00`, idle; due at `0xFFFFFF00 + 15000` (wrapped).

- [ ] **Step 2: Run the tests to check they fail**

  Run: `export PATH="$HOME/scoop/apps/mingw/current/bin:$PATH"; pio test -e native > "$TMP/native.log" 2>&1; tail -30 "$TMP/native.log"`

  Expected: the build fails, because `ReportPolicy.h` does not exist.

- [ ] **Step 3: Implement the names, `reportValues`, `reportIntervalMs` and `ReportScheduler`**

  The scheduler keeps the `ReportValues` it last sent, plus that report's link, state, fan, fault, power percent and target. `due()` compares against those.

- [ ] **Step 4: Run the tests to check they pass**

  Run the same command. Expected: every test passes; 47 old plus 17 new.

- [ ] **Step 5: Replace the local state array with the new helpers, build, and commit**

  In `fillIheaterJson`, replace the local `kStates` array and the link `switch` with `iheaterStateName()` and `linkStatusName()`. The status JSON must not change.

  Run `pio run -e esp32c3-super-mini-remote` and expect SUCCESS. Then commit:

  ```bash
  git add lib/remote-policy test/test_remote_policy src/standalone/StandaloneApp.cpp
  git commit -m "feat(policy): report scheduler for the two-way Klipper link"
  ```

### Task 2: `heater_virtual` takes reports, and the `iheater_remote` sensor type

**Files:**
- Modify: `klipper/heater_virtual.py`
- Modify: `klipper/test_heater_virtual.py`
- Modify: `klipper/README.md`

**Interfaces:**
- Consumes: Klipper's `printer.lookup_object('webhooks').register_endpoint(path, callback)`. A duplicate path raises, so register once.
- The callback receives `web_request`, which has:
  - `get_args()`, which returns the dict;
  - `.error`, a `command_error` subclass;
  - `send(dict)`.
- From `pheaters`, it uses `add_sensor_factory(name, factory)`. It also uses `printer.add_object(name, obj)` and `printer.get_reactor().monotonic()`.
- Produces:
  - Klippy endpoint `heater_virtual/report`, which replies `{'ok': True}` or raises `web_request.error`.
  - `HeaterVirtual.apply_report(report: dict, now: float) -> None`, which raises `ValueError` on a bad field.
  - `HeaterVirtual.get_reading(kind: str) -> float | None`, where `kind` is `'air'` or `'element'`. It returns the last non-null value received.
  - Sensor type `iheater_remote`, with options `heater:` (required, the short name) and `reading:` (`element` by default, or `air`).

**Decided:**
- **Router:**
  - A module-level `_ReportRouter` is looked up as printer object `heater_virtual_router`.
  - The first `HeaterVirtual` creates it, adds it, and registers the endpoint. Each heater calls `router.add(self)`.
  - The handler matches the token case-insensitively after stripping hyphens.
  - With no match it raises `web_request.error("no heater_virtual has remote_token XXXX-XXXX")`.
  - It turns any `ValueError` from `apply_report` into `web_request.error(str(e))`.
  - It catches nothing else, so nothing else must be able to raise. Validate before you store.
- **`apply_report` validation:**
  - `link` is one of the four names.
  - `air`, `element` and `power` are `None`, or an `int` or `float` that is not a `bool`.
  - `fan` is a `bool`.
  - `state` and `fault_name` are `str`.
  - `fault` is an `int` and not a `bool`.
  - `next_within` is a number with 0 < n ≤ 60.
  - Unknown keys are ignored.
- **`sensor:`** is optional. With it, a missing object at connect is still a config error, as today.
- **`get_temp`** returns:
  - the report's air while fresh and not null;
  - else the `sensor:` reading if configured;
  - else the last non-null air received;
  - else 0.
- **`get_status` power:** the report's `power` while linked and not null. Otherwise 1 when a target is set, else 0.
- **`get_status`** adds:
  - `linked` (bool)
  - `iheater_state` (str, `''` before any report)
  - `fault` (int, 0)
  - `fault_name` (str, `''`)
  - `fan` (bool, False)
- **Sensor:**
  - `pheaters.add_sensor_factory('iheater_remote', IheaterRemoteSensor)` runs once, skipped if already present.
  - The sensor has `setup_minmax` (stored, not enforced), `setup_callback` and `get_report_time_delta()` (returns 1.0).
  - A reactor timer runs every 1.0 s and calls `callback(eventtime, value or 0.)`.
  - At `klippy:connect` it looks up `printer.lookup_object('heater_virtual ' + heater, None)`. A missing heater, or one that is not a `HeaterVirtual`, is a config error.
- **Fakes:**
  - `FakeReactor.monotonic()` returns a settable `self.now`.
  - Add `FakePrinter.add_object`, `FakeHeaters.add_sensor_factory` with `sensor_factories`, and `FakeWebhooks` (`register_endpoint`, raising on a duplicate).
  - Add `FakeWebRequest(args)` with `error = CommandError`, `get_args()` and `send()` that records.

- [ ] **Step 1: Write the failing tests**

  Helper `report(**over)` returns:
  ```python
  {'remote_token': '7K3P9QXM', 'link': 'up', 'air': 41.5, 'element': 88.0,
   'power': 0.42, 'fan': True, 'state': 'heating', 'fault': 0,
   'fault_name': 'none', 'next_within': 5}
  ```
  with any overrides applied.

  Tests (in `HeaterVirtualTest` unless stated):
  - `test_report_endpoint_registered_once`: two heaters with different tokens give one webhooks registration for `heater_virtual/report`.
  - `test_report_routes_by_token`: two heaters; a report with the second token changes only the second. `send` received `{'ok': True}`.
  - `test_report_token_hyphen_and_case`: `'7k3p-9qxm'` matches.
  - `test_report_unknown_token_is_error`: raises `CommandError`, and the message contains `7K3P-9QXX`.
  - `test_report_sets_temperature_power_linked`:
    - at now 100: temperature 41.5, power 0.42, `linked` True;
    - `iheater_state` `'heating'`, `fan` True.
  - `test_report_goes_stale_after_2x_next_within_plus_2`:
    - arrival 100 with `next_within` 5;
    - at 112.0, `linked` True;
    - at 112.1, `linked` False and the temperature falls back to the sensor (21.5).
  - `test_link_not_up_falls_back_to_sensor`: a report with link `'no-reply'` and null readings gives `linked` False, temperature 21.5, and power by the old rule.
  - `test_no_sensor_keeps_last_air`:
    - no `sensor:` option, so connect succeeds;
    - report air 41.5, then let it go stale; the temperature stays 41.5;
    - before any report it is 0.
  - `test_malformed_report_raises_command_error_only`: each of these raises `CommandError`, and no other exception type, with state unchanged:
    - `air='hot'`, `air=False`;
    - `fault=None`, `fault=True`;
    - `fan=1`, `link='sideways'`;
    - `next_within=0`, `next_within=61`, `next_within=True`;
    - a missing `state`.
  - `test_unknown_report_field_ignored`: an extra `'humidity': 40` is accepted.
  - `test_set_temp_accepted_while_unlinked`: no report; `set_temp(60.)` sets the target.
  - `test_iheater_remote_sensor_reports_element`:
    - build an `IheaterRemoteSensor` through the registered factory, with a `FakeConfig` for `temperature_sensor chamber_element` with `heater: chamber`;
    - after a report and one timer tick, the callback got 88.0.
  - `test_iheater_remote_sensor_reading_air`: with `reading: air` the callback gets 41.5.
  - `test_iheater_remote_sensor_bad_heater_is_config_error`: `heater: nope` raises `ConfigError` at connect.
  - `test_iheater_remote_sensor_bad_reading_is_config_error`: `reading: aux` raises `ConfigError`.

  Delete `test_missing_sensor_option_is_config_error`: the spec makes `sensor:` optional.

- [ ] **Step 2: Run the tests to check they fail**

  Run: `python -m unittest discover -s klipper -p "test_*.py"`

  Expected: the new tests fail with `AttributeError` or `KeyError`. The old tests pass.

- [ ] **Step 3: Implement the router, `apply_report`, `get_reading`, the `get_temp`/`get_status` changes and `IheaterRemoteSensor` in `klipper/heater_virtual.py`**

  Update the header comment to describe the two-way link.

- [ ] **Step 4: Run the tests to check they pass**

  Run the same command. Expected: OK, with no test skipped.

- [ ] **Step 5: Update the README, then commit**

  In `klipper/README.md`:
  - The intro gains the two-way behaviour.
  - Install gains a Moonraker step (a link to `moonraker/README.md`).
  - Options documents that `sensor:` is optional.
  - A new section, "Element temperature sensor", has the config block from the spec and the note that `[heater_virtual]` must come before the `[temperature_sensor]` section.
  - A new section, "Checking the link in `START_PRINT`", has the spec's snippet.
  - `Commands` notes that `M191` waits on the air reading while linked.

  ```bash
  git add klipper
  git commit -m "feat(klipper): heater_virtual takes iHeater reports, adds iheater_remote sensor"
  ```

### Task 3: Moonraker component `iheater_remote`

**Files:**
- Create: `moonraker/iheater_remote.py`
- Create: `moonraker/test_iheater_remote.py`
- Create: `moonraker/README.md`
- Modify: `AGENTS.md` (gate commands and layout)

**Interfaces:**
- **Consumes:**
  - `config.get_server()`
  - `server.register_endpoint("/server/iheater_remote/report", RequestType.POST, handler)`, imported from `..common`. With a single request type, the JSON-RPC method is `server.iheater_remote.report`.
  - `server.lookup_component("klippy_connection")`, which has `is_connected()` and `async request(WebRequest) -> Any`.
  - `WebRequest(endpoint, args)` from `..common`.
  - `server.error(message, code)`.
  - In the handler, `web_request.get_args()`.
- **Produces:**
  - `load_component(config) -> IheaterRemote`.
  - `IheaterRemote._handle_report(web_request)`, which returns Klippy's result (`{"ok": true}`).
  - `validate_report(args: dict) -> None`, which raises `ValueError`, using the same rules as Task 2's `apply_report` plus `remote_token` matching `^[0-9A-HJKMNP-TV-Z]{8}$`.

**Decided:**
- **Errors:**
  - A `ValueError` becomes `server.error(str(e), 400)`.
  - When `klippy_connection.is_connected()` is false, the handler raises `server.error("Klippy Host not connected", 503)` without calling `request`.
  - Klippy's error passes through unchanged.
- **Forwarding:** the report is forwarded unchanged, unknown fields included.
- **Tests:**
  - The tests load the component under a fake package: put the modules `mr`, `mr.common` (with `RequestType`, `WebRequest`) and `mr.components` in `sys.modules`.
  - Then load `moonraker/iheater_remote.py` as `mr.components.iheater_remote` with `importlib.util.spec_from_file_location`, so `from ..common import` resolves.
  - Run coroutines with `asyncio.run`.

- [ ] **Step 1: Write the failing tests**

  Tests:
  - `test_registers_report_endpoint`: the path, `RequestType.POST`, and the handler.
  - `test_forwards_report_to_klippy`: the fake connection receives a `WebRequest` with endpoint `heater_virtual/report` and the same args. The handler returns `{'ok': True}`.
  - `test_unknown_field_forwarded`: `'humidity': 40` reaches Klippy.
  - `test_bad_token_rejected`: `'7K3P9QXI'` gives a server error with code 400, and nothing is forwarded.
  - `test_bool_is_not_a_number`: `fault=True`, `next_within=True` and `air=False` each give 400.
  - `test_null_readings_accepted`: `air`, `element` and `power` set to `None` are forwarded.
  - `test_klippy_not_connected`: a 503 error, and `request` is never called.
  - `test_klippy_error_passes_through`: the fake `request` raises `server.error("no heater_virtual has remote_token 7K3P-9QXM", 400)`, and the same error propagates.

- [ ] **Step 2: Run the tests to check they fail**

  Run: `python -m unittest discover -s moonraker -p "test_*.py"`

  Expected: errors, because the file is missing.

- [ ] **Step 3: Implement `moonraker/iheater_remote.py`**

  Give it a GPLv3 header comment like Klipper's.

- [ ] **Step 4: Run the tests to check they pass**

  Run the same command, plus the Klipper suite. Expected: both OK.

- [ ] **Step 5: Write the docs, check the API, and commit**

  - `moonraker/README.md`:
    - install: `ln -s ~/iHeater-Remote/moonraker/iheater_remote.py ~/moonraker/moonraker/components/`, add `[iheater_remote]` to `moonraker.conf`, then restart Moonraker;
    - what it does;
    - how to tell it works, by the Remote's diagnostics showing "Klipper plugin: found".
  - `AGENTS.md`: add the fourth gate command, and the layout line `- moonraker/: the iheater_remote Moonraker component and its unit tests.`
  - Check `register_endpoint`, `klippy_connection.request` and `WebRequest`'s signature against current Moonraker master. Note the commit you checked against in the README.

  ```bash
  git add moonraker AGENTS.md
  git commit -m "feat(moonraker): iheater_remote component forwards Remote reports to Klipper"
  ```

### Task 4: Fake Moonraker accepts reports

**Files:**
- Modify: `tools/fake_moonraker/fake_moonraker.py`
- Modify: `tools/fake_moonraker/fake_moonraker.README.md`

**Interfaces:**
- Produces:
  - The method `server.iheater_remote.report`. It logs one line per report:
    ```
    report link=up air=41.5 element=88.0 power=0.42 state=heating next=5
    ```
  - It replies `{"ok": true}`.
  - The stdin commands `plugin off`, which answers `{"code": -32601, "message": "Method not found"}`, and `plugin on`.
  - While `klippy_state == "startup"` it answers `{"code": 503, "message": "Klippy Host not connected"}`.

- [ ] **Step 1: Add the method and the commands**

  Add the method branch, the `plugin on|off` command and its usage text.

- [ ] **Step 2: Smoke-test it locally**

  Start `python tools/fake_moonraker/fake_moonraker.py --token 7K3P9QXM`. Send one report from a short `websockets` client script in the scratchpad, then repeat with `plugin off`.

  Expected: the log line, then a `-32601` reply.

- [ ] **Step 3: Update the README, then commit**

  In the README:
  - "What it does" gains the report method.
  - The bench checklist gains three checks:
    1. Reports arrive about every 15 s when idle, and every 5 s with `t 60`.
    2. `plugin off` then `drop` shows the plugin-missing note on the dashboard.
    3. `plugin on` then `drop` makes reports resume.

  ```bash
  git add tools/fake_moonraker
  git commit -m "feat(fake-moonraker): accept iheater_remote reports, plugin off switch"
  ```

### Task 5: Firmware sends reports

**Files:**
- Modify: `src/standalone/MoonrakerLink.h` and `src/standalone/MoonrakerLink.cpp`
- Modify: `src/standalone/StandaloneApp.cpp`
- Modify: `src/standalone/RemoteDashboard.h`

**Interfaces:**
- Consumes: Task 1's `ReportInputs`, `ReportScheduler`, `reportValues`, `linkStatusName`, `iheaterStateName`, `pluginStateName` and `faultName`.
- Produces:
  - `void MoonrakerLink::report(const policy::ReportInputs &in)`. Call it every main-loop pass, in UART mode with the Klipper source. It sends only when `subscribed_` and `scheduler_.due(in, millis())`.
  - `const policy::ReportScheduler &MoonrakerLink::reports() const`.

**Decided:**
- **`MoonrakerLink` changes:**
  - Add `Pending::Report`.
  - `handleError(Pending kind, int code, const char *message)`, where `code` is `root["error"]["code"] | 0`. For Report:
    - `-32601` calls `onMethodNotFound()` and logs once: `[MOON] Klipper plugin not installed: reports stopped until reconnect`;
    - any other error calls `onRefused()` and logs the message, at most once per 60 s;
    - a Report error never changes `subscribed_`, `klippy_` or `phase_`.
  - A Report result calls `onAccepted()`.
  - A Report slot timeout calls `onFailed()`, before the existing reconnect.
  - After `call()`, a non-zero id calls `onSent(in, now)`. A Report call that returns 0 counts as sent and then failed: call `onSent()`, then `onFailed()`.
  - `resetSession()` calls `scheduler_.reset()`.
  - The point where `subscribed_` becomes true calls `scheduler_.forceNext()`.
- **The report frame:**
  - Its params come from a `StaticJsonDocument<384>`.
  - Fields: `remote_token` is `token_`; air and element are `T10 / 10.0f`; power is `permille / 1000.0f`; `next_within` is `nextWithinS`. Each null is a JSON `null`.
  - With no snapshot: `state` is `"unknown"`, `fan` false, `fault` 0, `fault_name` `faultName(0)`.
- **`StandaloneApp`:**
  - A new `updateKlipperReport()` in `loop()` runs right after `s_moonraker.loop()`.
  - It builds `ReportInputs` from `s_uart.view()`. For `targetT10`, it uses `round(targetTempC * 10)` when the last output command is TargetTemperature, else 0.
  - It returns early unless `s_outputKind == OutputKind::Uart && klipperSource()`.
- **Status JSON:** `fillIheaterJson`'s `diag` gains `reportsSent`, `reportsRefused`, `reportsFailed` and `plugin` (from `pluginStateName`). Check `doc.overflowed()` once on the bench; if it overflows, raise the 2048 bytes.
- **Dashboard:**
  - `srcWarn` adds the line `Klipper plugin not installed: readings are not sent to Klipper` when Klipper is the source, Moonraker is `ready`, and `iheater.diag.plugin === "missing"`.
  - The `ihDiag` list gains `["Klipper plugin", d.plugin||"—"]` and `["Reports sent / refused / failed", ...]`. The second shows the three counts as `a / b / c`.

- [ ] **Step 1: Make the `MoonrakerLink` and `StandaloneApp` changes, then build**

  Run: `pio run -e esp32c3-super-mini-remote > "$TMP/build.log" 2>&1; tail -15 "$TMP/build.log"`

  Expected: SUCCESS. Record the RAM and flash figures for the report.

- [ ] **Step 2: Check the dashboard against mocked status**

  Use the scratchpad preview harness: mock status with `iheater.diag.plugin` set to `"missing"` and with `"found"`. Render and screenshot both.

  Expected: the note appears only in the first, and the diagnostics list shows the counts.

- [ ] **Step 3: Run all four gates**

  Expected: build SUCCESS, native tests pass, and both Python suites OK.

- [ ] **Step 4: Commit**

  ```bash
  git add src/standalone
  git commit -m "feat(remote): report iHeater readings to Klipper over Moonraker"
  ```

- [ ] **Step 5: Bench check, for the user**

  Ask the user to flash; do not flash. Then, against the fake (Task 4 checklist):
  1. Reports arrive every 15 s when idle and every 5 s at `t 60`.
  2. A UART unplug gives a report with null readings within about 1 s.
  3. `plugin off` + `drop` shows the note, and stops reports after the first refusal.
  4. `restart` gives a report straight after the new subscription.

  Then on hestia, with real Moonraker and Klipper:
  1. Check that `printer["heater_virtual chamber"].linked` is true.
  2. Mainsail shows the air temperature and the real power.
  3. The `chamber_element` sensor graphs.

## Self-review notes

- **Spec coverage:** each spec section maps to a task:
  - report fields and nulls: Task 1 and Task 5;
  - send schedule: Task 1;
  - plugin missing: Tasks 1, 4 and 5;
  - the component: Task 3;
  - the Klipper module: Task 2;
  - fallback: Task 2;
  - sensor type: Task 2;
  - the `START_PRINT` snippet: Task 2's README;
  - dashboard diagnostics: Task 5;
  - the bench: Tasks 4 and 5.
- **Calls this plan made beyond the spec:**
  - Reports resend at once after each new subscription (`forceNext`). This covers Review Focus 3.
  - With no snapshot, the state is sent as `"unknown"`.
  - Unknown fields are ignored on both sides.
  - `next_within` is limited to (0, 60].
  - A refused-report log line appears at most once per 60 s.
