# Local-only cleanup and own Moonraker client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drop idryer-core and everything upstream. The Remote follows one
token-paired `heater_virtual` through its own Moonraker WebSocket client
(optionally TLS), with settings migrated from idryer-core.

**Architecture:** Pure decisions go in `lib/remote-policy` (Unity, build-only).
The client is `src/standalone/MoonrakerLink.{h,cpp}`, built on
`links2004/WebSockets`. Settings and migration live in
`src/standalone/RemoteSettings.{h,cpp}`. `StandaloneApp.cpp` wires them in.
The Klipper module drops the macro mirror and gains `remote_token`.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.0.17, links2004/WebSockets 2.7.3,
ArduinoJson 6, Unity, Python 3 unittest.

**Spec:** `docs/design/2026-11-19-local-only-cleanup-design.md`

## Global Constraints

**Repo rules**
- Work on branch `feature/local-only-cleanup`.
- Use Canadian English.
- Never modify `c:\git\github\idryer-core`.
- Never flash the device without asking the user.

**Removing the idryer-core link**
- `lib/idryer-core` is a Windows junction **and** a gitlink.
- Remove it only with `cmd //c rmdir lib\\idryer-core` plus
  `git rm --cached lib/idryer-core`.
- **Never `rm -rf` it**: that deletes the real idryer-core checkout.

**NVS (`iheater-remote` namespace)**

| Key | Type | Values |
|-----|------|--------|
| `source` | UChar | 0 = manual, 1 = klipper |
| `moon-host` | String | |
| `moon-port` | UShort | |
| `moon-tls` | Bool | |
| `moon-key` | String | |
| `pair-token` | String | normalized, 8 chars |

- Keys kept as they are: `ssid`, `pass`, `min-target`, `max-target`,
  `tx-power`.
- Keys removed: `auto-target`, `auto-bed`.

**Legacy namespaces to read, then erase**
- `li_common`: `active`, UChar; 0 = none, 1 = HA, 2 = Bambu, 3 = Moonraker.
- `li_moon`: `host` (String), `port` (UShort, default 7125), `ssl` (Bool),
  `apiKey` (String).
- `li_bambu` and `li_ha` are erased too.

**Pairing token**
- Crockford base32 alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ`, 8 characters.
- Shown as `XXXX-XXXX`.
- Input is case-insensitive, and hyphens are ignored anywhere.

**Moonraker**
- Default port 7125, or 443 with TLS. The path is `/websocket`.
- Identify as client `iheater-remote`, type `other`, url
  `https://github.com/Vylyne/iHeater-Remote`.

**Timings**

| What | Value |
|------|-------|
| Ping interval | 10000 ms |
| Pong timeout | 5000 ms, 2 missed means disconnect |
| Query interval | 30000 ms |
| Request timeout | 5000 ms |
| Reconnect backoff | 2000 ms, doubling, capped at 30000 ms |
| Link-loss grace | 600000 ms (unchanged) |

**Status JSON `moonrakerState` values:** `disconnected`, `connecting`,
`klippy-not-ready`, `ready`.

**Gates**
- Build: `pio run -e esp32c3-super-mini-remote`.
- Policy tests (build only):
  `pio test -e esp32c3-super-mini-remote -f test_remote_policy --without-uploading --without-testing`.
- Klipper tests: `python -m unittest discover -s klipper -p "test_*.py"`.

**Commits** end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Klipper restart while heating.** `notify_klippy_disconnected` then
   `notify_klippy_ready`. The Remote holds, rediscovers and resubscribes,
   and the new target (0) applies within seconds, not after 10 min.
   Pinned by the fake Moonraker `restart` scenario (Task 7).
2. **Half-open socket** (router reboot, no TCP FIN). Missed pongs force a
   reconnect, and the 10 min link-loss rule still cuts heat if it never
   recovers. Pinned by `moonrakerLinkHealthy` tests (Task 1) and the
   fake's `freeze` command (Task 7).
3. **Not paired after the update** (no `remote_token` in `printer.cfg`).
   The Remote never heats from Klipper, the dashboard shows the "not
   paired" text with the token, and manual heat still works. Pinned by
   `matchPairedHeater` tests (Task 1) and the fake's `--token` option
   (Task 7).
4. **TLS failure** (bad certificate, wrong host, port 443 without TLS).
   - Backoff reaches the 30 s cap.
   - Free heap is stable across many failed attempts.
   - `moonrakerError` says what failed.
   Pinned by `nextBackoffMs` tests (Task 1) and the Task 5 heap log line.
