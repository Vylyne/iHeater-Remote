# Wi-Fi resilience, TX power and link-loss safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Remote reconnects Wi-Fi on its own, lets the user pick TX power, and never heats while the printer link is down.

**Architecture:** Pure decisions go in `lib/remote-policy` (Unity-tested); `StandaloneApp.cpp` wires them into `loop()`, NVS and the Setup page.

**Tech Stack:** PlatformIO, Arduino-ESP32, Unity.

**Spec:** `docs/design/2026-11-19-wifi-resilience-safety-design.md`

## Global Constraints

- Branch `feature/wifi-resilience-safety`; no idryer-core changes; Canadian English.
- STA reconnect interval 30000 ms; AP-mode STA retry interval 120000 ms.
- TX power values (Arduino `wifi_power_t`, quarter-dBm): 34 (8.5), 44 (11), 52 (13), 60 (15), 68 (17), 78 (19.5). Default 34. NVS key `tx-power`.
- Build: `pio run -e esp32c3-super-mini-remote` must succeed.
- Policy tests: `pio test -e esp32c3-super-mini-remote -f test_remote_policy --without-uploading --without-testing` (build only; no device run without asking).

## Review Focus

1. Router reboots while printing → STA reconnect within ~30 s, Moonraker resubscribes, heat resumes.
2. Power cut, router slower than ESP32 → AP fallback, joins within ~2 min, AP and DNS stop.
3. Moonraker in bed-rule mode (no VIRTUAL_CHAMBER), Wi-Fi drops → heat off.
4. `millis()` wrap during either interval → no stuck or rapid-fire retries.
5. Manual mode while Moonraker/Bambu selected → link-loss rule does not touch manual heat.

---

### Task 1: Policy functions

**Files:**
- Modify: `lib/remote-policy/src/RemotePolicy.h`, `lib/remote-policy/src/RemotePolicy.cpp`
- Test: `test/test_remote_policy/test_main.cpp`

**Interfaces (namespace `iheaterlink::policy`):**
- `constexpr uint32_t kStaReconnectMs = 30000; constexpr uint32_t kApRetryMs = 120000;`
- `enum class WifiAction : uint8_t { None, Reconnect, RetryFromAp };`
- `WifiAction wifiSupervisorAction(bool apMode, bool hasCredentials, bool staConnected, uint32_t nowMs, uint32_t lastAttemptMs);` — STA connected → None; STA (not AP) disconnected and elapsed ≥ kStaReconnectMs → Reconnect; AP mode with credentials, STA not connected, elapsed ≥ kApRetryMs → RetryFromAp; else None. Elapsed is wrap-safe.
- `bool cutHeatForLinkLoss(bool printerSourceActive, bool heatingFromPrinter, bool linkConnected);` — `printerSourceActive && heatingFromPrinter && !linkConnected`.
- `bool validTxPower(int quarterDbm);` — true only for 34, 44, 52, 60, 68, 78.

- [ ] **Step 1: Add failing tests**

```cpp
test_wifi_connected_none:        wifiSupervisorAction(false,true,true,100000,0)==None
test_wifi_sta_reconnect_due:     wifiSupervisorAction(false,true,false,30000,0)==Reconnect
test_wifi_sta_reconnect_not_due: wifiSupervisorAction(false,true,false,29999,0)==None
test_wifi_ap_retry_due:          wifiSupervisorAction(true,true,false,120000,0)==RetryFromAp
test_wifi_ap_retry_not_due:      wifiSupervisorAction(true,true,false,119999,0)==None
test_wifi_ap_no_credentials:     wifiSupervisorAction(true,false,false,500000,0)==None
test_wifi_wraparound:            wifiSupervisorAction(false,true,false,5,0xFFFFFFF0u)==None; (false,true,false,30000,0xFFFFFFF0u)==Reconnect
test_link_loss_cuts:             cutHeatForLinkLoss(true,true,false)==true
test_link_loss_connected:        cutHeatForLinkLoss(true,true,true)==false
test_link_loss_not_heating:      cutHeatForLinkLoss(true,false,false)==false
test_link_loss_manual:           cutHeatForLinkLoss(false,true,false)==false
test_tx_power_valid:             validTxPower(34),44,52,60,68,78 true; 0,33,80,84 false
```

- [ ] **Step 2: Build tests, expect compile failure.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Build tests, expect success.**
- [ ] **Step 5: Commit** `feat(remote): Wi-Fi supervisor, link-loss and TX power policy`

### Task 2: Wire into the Remote

**Files:** Modify `src/standalone/StandaloneApp.cpp`.

**Interfaces:** Consumes Task 1. Produces status JSON `wifiTxPower` (int, quarter-dBm) and `wifiRssi` (int dBm, 0 in AP mode); setup form field `txPower`.

- [ ] **Step 1: TX power.** `kTxPowerKey = "tx-power"`, `int s_txPower` loaded in `loadAutoSettings` (default `IHEATER_WIFI_TX_POWER`, accepted only if `validTxPower`). Replace `applyConfiguredWifiTxPower(...)` calls in this file with a local `applyTxPower(const char *context)` using `s_txPower` (load settings before `configureWiFi()` in `setup()`). Setup page: in the Wi-Fi card add `<label>Transmit power<select name=txPower>` with the six options (labels `8.5 dBm (default)`, `11 dBm`, `13 dBm`, `15 dBm`, `17 dBm`, `19.5 dBm`; `__TXP_<n>__` → `selected`), a note `Higher power can help a weak signal; some C3 Super Mini boards fail to connect above 8.5 dBm. Changing it restarts the device.`, and `Signal: __RSSI__` note. `applySetupForm`: if `txPower` present and different, validate, persist, `scheduleRestart()`; invalid → error `Invalid transmit power.`
- [ ] **Step 2: Wi-Fi supervisor.** `void updateWifiSupervisor()` in `loop()`: track `s_wifiLastAttemptMs` (set to now while STA connected). On `Reconnect`: log `[REMOTE] WiFi down, reconnecting`, `WiFi.reconnect()`, stamp. On `RetryFromAp`: if not yet AP+STA, `WiFi.mode(WIFI_AP_STA)`; `WiFi.begin(stored ssid, password)`; `applyTxPower`; log; stamp. When in AP mode and `WiFi.status() == WL_CONNECTED`: log IP, `s_dnsServer.stop()`, `WiFi.softAPdisconnect(true)`, `WiFi.mode(WIFI_STA)`, `s_apMode = false`. Credentials come from `loadWifiCredentials`.
- [ ] **Step 3: Link-loss safety.** `void updateLinkSafety()` in `loop()` after `s_integrations.loop()`: printer source = active is Moonraker or Bambu; heating from printer = output heating and `s_targetSource` in {klipper, bed-rule, bambu}; connected = that client's state is Connected. If `cutHeatForLinkLoss` → `applyIntegrationTarget(0, "printer link lost", "link-lost")` and log once.
- [ ] **Step 4: Status JSON** `wifiTxPower`, `wifiRssi`.
- [ ] **Step 5: Build** `pio run -e esp32c3-super-mini-remote` → SUCCESS.
- [ ] **Step 6: Docs** — README Wi-Fi section: reconnect, AP retry, TX power, link-loss rule.
- [ ] **Step 7: Commit** `feat(remote): Wi-Fi reconnect, AP retry, TX power setting, link-loss safety`
- [ ] **Step 8: On-device checks (user):** router off/on → reconnects ≤ ~30 s; boot with router off then on → joins ≤ ~2 min, AP gone; Moonraker heating then stop Moonraker → heat off immediately; TX power change → restart and new value shown.