5. **Noisy or large frames.**
   - `notify_proc_stat_update` arrives about 1 Hz and must be skipped
     before parsing.
   - A large `printer.objects.list` reply.
   - Malformed JSON is ignored, never a crash.
   Pinned by the fake sending proc-stat noise and a 150-object list
   (Task 7).
6. **Migration.** An old device with `li_moon` host set ends up with
   source Klipper and the same host, port, TLS flag and key. The old
   namespaces are gone, and a second boot does not migrate again. This
   holds even if the old firmware saved `active=0` during a web OTA.
   Pinned by the Task 4 serial log check.

---

### Task 1: Policy for pairing, discovery, health and backoff

**Files:**
- Modify: `lib/remote-policy/src/RemotePolicy.h`, `lib/remote-policy/src/RemotePolicy.cpp`
- Test: `test/test_remote_policy/test_main.cpp`

**Interfaces** (namespace `iheaterlink::policy`, all new; nothing removed
in this task):
- `constexpr size_t kPairTokenLen = 8;`
- `bool normalizePairToken(const char *in, char out[kPairTokenLen + 1]);`
  - Upper-cases the input and drops `-`.
  - Returns false for null, the wrong length, or any character outside
    the alphabet.
- `void pairTokenFromBits(uint64_t bits, char out[kPairTokenLen + 1]);`
  - Uses the low 40 bits, 5 per character, most significant first.
- `void formatPairToken(const char *normalized, char out[kPairTokenLen + 2]);`
  - Produces `XXXX-XXXX`.
- `bool isHeaterVirtualObject(const char *name);`
  - True when the name starts with `heater_virtual ` (with the trailing
    space) and has a name after it.
- `constexpr int kHeaterNone = -1; constexpr int kHeaterDuplicate = -2;`
- `int matchPairedHeater(const char *const *tokens, size_t count, const char *myToken);`
  - Normalizes both sides and skips null or invalid tokens.
  - Returns the index of the single match, `kHeaterNone` (also when
    `myToken` is invalid), or `kHeaterDuplicate`.
- `enum class KlippyState : uint8_t { Unknown, Ready, NotReady };`
- `bool moonrakerLinkHealthy(bool socketUp, bool subscribed, KlippyState klippy);`
- `constexpr uint32_t kBackoffInitialMs = 2000, kBackoffMaxMs = 30000;`
- `uint32_t nextBackoffMs(uint32_t currentMs);`
  - 0 gives the initial value, then it doubles up to the cap.
- `bool intervalElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs);`
  - Wrap-safe; this replaces `moonrakerStale`.
- `uint16_t defaultMoonrakerPort(bool tls);` returns 443 or 7125.
- Timing constants: `kPingIntervalMs = 10000`, `kPongTimeoutMs = 5000`,
  `kMissedPongs = 2`, `kQueryIntervalMs = 30000`, `kRequestTimeoutMs = 5000`.

- [ ] **Step 1: Write the failing tests** in `test_main.cpp` and add each to `setup()` with `RUN_TEST`:
  - `test_token_normalize`:
    - `"7k3p-9qxm"` → true, `"7K3P9QXM"`.
    - `"--7K3P9QXM"` → true.
    - `"7K3P-9QX"`, `"7K3P9QXMM"`, `"7K3P9QXI"`, `"7K3P9QXU"`,
      `"7K3P 9QXM"`, `""` and `nullptr` → false.
  - `test_token_from_bits`:
    - `0` → `"00000000"`.
    - `0xFFFFFFFFFFull` → `"ZZZZZZZZ"`.
    - `1` → `"00000001"`.
    - `31ull << 35` → `"Z0000000"`.
    - Bits above 40 are ignored: `(1ull << 40) | 1` → `"00000001"`.
  - `test_token_format`: `"7K3P9QXM"` → `"7K3P-9QXM"`.
  - `test_heater_object_name`:
    - `"heater_virtual chamber"` → true.
    - `"heater_virtual"`, `"heater_virtual "`, `"heater_generic chamber"`
      and `"heater_virtualx a"` → false.
  - `test_match_paired_heater`:
    - `{"AAAA0000","7K3P9QXM"}` with `"7k3p-9qxm"` → 1.
    - No match → `kHeaterNone`.
    - Two equal tokens → `kHeaterDuplicate`.
    - `{nullptr, "", "7K3P9QXM"}` → 2.
    - An invalid `myToken` → `kHeaterNone`.
  - `test_link_healthy`: only `(true, true, Ready)` is true; each single
    false input, `NotReady` and `Unknown` give false.
  - `test_backoff`: 0→2000, 2000→4000, 16000→30000, 30000→30000.
  - `test_interval_elapsed`:
    - `(1000, 0, 1000)` → true.
    - `(999, 0, 1000)` → false.
    - Wrap: `(500, 0xFFFFFE00, 1000)` → false and `(600, 0xFFFFFE00, 1000)`
      → true.
  - `test_default_port`: `true` → 443, `false` → 7125.

- [ ] **Step 2: Build the tests and watch them fail**

Run: the policy test command.
Expected: compile errors naming the missing functions.

- [ ] **Step 3: Implement the interfaces** in `RemotePolicy.{h,cpp}`.
  - `pairTokenFromBits`: character `i` is
    `alphabet[(bits >> (35 - 5 * i)) & 31]`.

- [ ] **Step 4: Build the tests again**

Run: the policy test command.
Expected: `SUCCESS` for `test_remote_policy`.

- [ ] **Step 5: Commit**: `feat(policy): pairing token, heater match, link health, backoff`.

---

### Task 2: Klipper module, `remote_token` instead of the macro mirror

**Files:**
- Modify: `klipper/heater_virtual.py`, `klipper/iheater_remote.cfg`, `klipper/README.md`
- Test: `klipper/test_heater_virtual.py`

**Interfaces:**
- New config option `remote_token`.
  - It is optional. It is normalized (upper case, `-` removed) and must
    match `^[0-9A-HJKMNP-TV-Z]{8}$`; otherwise it is a `config.error`
    naming the section and the allowed characters.
- `get_status()` adds `'remote_token': <normalized or ''>`.
- A second `heater_virtual` with the same non-empty token is a
  `config.error`. Detect it by scanning `self.pheaters.heaters.values()`
  for `HeaterVirtual` instances.
- Removed: `mirror_macro`, `self.macro`, `_mirror`, `_mirror_timer`, and
  the `klippy:ready` timer. Shutdown still sets the target to 0.

- [ ] **Step 1: Update the tests first.**
  - Delete these tests: `test_set_temp_mirrors_to_macro`,
    `test_missing_macro_on_connect_is_config_error`,
    `test_timer_mirrors_temperature`.
  - Change `test_shutdown_zeroes_and_mirrors` to
    `test_shutdown_zeroes_target`, asserting only `get_status` target 0.
  - Drop `FakeMacro` and its `printer.objects` entry.
  - Add these tests:
    - `test_remote_token_normalized_in_status`: `'7k3p-9qxm'` gives
      `get_status(0.)['remote_token'] == '7K3P9QXM'`.
    - `test_remote_token_absent_is_empty`: `''`.
    - `test_remote_token_bad_format_is_config_error`: subtests with
      `'ABC'`, `'7K3P9QXI'` and `'7K3P9QXMM'`.
    - `test_duplicate_remote_token_is_config_error`: make `chamber` with
      a token, then build a second `FakeConfig(self.printer,
      'heater_virtual chamber2', {...same token})` through
      `load_config_prefix`, which raises `ConfigError`.
    - `test_mirror_macro_not_read`: building with
      `mirror_macro='VIRTUAL_CHAMBER'` leaves `'mirror_macro'` out of
      `config.settings`, so real Klipper rejects it as unknown.

- [ ] **Step 2: Run the tests and watch them fail**

Run: the Klipper test command.
Expected: failures and errors in the new token tests.

- [ ] **Step 3: Implement the changes** in `heater_virtual.py` and update its header comment.
  - The header says the Remote reads the heater object directly and
    pairs by `remote_token`.

- [ ] **Step 4: Run the tests and watch them pass**

Run: the Klipper test command.
Expected: `OK`.

- [ ] **Step 5: Update `iheater_remote.cfg`.**
  - Delete the `[gcode_macro VIRTUAL_CHAMBER]` block.
  - Add a commented `#remote_token: XXXX-XXXX` line with a comment:
    "copy from the Remote's Setup page".
  - Keep M141 and M191.

- [ ] **Step 6: Update `klipper/README.md`.**
  - Options table: remove `mirror_macro` and add `remote_token`.
  - Add an "Updating from the macro version" note: delete `mirror_macro`
    and the `VIRTUAL_CHAMBER` macro, then add `remote_token`. Until you
    do, the Remote shows "not paired" and does not heat from Klipper.

- [ ] **Step 7: Commit**: `feat(klipper): pair heater_virtual by remote_token, drop macro mirror`.

---

### Task 3: Moonraker client

**Files:**
- Create: `src/standalone/MoonrakerLink.h`, `src/standalone/MoonrakerLink.cpp`, `src/standalone/MoonrakerCa.h`

**Interfaces:**
- Consumes everything from Task 1.
- Produces:

```cpp
namespace iheaterlink::standalone {
struct MoonrakerSettings { String host; uint16_t port = 7125; bool tls = false; String apiKey; };
class MoonrakerLink {
 public:
  using TargetCallback = void (*)(float targetC);
  void onTarget(TargetCallback cb);
  void configure(const MoonrakerSettings &s, const char *pairToken); // (re)starts
  void stop();                       // disconnect, no reconnect
  void loop();                       // call every loop() pass
  bool healthy() const;              // policy::moonrakerLinkHealthy(...)
  const char *stateString() const;   // disconnected/connecting/klippy-not-ready/ready
  const String &heaterObject() const;
  const String &lastError() const;
};
}
```

- `MoonrakerCa.h`: `static const char kMoonrakerCaPem[] PROGMEM`.
  - It holds the self-signed ISRG Root X1, ISRG Root X2, ISRG Root YE and
    ISRG Root YR, concatenated PEM, downloaded from
    https://letsencrypt.org/certificates/.
  - A comment notes that another CA needs its root added here.

**Behaviour** (from the spec; the implementer writes the bodies):
- **Connect.**
  - Plain: `begin(host, port, "/websocket")`.
  - TLS: `beginSslWithCA(host, port, "/websocket", kMoonrakerCaPem)`.
  - With an API key, call `setExtraHeaders` with `X-Api-Key: <key>`,
    keeping that string in a member.
  - `enableHeartbeat(kPingIntervalMs, kPongTimeoutMs, kMissedPongs)`.
  - `setReconnectInterval(backoff)`, where the backoff comes from
    `nextBackoffMs` on each disconnect and resets to the initial value on
    connect.
- **Sequence on connect:**
  1. `server.connection.identify` with `client_name`, `version`
     (`VERSION_STR`), `type`, `url`, and `api_key` if one is set.
  2. `printer.objects.list`.
  3. One `printer.objects.query` asking each `heater_virtual ` object for
     `["remote_token"]`, capped at 8 objects.
  4. `matchPairedHeater`.
  5. `printer.objects.subscribe` with `{<heater>: ["target"], webhooks: ["state"]}`.
- **Requests.**
  - A private `uint32_t call(const char *method, JsonVariantConst params, Pending kind)`.
  - Up to 4 pending slots `{id, kind, sentMs}`, matched by `id`.
  - Any slot older than `kRequestTimeoutMs` means: disconnect, set
    `lastError` to `"no reply from Moonraker"`, and let the backoff
    reconnect.
- **Replies:**
  - An identify error sets `lastError` to `"auth failed: <message>"` and
    disconnects.
  - A list or query error (Klipper not ready) sets `klippy = NotReady`
    and waits.
  - No heater objects: `"no heater_virtual in Klipper"`.
  - `kHeaterNone`: `"not paired: add remote_token: <XXXX-XXXX> to your [heater_virtual] section"`.
  - `kHeaterDuplicate`: `"remote_token used by more than one heater"`.
  - A subscribe or query result sets `subscribed = true`, sets `klippy`
    from `webhooks.state` (`"ready"` means Ready, anything else NotReady),
    and calls `onTarget(target)` when the heater's `target` is present.
- **Notifications:**
  - `notify_status_update` with `params[0]`: same handling as a subscribe
    result.
  - `notify_klippy_ready`: restart the discovery sequence from step 2.
  - `notify_klippy_shutdown` and `notify_klippy_disconnected`: set
    `subscribed = false` and `klippy = NotReady`, holding the target.
  - Target updates apply whatever the Klipper state.
- **Pre-parse filter.**
  - If the frame contains `"method"` but none of the four handled
    notification names, skip it unparsed. This covers
    `notify_proc_stat_update` and the rest; Moonraker puts spaces after
    colons, so match on names, not on `"method":"`.
  - Parse with zero-copy (`deserializeJson(doc, (char *)payload, length)`)
    into a `DynamicJsonDocument` of 8192 bytes.
  - On any parse error, log once and ignore the frame.
- **Every 30 s while the socket is up:** if subscribed, send the same
  objects with `printer.objects.query`. If not subscribed and nothing is
  pending, restart discovery from step 2.
- **`lastError`:**
  - A disconnect before any connect sets `"could not connect"`, plus
    `" (TLS: check certificate and host name)"` when TLS is on.
  - It clears to `""` when subscribed with Klipper ready.
- **`stateString`:**
  - `disconnected`: stopped, or the socket is down.
  - `connecting`: the socket is up but identify or discovery is not done.
  - `klippy-not-ready`: subscribed or discovering, and Klipper is not
    ready.
  - `ready`: `healthy()`.

- [ ] **Step 1: Write `MoonrakerCa.h`.**
  - Save the four PEMs to the scratchpad.
  - Check them against the user's server:
    `openssl s_client -connect hestia.brunton.ca:443 -servername hestia.brunton.ca -CAfile <pem> -verify_return_error </dev/null 2>&1 | grep "Verify return code"`.
  - Expected: `Verify return code: 0 (ok)`.

- [ ] **Step 2: Write `MoonrakerLink.{h,cpp}`** to the interface and behaviour above.

- [ ] **Step 3: Build.**

Run: the build command (the new files compile but nothing calls them yet).
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**: `feat(remote): own Moonraker WebSocket client with TLS and token pairing`.

---

### Task 4: Settings and migration

**Files:**
- Create: `src/standalone/RemoteSettings.h`, `src/standalone/RemoteSettings.cpp`

**Interfaces:**
- Consumes `MoonrakerSettings` (Task 3), and `normalizePairToken`,
  `pairTokenFromBits` and `defaultMoonrakerPort` (Task 1).
- Produces:

```cpp
enum class ControlSource : uint8_t { Manual = 0, Klipper = 1 };
struct RemoteSettings { ControlSource source = ControlSource::Manual; MoonrakerSettings moon; String pairToken; };
void loadRemoteSettings(RemoteSettings &out);      // migrate if needed, erase legacy, ensure token
bool saveSourceAndMoonraker(const RemoteSettings &s);
bool savePairToken(const char *normalized);
String generatePairToken();                         // normalized; esp_random() bits
```

- **Migration** runs inside `loadRemoteSettings` when `iheater-remote`
  has no `source` key.
  - Read `li_moon` `host`, `port`, `ssl` and `apiKey`, and `li_common`
    `active`.
  - Set the source to Klipper when the host is non-empty and `active` is
    not 1 or 2. Otherwise Manual.
  - Write the `moon-*` keys, then `source`, and remove `auto-target` and
    `auto-bed`.
  - Log: `[REMOTE] migrated settings source=<s> host=<h> port=<p> tls=<0|1> key=<set|none>`.
- **Legacy erase runs every boot** after migration. For each of
  `li_common`, `li_moon`, `li_bambu` and `li_ha`: if
  `Preferences::begin(ns, true)` succeeds, `end()`, then `begin(ns,
  false)`, `clear()`, `end()`, and log `[REMOTE] erased legacy NVS <ns>`.
  Erasing on every boot means a power cut between writing and erasing is
  harmless.
- **Token:** if `pair-token` is missing or fails `normalizePairToken`,
  generate one, save it and log it formatted.
- **Port:** a stored port of 0 becomes `defaultMoonrakerPort(tls)`.

- [ ] **Step 1: Write `RemoteSettings.{h,cpp}`** to the interface above.

- [ ] **Step 2: Build.**

Run: the build command.
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**: `feat(remote): own settings with one-time idryer-core migration`.

The migration is verified on the device in Task 5, Step 7.

---

### Task 5: Wire the client in and remove idryer-core usage from the app

**Files:**
- Modify: `src/standalone/StandaloneApp.cpp`, `src/standalone/RemoteDashboard.h`
- Modify: `lib/remote-policy/src/RemotePolicy.{h,cpp}`, `test/test_remote_policy/test_main.cpp`

**Interfaces:** consumes Tasks 1, 3 and 4. Produces the status JSON fields
`source` (`manual`/`klipper`), `moonrakerState`, `heaterObject` and
`moonrakerError`. Removed fields: `integration`, `bambuState`,
`bambuFilament`, `bambuPrintState`.

- [ ] **Step 1: Remove the old Moonraker policy.**
  - Delete `MoonrakerInputs`, `resolveMoonrakerTargetC`, `moonrakerStale`,
    `kMoonrakerStaleMs` and `moonrakerStatusIsReset`.
  - Delete their tests: `test_resolve_*`, `test_stale_*` and
    `test_moonraker_reset_status`, plus their `RUN_TEST` lines.

- [ ] **Step 2: Build and watch it fail.**

Run: the build command.
Expected: compile errors in `StandaloneApp.cpp` naming the removed policy
functions. This proves the app still depends on them.

- [ ] **Step 3: Rewrite the app wiring in `StandaloneApp.cpp`.**
  - **Delete:**
    - The `integrations/*` and `hal/hal_arduino.h` includes.
    - `s_integrationStore` and `s_integrations`.
    - The auto target and bed keys and globals.
    - `autoHeatAllowed`, `onMoonrakerVirtualChamber`, `onMoonrakerStatus`,
      `onBambuPrinterStatus` and `updateMoonrakerWatchdog`.
    - The Moonraker silence globals.
    - The whole `#if 0` legacy dashboard block.
    - The `initArduinoHal` and `s_integrations.*` calls in `setup()` and
      `loop()`.
  - **Add** the globals `RemoteSettings s_settings;` and `MoonrakerLink s_moonraker;`.
  - **Add** `onKlipperTarget(float c)`: when the source is Klipper, call
    `applyIntegrationTarget(c, "klipper", "klipper")`.
  - **`applyMode(uint8_t mode, const char *source, bool releasePrinter = true)`**
    - With `releasePrinter` and source Klipper: set the source to Manual,
      call `saveSourceAndMoonraker`, then `s_moonraker.stop()`. This keeps
      today's rule that a manual press unbinds the printer.
    - OTA start passes `false`, so an update no longer unbinds.
  - **`updateLinkSafety`**
    - `printerSource` means the source is Klipper.
    - `healthy` is `!printerSource || s_moonraker.healthy()`.
    - Heating from the printer means mode `TargetTemperature` and
      `s_targetSource == "klipper"`.
  - **Device settings:** rename `loadAutoSettings` to `loadDeviceSettings`
    (range and TX power). Replace `saveAutoSettings` with
    `saveRange(int minC, int maxC)`.
  - **`setup()`** order:
    1. `loadDeviceSettings()`.
    2. `configureWiFi()`.
    3. `loadRemoteSettings(s_settings)`, after Wi-Fi so `esp_random`
       has RF entropy.
    4. `s_moonraker.onTarget(onKlipperTarget)`.
    5. If the source is Klipper, `configure(s_settings.moon, s_settings.pairToken)`.
  - **`loop()`:** call `s_moonraker.loop()` in place of the integration
    loop and the watchdog. Every 60 s, log
    `[REMOTE] heap free=<ESP.getFreeHeap()> min=<ESP.getMinFreeHeap()> moon=<stateString>`.

- [ ] **Step 4: Rewrite the Setup page and form handling.**
  - **Control source card:** a select with `manual` and `moonraker`
    (labelled "Klipper (Moonraker)"), then:
    - Host.
    - Port.
    - Checkbox `tls` value 1, labelled "Use TLS (wss://, e.g. through Mainsail on 443)".
    - API key, write-only.
    - One line of JS swaps the port between 7125 and 443 when TLS is
      toggled and the field holds the other default.
  - **New Pairing card:**
    - Input `pairToken` prefilled with the formatted token.
    - Checkbox `regenToken` value 1, "Generate a new token on save".
    - Note: "Put this in your [heater_virtual] section as remote_token:
      XXXX-XXXX. After a wipe, type your old token here to keep your
      printer.cfg unchanged."
  - Delete the Bambu block and the Auto control card.
  - The intro note becomes: "Blank password and API key fields keep their
    stored values."
  - **`applySetupForm`**
    - Accepts `type` values `manual` / `none` (Manual) and `moonraker` /
      `klipper` (Klipper). Anything else is "Unknown control source."
    - Validates the transmit power (unchanged), the range, and
      `pairToken`. A non-empty token that fails `normalizePairToken`
      gives: "Pairing token must be 8 characters: 0–9 and A–Z without I,
      L, O or U."
    - Klipper with no host and none stored gives: "Klipper needs the
      Moonraker host or IP address."
    - A blank port means `defaultMoonrakerPort(tls)`. A blank API key
      keeps the stored key.
    - `regenToken=1` wins over a typed token.
    - **Saves:** source and Moonraker settings, the token, the range, then
      the transmit power (and restart), in that order.
    - **After saving:** with Klipper selected, call
      `s_moonraker.configure(...)`. With Manual, `stop()` and
      `applyIntegrationTarget(0, "binding disabled", "manual")`.
  - **`statusJson`:** replace the four removed fields with the four new
    ones. `moonrakerError` is `""` when the source is Manual.

- [ ] **Step 5: Update the dashboard in `RemoteDashboard.h`.**
  - Delete the unreachable `data-content="bind"` section and its JS
    (`saveBinding`, `saveBambu`, `saveMoon`, `disableBinding`).
  - In `applyStatus`, the source text is:
    - `"Manual"` for Manual.
    - `"Klipper"` when ready.
    - Otherwise `"Klipper · " + (s.moonrakerError || s.moonrakerState)`.
  - Remove the `bindingFeedback` line.

- [ ] **Step 6: Run the build and policy test gates and watch them pass**

Run: the build command, then the policy test command.
Expected: both `SUCCESS`, and
`git grep -n "idryer::\|integrations/" src` prints nothing. Record the
flash percentage in the ledger, against 84.5 % before.

- [ ] **Step 7: Bench check on the device.** Ask the user before flashing.
  - Flash by OTA or USB, as the user chooses.
  - Read the serial log for:
    - the `migrated settings` and `erased legacy NVS` lines, with the
      user's old host, port and key.
    - the token log line.
    - `moon=klippy-not-ready` or `ready`.
  - The Setup page shows the same host, port and key.
  - Reboot once more: there is no second `migrated` line.
  - If the user declines, record
    `Ruling: device migration check deferred to user`.

- [ ] **Step 8: Commit**: `feat(remote): Klipper-only control through MoonrakerLink; drop Bambu, bed rule, idryer-core use`.

---

### Task 6: Remove upstream code, targets and assets

**Files:**
- Delete:
  - `src/heater/`, `src/menu/`, `include/wifi_tx_power.h`.
  - `lib/idryer-bambu/`, `lib/idryer-core` (gitlink and junction).
  - `BAMBU_MQTT_FIELDS.md`, `img/`, `imgweb/`.
  - `docs/{cs,de,en,es,fr,ja,pt,pt-BR,ru,zh,zh-Hant}/`.
  - `tools/fake_bambu/`, `tools/mock_portal.py`, `extra_scripts/`.
  - `.env.example`, `package.json`, `package-lock.json`.
- Modify: `src/main.cpp`, `src/version.h`, `platformio.ini`, `.github/workflows/release.yml`, `.gitignore`

- [ ] **Step 1: Delete the files.**
  - Run `git rm -r -q` on all tracked paths above except `lib/idryer-core`.
  - Then run `git rm --cached -q lib/idryer-core && cmd //c rmdir lib\\idryer-core`.
  - Check `ls c:/git/github/idryer-core/src` still lists files: the
    junction target must survive.

- [ ] **Step 2: Replace `src/main.cpp`** with only the forwarder: include
  `standalone/StandaloneApp.h`, plus `setup()` and `loop()` calling
  `iheaterlink::standalone::setup()` and `loop()`.
  - `Ruling: src/main.cpp kept as a 3-line forwarder rather than deleted — Arduino needs global setup/loop — none`.

- [ ] **Step 3: Clean up `src/version.h`.** Drop the `__has_include("../lib/idryer-menu/...")`
  block and the Russian comment, and keep the version macros as they are.

- [ ] **Step 4: Rewrite `platformio.ini`** to one env:
  - `[platformio] default_envs = esp32c3-super-mini-remote`.
  - `[env:esp32c3-super-mini-remote]` with:
    - `platform = espressif32`, `board = esp32-c3-devkitm-1`,
      `framework = arduino`.
    - `monitor_speed = 115200`, `monitor_raw = yes`,
      `upload_speed = 115200`, `board_build.filesystem = littlefs`.
    - `lib_deps`: `bblanchon/ArduinoJson @ ^6.21.3` and
      `links2004/WebSockets @ ^2.4.0`.
    - `build_flags`: `-DDEBUG_SERIAL=Serial`,
      `-DIHEATER_TRIGGER_OUTPUT_PIN=3`, `-DPIO_ENV=\"$PIOENV\"`,
      `-DARDUINO_USB_MODE=1`, `-DARDUINO_USB_CDC_ON_BOOT=1`,
      `-DIHEATER_WIFI_TX_POWER=WIFI_POWER_8_5dBm`,
      `-DIHEATER_REMOTE_LOCAL=1`, `-DCORE_DEBUG_LEVEL=3`,
      `-DIHEATER_BOOT_BUTTON_PIN=9`, `-DIHEATER_STATUS_LED_PIN=8`,
      `-DIHEATER_STATUS_LED_ACTIVE_LOW=1`.
  - No `build_src_filter`, and no cloud or MQTT placeholder flags.
  - A three-line header comment in English replaces the Russian one.

- [ ] **Step 5: Update `release.yml`** by deleting the "Fetch iDryer core dependency" step.

- [ ] **Step 6: Update `.gitignore`** by dropping the entries that only
  served removed things: `img/.resize-hash`, `package*.json`,
  `docs-service/` and the commit-message scripts.

- [ ] **Step 7: Run all three gates**

Run: the build command, the policy test command and the Klipper test command.
Expected: all pass. `git grep -n -i "idryer-core\|idryer::\|bambu\|PubSubClient" -- . ':!docs/design'`
prints nothing. Record the flash percentage.

- [ ] **Step 8: Commit**: `chore: remove upstream iDryer firmware, envs, docs and tools`.

---

### Task 7: Fake Moonraker, docs and AGENTS.md

**Files:**
- Modify: `tools/fake_moonraker/fake_moonraker.py`, `tools/fake_moonraker/fake_moonraker.README.md`, `tools/README.md`, `README.md`
- Create: `AGENTS.md`

- [ ] **Step 1: Rewrite `fake_moonraker.py`** in English, using `websockets`.
  - **Options:** `--port 7125`, `--token XXXX-XXXX` (default none, which
    means no token, the unpaired case), `--heaters N` (default 1; extra
    heaters get other tokens), `--objects 150` (padding objects in the
    list).
  - **Handles:** `server.connection.identify`, `printer.objects.list`,
    `printer.objects.query` and `printer.objects.subscribe`. List and
    query return a JSON-RPC error while the simulated Klipper is not
    ready.
  - **Sends:** `notify_proc_stat_update` every second as noise.
  - **stdin commands:**
    - `t <C>` sets the target and notifies.
    - `restart`: sends `notify_klippy_disconnected`, sets the target to 0,
      and sends `notify_klippy_ready` after 2 s.
    - `shutdown`: sets the target to 0, sends `notify_status_update` and
      `notify_klippy_shutdown`.
    - `freeze` stops answering everything, pings included, until
      `thaw`.
    - `drop` closes the sockets.

- [ ] **Step 2: Syntax check**

Run: `python -I -m py_compile tools/fake_moonraker/fake_moonraker.py`.
Expected: no output.

- [ ] **Step 3: Update the fake's README and `tools/README.md`** in English.
  - The fake README gets the Review Focus scenarios as a bench checklist:
    restart, freeze, unpaired, noise and large list, shutdown.
  - Remove the Bambu and portal sections from `tools/README.md`.

- [ ] **Step 4: Update `README.md`.**
  - Delete "Keeping Sync With Upstream".
  - Rewrite "What the Local Remote Does" and "Controls": Manual or
    Klipper (Moonraker), pairing token, TLS, `moonrakerError`.
  - Remove the Bambu and bed-rule bullets.
  - Replace the watchdog bullet with the ping and 30 s query.
  - Keep the link-loss text, Credits and Notes.
  - Add "Updating from 2.0": settings carry over; add `remote_token` and
    remove the macro (link to `klipper/README.md`).

- [ ] **Step 5: Create `AGENTS.md`** at the repo root with:
  - The purpose, in one paragraph.
  - The three gate commands, noting that policy tests are build-only.
  - The layout: `src/standalone/`, `lib/remote-policy/`, `klipper/`,
    `tools/fake_moonraker/`.
  - Rules:
    - Canadian English.
    - Pure decisions go in `remote-policy` with tests.
    - Never flash without asking.
    - Policy files use CRLF.
    - Design and plan docs go in `docs/design/`.
    - Commit trailer.

- [ ] **Step 6: Run all three gates**

Run: all three gates.
Expected: all pass.

- [ ] **Step 7: Commit**: `docs: README, AGENTS.md and fake Moonraker for the local-only Remote`.

- [ ] **Step 8: Bench run, with the user's go-ahead.**
  - Run the fake Moonraker scenarios against the device, then
    `wss://hestia.brunton.ca` with the real `remote_token`.
  - Record each Review Focus item as pass or fail in the ledger.
