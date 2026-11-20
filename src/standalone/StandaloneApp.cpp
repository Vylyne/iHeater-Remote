#include "standalone/StandaloneApp.h"
#include "standalone/MoonrakerLink.h"
#include "standalone/RemoteDashboard.h"
#include "standalone/RemoteSettings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <IPAddress.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <string.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include "controller/RmtOutputAdapter.h"
#include <RemotePolicy.h>
#include "version.h"

namespace iheaterlink {
namespace standalone {

#ifndef IHEATER_BOOT_BUTTON_PIN
#define IHEATER_BOOT_BUTTON_PIN 9
#endif

#ifndef IHEATER_STATUS_LED_PIN
#define IHEATER_STATUS_LED_PIN 8
#endif

#ifndef IHEATER_STATUS_LED_ACTIVE_LOW
#define IHEATER_STATUS_LED_ACTIVE_LOW 1
#endif

#ifndef IHEATER_AP_PASSWORD
#define IHEATER_AP_PASSWORD ""
#endif

namespace {

constexpr uint8_t kMaxMode = 7;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kRestartDelayMs = 800;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kMultiClickWindowMs = 650;
constexpr uint32_t kLongPressMs = 2000;
constexpr uint32_t kLedOnMs = 80;
constexpr uint32_t kLedOffMs = 120;
constexpr uint32_t kLedPauseMs = 1600;
constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kMaxTimerMinutes = 24 * 60;
constexpr size_t kMaxWifiSsidLen = 32;
constexpr size_t kMaxWifiPasswordLen = 64;

const char *kPrefsNamespace = "iheater-remote";
const char *kWifiSsidKey = "ssid";
const char *kWifiPasswordKey = "pass";
const char *kMinTargetKey = "min-target";
const char *kMaxTargetKey = "max-target";
const char *kTxPowerKey = "tx-power";

#ifdef IHEATER_WIFI_TX_POWER
constexpr int kDefaultTxPower = IHEATER_WIFI_TX_POWER;
#else
constexpr int kDefaultTxPower = WIFI_POWER_8_5dBm;
#endif
constexpr uint32_t kRssiSampleMs = 5000;
constexpr uint32_t kHeapLogMs = 60000;

const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApSubnet(255, 255, 255, 0);

const float kModeTempsC[] = {
    0.0f, 55.0f, 60.0f, 65.0f, 70.0f, 75.0f, 80.0f, 85.0f,
};

DNSServer s_dnsServer;
WebServer s_server(80);
WebSocketsServer s_statusSocket(81);
// The adapter's own window is widened to the full pulse range; the target
// range policy (s_range) does the clamping.
RmtOutputConfig remoteOutputConfig() {
  RmtOutputConfig config{};
  config.minTempC = policy::kRangeFloorC;
  config.maxTempC = policy::kRangeCeilC;
  return config;
}

RmtOutputAdapter s_output{remoteOutputConfig()};
RemoteSettings s_settings;
MoonrakerLink s_moonraker;

uint8_t s_mode = 0;
bool s_apMode = false;
String s_apSsid;
String s_stationSsid;
String s_wifiCredentialSource = "none";
uint16_t s_timerMinutes = 0;
uint32_t s_timerOffAtMs = 0;
uint32_t s_restartAtMs = 0;
bool s_otaActive = false;
bool s_otaOk = false;
size_t s_otaBytesWritten = 0;
String s_otaError;
policy::TargetRange s_range{policy::kDefaultMinC, policy::kDefaultMaxC};
String s_targetSource = "manual";
// Last loop() pass on which the active printer link was healthy.
uint32_t s_linkHealthyAtMs = 0;
uint32_t s_wifiLastAttemptMs = 0;
int s_txPower = kDefaultTxPower;
int s_wifiRssi = 0;
uint32_t s_wifiRssiAtMs = 0;
uint32_t s_heapLogAtMs = 0;

bool s_buttonStableDown = false;
bool s_buttonLastRawDown = false;
bool s_longPressHandled = false;
uint8_t s_pendingClicks = 0;
uint32_t s_buttonLastRawChangeMs = 0;
uint32_t s_buttonDownAtMs = 0;
uint32_t s_clickDeadlineMs = 0;

bool s_ledOn = false;
uint8_t s_ledFlashesDone = 0;
uint32_t s_ledNextAtMs = 0;

void applyTxPower(const char *context) {
  const bool ok = WiFi.setTxPower(static_cast<wifi_power_t>(s_txPower));
  Serial.printf("[WIFI] TX power %s power=%d context=%s\n",
                ok ? "set" : "not-set", s_txPower, context);
}

// Sampled every few seconds so signal jitter does not trigger a status
// push on every 250 ms check.
int wifiRssi() {
  if (s_apMode || WiFi.status() != WL_CONNECTED)
    return 0;
  const uint32_t now = millis();
  if (s_wifiRssi == 0 || now - s_wifiRssiAtMs >= kRssiSampleMs) {
    s_wifiRssi = WiFi.RSSI();
    s_wifiRssiAtMs = now;
  }
  return s_wifiRssi;
}

bool ledEnabled() { return IHEATER_STATUS_LED_PIN != 0xFF; }
void resetLedPattern();

bool klipperSource() { return s_settings.source == ControlSource::Klipper; }

void applyIntegrationTarget(float targetTempC, const char *source,
                            const char *targetSource) {
  targetTempC = policy::clampTargetC(targetTempC, s_range);
  s_targetSource = targetSource;
  ControllerOutputCommand cmd{};
  if (targetTempC > 0.0f) {
    cmd.mode = ControllerOutputMode::TargetTemperature;
    cmd.targetTempC = targetTempC;
  } else {
    cmd.mode = ControllerOutputMode::Off;
  }
  s_mode = 0; // Integration targets are not one of the seven manual modes.
  s_timerOffAtMs = 0;
  s_output.apply(cmd);
  s_output.forceFrame();
  resetLedPattern();
  Serial.printf("[REMOTE] %s target=%.1fC pulse=%u\n", source,
                (double)cmd.targetTempC, s_output.getLastPulseCode());
}

// MoonrakerLink callback: the paired heater_virtual's target (0 = off). The
// link holds the last target through outages; updateLinkSafety() cuts heat
// if the link stays down.
void onKlipperTarget(float targetC) {
  if (klipperSource())
    applyIntegrationTarget(targetC, "klipper", "klipper");
}

void writeLed(bool on) {
  if (!ledEnabled())
    return;
#if IHEATER_STATUS_LED_ACTIVE_LOW
  digitalWrite(IHEATER_STATUS_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(IHEATER_STATUS_LED_PIN, on ? HIGH : LOW);
#endif
  s_ledOn = on;
}

void resetLedPattern() {
  s_ledFlashesDone = 0;
  s_ledNextAtMs = 0;
  writeLed(false);
}

// Only user actions (web and button) pass releasePrinter: a manual press
// unbinds the printer. Boot, the timer and OTA leave the binding alone.
void applyMode(uint8_t mode, const char *source, bool releasePrinter = true) {
  if (mode > kMaxMode)
    mode = kMaxMode;

  if (releasePrinter && klipperSource()) {
    s_settings.source = ControlSource::Manual;
    if (!saveSourceAndMoonraker(s_settings))
      Serial.println("[REMOTE] could not save manual source");
    s_moonraker.stop();
  }
  s_mode = mode;
  if (mode == 0) {
    s_timerOffAtMs = 0;
  } else if (s_timerMinutes > 0) {
    s_timerOffAtMs = millis() + (uint32_t)s_timerMinutes * 60u * 1000u;
  } else {
    s_timerOffAtMs = 0;
  }

  ControllerOutputCommand cmd;
  if (mode == 0) {
    cmd.mode = ControllerOutputMode::Off;
    cmd.targetTempC = 0.0f;
  } else {
    cmd.mode = ControllerOutputMode::TargetTemperature;
    cmd.targetTempC = policy::clampTargetC(kModeTempsC[mode], s_range);
  }
  s_targetSource = "manual";
  s_output.apply(cmd);
  s_output.forceFrame();
  resetLedPattern();

  Serial.printf("[REMOTE] %s mode=%u target=%.1fC timer=%umin pulse=%u\n",
                source, mode, (double)cmd.targetTempC, s_timerMinutes,
                s_output.getLastPulseCode());
}

void updateHeatTimer() {
  if (s_mode == 0 || s_timerOffAtMs == 0)
    return;

  if ((int32_t)(millis() - s_timerOffAtMs) >= 0) {
    applyMode(0, "timer", false);
  }
}

uint32_t timerRemainingSeconds() {
  if (s_mode == 0 || s_timerOffAtMs == 0)
    return 0;

  const int32_t remainingMs = (int32_t)(s_timerOffAtMs - millis());
  if (remainingMs <= 0)
    return 0;
  return ((uint32_t)remainingMs + 999u) / 1000u;
}

String localIpString() {
  return s_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

const char *wifiModeString() { return s_apMode ? "ap" : "sta"; }

bool loadStoredWifiCredentials(String &ssid, String &password) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true))
    return false;

  ssid = prefs.getString(kWifiSsidKey, "");
  password = prefs.getString(kWifiPasswordKey, "");
  prefs.end();

  ssid.trim();
  return ssid.length() > 0;
}

bool loadWifiCredentials(String &ssid, String &password, String &source) {
  if (loadStoredWifiCredentials(ssid, password)) {
    source = "stored";
    return true;
  }

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  ssid = WIFI_SSID;
  password = WIFI_PASSWORD;
  ssid.trim();
  if (ssid.length() > 0) {
    source = "build";
    return true;
  }
#endif

  source = "none";
  return false;
}

bool saveStoredWifiCredentials(const String &ssid, const String &password) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false))
    return false;

  const bool ok = prefs.putString(kWifiSsidKey, ssid) > 0 &&
                  prefs.putString(kWifiPasswordKey, password) >= 0;
  prefs.end();
  return ok;
}

bool clearStoredWifiCredentials() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false))
    return false;

  prefs.remove(kWifiSsidKey);
  prefs.remove(kWifiPasswordKey);
  prefs.end();
  return true;
}

// Target range and transmit power.
void loadDeviceSettings() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true))
    return;
  const uint8_t minTarget = prefs.getUChar(kMinTargetKey, policy::kDefaultMinC);
  const uint8_t maxTarget = prefs.getUChar(kMaxTargetKey, policy::kDefaultMaxC);
  const int txPower = prefs.getInt(kTxPowerKey, kDefaultTxPower);
  prefs.end();
  if (policy::validTxPower(txPower))
    s_txPower = txPower;
  if (policy::validRange(minTarget, maxTarget))
    s_range = policy::TargetRange{minTarget, maxTarget};
}

bool saveRange(int minC, int maxC) {
  if (!policy::validRange(minC, maxC))
    return false;
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false))
    return false;
  const bool ok = prefs.putUChar(kMinTargetKey, minC) > 0 &&
                  prefs.putUChar(kMaxTargetKey, maxC) > 0;
  prefs.end();
  if (ok)
    s_range = policy::TargetRange{static_cast<uint8_t>(minC),
                                  static_cast<uint8_t>(maxC)};
  return ok;
}

bool saveTxPower(int quarterDbm) {
  if (!policy::validTxPower(quarterDbm))
    return false;
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false))
    return false;
  const bool ok = prefs.putInt(kTxPowerKey, quarterDbm) > 0;
  prefs.end();
  if (ok)
    s_txPower = quarterDbm;
  return ok;
}

void scheduleRestart() { s_restartAtMs = millis() + kRestartDelayMs; }

void updateRestart() {
  if (s_restartAtMs == 0)
    return;

  if ((int32_t)(millis() - s_restartAtMs) >= 0) {
    Serial.println("[REMOTE] Restarting");
    Serial.flush();
    ESP.restart();
  }
}

// Backstop for a printer that never comes back: heat the printer commanded
// is held through link outages and cut once the link has been down for
// policy::kLinkLossGraceMs. Manual heat is never touched.
void updateLinkSafety() {
  const uint32_t now = millis();
  const bool printerSource = klipperSource();
  const bool healthy = !printerSource || s_moonraker.healthy();
  if (healthy) {
    s_linkHealthyAtMs = now;
    return;
  }
  const bool heatingFromPrinter =
      s_output.getLastCommand().mode == ControllerOutputMode::TargetTemperature &&
      s_targetSource == "klipper";
  if (policy::cutHeatForLinkLoss(printerSource, heatingFromPrinter, healthy,
                                 now, s_linkHealthyAtMs,
                                 policy::kLinkLossGraceMs))
    applyIntegrationTarget(0.0f, "printer link lost 10 min", "link-lost");
}

void updateHeapLog() {
  const uint32_t now = millis();
  if (!policy::intervalElapsed(now, s_heapLogAtMs, kHeapLogMs))
    return;
  s_heapLogAtMs = now;
  Serial.printf("[REMOTE] heap free=%u min=%u moon=%s\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()),
                s_moonraker.stateString());
}

// Station mode: reconnect every 30 s while down. AP fallback with saved
// credentials: retry them in AP+STA every 2 min, and drop the AP once joined.
void updateWifiSupervisor() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    s_wifiLastAttemptMs = now;
    if (s_apMode) {
      Serial.printf("[REMOTE] WiFi joined from AP mode IP=%s, stopping AP\n",
                    WiFi.localIP().toString().c_str());
      s_dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      applyTxPower("remote-sta");
      s_apMode = false;
    }
    return;
  }
  const policy::WifiAction action = policy::wifiSupervisorAction(
      s_apMode, s_stationSsid.length() > 0, false, now, s_wifiLastAttemptMs);
  if (action == policy::WifiAction::Reconnect) {
    Serial.println("[REMOTE] WiFi down, reconnecting");
    WiFi.reconnect();
    s_wifiLastAttemptMs = now;
  } else if (action == policy::WifiAction::RetryFromAp) {
    s_wifiLastAttemptMs = now;
    String ssid;
    String password;
    String source;
    if (!loadWifiCredentials(ssid, password, source))
      return;
    if (WiFi.getMode() != WIFI_AP_STA)
      WiFi.mode(WIFI_AP_STA);
    else
      WiFi.disconnect(false);
    WiFi.begin(ssid.c_str(), password.c_str());
    applyTxPower("remote-ap-sta");
    Serial.printf("[REMOTE] AP mode, retrying WiFi SSID=%s\n", ssid.c_str());
  }
}

bool isIpLiteral(const String &host) {
  for (size_t i = 0; i < host.length(); ++i) {
    const char c = host[i];
    if ((c < '0' || c > '9') && c != '.' && c != ':')
      return false;
  }
  return host.length() > 0;
}

bool captiveRedirectIfNeeded() {
  if (!s_apMode)
    return false;

  const String host = s_server.hostHeader();
  if (host.length() == 0 || isIpLiteral(host))
    return false;

  s_server.sendHeader("Location", String("http://") + kApIp.toString() + "/",
                      true);
  s_server.send(302, "text/plain", "");
  return true;
}

String htmlEscape(const char *raw) {
  String out;
  for (const char *p = raw ? raw : ""; *p; ++p) {
    switch (*p) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '\"': out += "&quot;"; break;
    case '\'': out += "&#39;"; break;
    default: out += *p; break;
    }
  }
  return out;
}

void handleSetupPage() {
  if (captiveRedirectIfNeeded())
    return;
  String page = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>iHeater setup</title><style>:root{color-scheme:dark;--bg:#090d14;--p:#101722;--l:#29384b;--t:#eaf1fa;--m:#91a2b8;--a:#5ba9ff;font-family:system-ui,sans-serif}body{margin:0;background:var(--bg);color:var(--t)}main{max-width:620px;margin:auto;padding:24px 15px}.card{background:var(--p);border:1px solid var(--l);border-radius:12px;padding:16px;margin:14px 0}h1{font-size:23px}h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:var(--m)}label{display:grid;gap:6px;margin:12px 0;color:var(--m);font-size:13px}input,select,button{min-height:43px;border-radius:8px;border:1px solid #38506b;background:#0c131d;color:var(--t);font:inherit;padding:0 10px}button{background:#1d5d99;border-color:#69b4ff;font-weight:700;width:100%;margin-top:8px}.check{display:flex;align-items:center;gap:10px}.check input{min-height:0;width:18px;height:18px;margin:0;padding:0}.note{font-size:13px;line-height:1.45;color:var(--m)}a{color:var(--a)}</style><main><p><a href="/">← Dashboard</a></p><h1>Wi-Fi &amp; printer setup</h1><p class=note>Blank password and API key fields keep their stored values.</p><form method=post action="/api/setup"><section class=card><h2>Wi-Fi</h2><label>Network name<input name=ssid value="__SSID__" maxlength=32></label><label>Password <input name=password type=password placeholder="(unchanged)" maxlength=64></label><label>Transmit power<select name=txPower><option value=34 __TXP_34__>8.5 dBm (default)</option><option value=44 __TXP_44__>11 dBm</option><option value=52 __TXP_52__>13 dBm</option><option value=60 __TXP_60__>15 dBm</option><option value=68 __TXP_68__>17 dBm</option><option value=78 __TXP_78__>19.5 dBm</option></select></label><p class=note>Higher power can help a weak signal; some C3 Super Mini boards fail to connect above 8.5 dBm. Changing it restarts the device.</p><p class=note>Signal: __RSSI__</p></section><section class=card><h2>Control source</h2><label>Bind this heater to<select name=type id=source onchange=showSource()><option value=manual __MANUAL__>Manual controls only</option><option value=moonraker __MOON__>Klipper (Moonraker)</option></select></label><div id=moon><label>Host / IP<input name=host value="__MOON_HOST__" maxlength=63></label><label>Port<input name=port id=port type=number value="__MOON_PORT__" min=1 max=65535></label><label class=check><input name=tls id=tls type=checkbox value=1 __MOON_TLS__>Use TLS (wss://, e.g. through Mainsail on 443)</label><label>API key <input name=apiKey type=password placeholder="(unchanged)" maxlength=64></label></div></section><section class=card><h2>Pairing</h2><label>Pairing token<input name=pairToken value="__PAIR_TOKEN__" maxlength=16 autocomplete=off autocapitalize=characters spellcheck=false></label><label class=check><input name=regenToken type=checkbox value=1>Generate a new token on save</label><p class=note>Put this in your [heater_virtual] section as remote_token: __PAIR_TOKEN__. After a wipe, type your old token here to keep your printer.cfg unchanged.</p></section><section class=card><h2>iHeater target range</h2><p class=note>These values do not change any setting on the iHeater. Stock v0.5.0 firmware accepts 45–90 and faults when the air reaches 90.</p><p class=note>Min must not be lower than your iHeater's minimum: lower targets are ignored by the iHeater, which keeps its previous target.</p><p class=note>We recommend a Max no higher than 80 (10 °C below the iHeater's MAX_AIR_TEMP).</p><label>Min target (C)<input name=minTargetC type=number value="__MIN_TARGET__" min=11 max=90></label><label>Max target (C)<input name=maxTargetC type=number value="__MAX_TARGET__" min=11 max=90></label></section><button>Save local setup</button></form></main><script>function showSource(){document.getElementById('moon').style.display=document.getElementById('source').value==='moonraker'?'block':'none'}showSource();document.getElementById('tls').onchange=e=>{let p=document.getElementById('port');if(e.target.checked&&p.value==='7125')p.value='443';else if(!e.target.checked&&p.value==='443')p.value='7125'}</script></html>)HTML";
  page.replace("<h1>Wi-Fi &amp; printer setup</h1>",
               "<div style='font-size:23px;font-weight:800;letter-spacing:-1px'><span style='color:#5ba9ff'>i</span>Dryer</div><h1>Wi-Fi &amp; printer setup</h1>");
  page.replace("</form></main>",
               String("</form><p class=note><a href='/fw'>Firmware update</a> · iHeater Remote v") + VERSION_STR + "</p></main>");
  for (const int txp : {34, 44, 52, 60, 68, 78})
    page.replace(String("__TXP_") + txp + "__", s_txPower == txp ? "selected" : "");
  const int rssi = wifiRssi();
  page.replace("__RSSI__", rssi ? String(rssi) + " dBm" : String("not connected"));
  const bool klipper = klipperSource();
  page.replace("__MANUAL__", klipper ? "" : "selected");
  page.replace("__MOON__", klipper ? "selected" : "");
  page.replace("__MOON_TLS__", s_settings.moon.tls ? "checked" : "");
  page.replace("__MOON_PORT__", String(s_settings.moon.port));
  page.replace("__MIN_TARGET__", String(s_range.minC));
  page.replace("__MAX_TARGET__", String(s_range.maxC));
  char token[policy::kPairTokenLen + 2] = {};
  if (s_settings.pairToken.length() == policy::kPairTokenLen)
    policy::formatPairToken(s_settings.pairToken.c_str(), token);
  // Stored, user-supplied values go last so they cannot fill a placeholder.
  page.replace("__PAIR_TOKEN__", token);
  page.replace("__MOON_HOST__", htmlEscape(s_settings.moon.host.c_str()));
  page.replace("__SSID__", htmlEscape(s_stationSsid.c_str()));
  s_server.send(200, "text/html", page);
}

void handleFirmwarePage() {
  if (captiveRedirectIfNeeded())
    return;
  String page = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>iDryer firmware update</title><style>:root{color-scheme:dark;--bg:#090d14;--p:#101722;--l:#29384b;--t:#eaf1fa;--m:#91a2b8;--a:#5ba9ff;--bad:#ff8893;font-family:system-ui,sans-serif}body{margin:0;background:var(--bg);color:var(--t)}main{max-width:560px;margin:auto;padding:36px 15px}.logo{font-size:24px;font-weight:800;letter-spacing:-1px}.logo i{font-style:normal;color:var(--a)}.card{background:var(--p);border:1px solid var(--l);border-radius:12px;padding:18px;margin:16px 0}h1{font-size:22px;margin:14px 0 7px}.note{color:var(--m);font-size:13px;line-height:1.5}input,button{box-sizing:border-box;width:100%;min-height:45px;border-radius:8px;border:1px solid #38506b;background:#0c131d;color:var(--t);font:inherit;padding:8px;margin:8px 0}button{background:#1d5d99;border-color:#69b4ff;font-weight:700;cursor:pointer}.warning{color:var(--bad)}a{color:var(--a)}</style><main><p><a href="/">← Dashboard</a></p><div class=logo><i>i</i>Dryer</div><h1>Firmware update</h1><p class=note>Installed: iHeater Remote v__VERSION__</p><section class=card><p class="note warning"><strong>Heating is stopped before the update is written.</strong> Do not disconnect power while the upload is in progress.</p><input id=file accept=".bin,application/octet-stream" type=file><button id=install>Install update</button><p class=note id=msg></p></section></main><script>const f=document.getElementById('file'),m=document.getElementById('msg');document.getElementById('install').onclick=()=>{let file=f.files[0];if(!file){m.textContent='Choose a firmware .bin first.';return}let x=new XMLHttpRequest(),d=new FormData();d.append('firmware',file,file.name);x.open('POST','/api/ota');x.upload.onprogress=e=>m.textContent=e.lengthComputable?'Uploading '+Math.round(e.loaded/e.total*100)+'%':'Uploading…';x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(_){}if(x.status>=200&&x.status<300){m.textContent='Update written. Rebooting…';setTimeout(waitForDevice,1500)}else m.textContent=r.error||'Update failed.'};x.onerror=()=>m.textContent='Update failed.';x.send(d)};function waitForDevice(){let end=Date.now()+120000,t=setInterval(async()=>{try{let r=await fetch('/',{cache:'no-store'});if(r.ok){clearInterval(t);location.href='/'}}catch(_){}if(Date.now()>end){clearInterval(t);m.textContent='The device is restarting. Reopen the dashboard when it is back online.'}},1000)}</script></main>)HTML";
  page.replace("__VERSION__", VERSION_STR);
  s_server.send(200, "text/html", page);
}

String statusJson() {
  updateHeatTimer();

  StaticJsonDocument<1024> doc;
  doc["mode"] = s_mode;
  const auto output = s_output.getLastCommand();
  doc["targetTempC"] = output.targetTempC;
  doc["heating"] = output.mode == ControllerOutputMode::TargetTemperature;
  doc["timerMinutes"] = s_timerMinutes;
  doc["timerActive"] = s_mode != 0 && s_timerOffAtMs != 0;
  doc["timerRemainingSeconds"] = timerRemainingSeconds();
  doc["pulseCode"] = s_output.getLastPulseCode();
  doc["minTargetC"] = s_range.minC;
  doc["maxTargetC"] = s_range.maxC;
  doc["targetSource"] = s_targetSource;
  doc["rmtEnabled"] = s_output.isEnabled();
  doc["wifiMode"] = wifiModeString();
  doc["ip"] = localIpString();
  doc["ssid"] = s_apMode ? s_apSsid : WiFi.SSID();
  doc["wifiTxPower"] = s_txPower;
  doc["wifiRssi"] = wifiRssi();
  doc["configuredWifiSsid"] = s_stationSsid;
  doc["wifiCredentialSource"] = s_wifiCredentialSource;
  doc["restartPending"] = s_restartAtMs != 0;
  doc["otaActive"] = s_otaActive;
  doc["otaOk"] = s_otaOk;
  doc["otaBytesWritten"] = s_otaBytesWritten;
  doc["otaError"] = s_otaError;
  doc["firmwareVersion"] = VERSION_STR;
  const bool klipper = klipperSource();
  doc["source"] = klipper ? "klipper" : "manual";
  doc["moonrakerState"] = s_moonraker.stateString();
  doc["heaterObject"] = s_moonraker.heaterObject();
  doc["moonrakerError"] = klipper ? s_moonraker.lastError() : String();

  String out;
  serializeJson(doc, out);
  return out;
}

void sendActionStatus() {
  s_server.send(200, "application/json", statusJson());
}

void pushStatusIfChanged(bool force = false) {
  static String lastStatus;
  static uint32_t lastCheckMs = 0;
  const uint32_t now = millis();
  if (!force && now - lastCheckMs < 250)
    return;
  lastCheckMs = now;

  String status = statusJson();
  if (force || status != lastStatus) {
    s_statusSocket.broadcastTXT(status);
    lastStatus = status;
  }
}

void onStatusSocketEvent(uint8_t client, WStype_t type, uint8_t *, size_t) {
  if (type == WStype_CONNECTED) {
    String status = statusJson();
    s_statusSocket.sendTXT(client, status);
  }
}

void handleModePost() {
  if (!s_server.hasArg("mode")) {
    s_server.send(400, "application/json", "{\"error\":\"missing mode\"}");
    return;
  }
  const int requested = s_server.arg("mode").toInt();
  if (requested < 0 || requested > kMaxMode) {
    s_server.send(400, "application/json", "{\"error\":\"mode range is 0..7\"}");
    return;
  }

  applyMode(static_cast<uint8_t>(requested), "web");
  sendActionStatus();
}

void handleTimerPost() {
  if (!s_server.hasArg("minutes")) {
    s_server.send(400, "application/json", "{\"error\":\"missing minutes\"}");
    return;
  }

  const int requested = s_server.arg("minutes").toInt();
  if (requested < 0 || requested > kMaxTimerMinutes) {
    s_server.send(400, "application/json",
                  "{\"error\":\"minutes range is 0..1440\"}");
    return;
  }

  s_timerMinutes = static_cast<uint16_t>(requested);
  if (s_mode != 0 && s_timerMinutes > 0) {
    s_timerOffAtMs = millis() + (uint32_t)s_timerMinutes * 60u * 1000u;
  } else {
    s_timerOffAtMs = 0;
  }

  Serial.printf("[REMOTE] web timer=%umin\n", s_timerMinutes);
  sendActionStatus();
}

bool applySetupForm(String &error) {
  const String txPower = s_server.arg("txPower");
  if (txPower.length() && !policy::validTxPower(txPower.toInt())) {
    error = "Invalid transmit power.";
    return false;
  }

  const String type = s_server.arg("type");
  RemoteSettings next = s_settings;
  if (type == "manual" || type == "none") {
    next.source = ControlSource::Manual;
  } else if (type == "moonraker" || type == "klipper") {
    next.source = ControlSource::Klipper;
  } else {
    error = "Unknown control source.";
    return false;
  }

  const String minTarget = s_server.arg("minTargetC");
  const String maxTarget = s_server.arg("maxTargetC");
  const int minC = minTarget.length() ? minTarget.toInt() : s_range.minC;
  const int maxC = maxTarget.length() ? maxTarget.toInt() : s_range.maxC;
  if (!policy::validRange(minC, maxC)) {
    error = "Range must satisfy 11 ≤ min ≤ max ≤ 90.";
    return false;
  }

  // regenToken=1 wins over a typed token, so the typed one is not checked.
  const bool regenToken = s_server.arg("regenToken") == "1";
  String typedToken = s_server.arg("pairToken");
  typedToken.trim();
  char token[policy::kPairTokenLen + 1] = {};
  if (!regenToken && typedToken.length() &&
      !policy::normalizePairToken(typedToken.c_str(), token)) {
    error = "Pairing token must be 8 characters: 0–9 and A–Z without I, L, O or U.";
    return false;
  }

  // The Moonraker fields are only read with Klipper selected: the Setup page
  // hides them for Manual, and /api/integration {type:none} sends none.
  if (next.source == ControlSource::Klipper) {
    String host = s_server.arg("host");
    host.trim();
    if (host.length())
      next.moon.host = host;
    if (!next.moon.host.length()) {
      error = "Klipper needs the Moonraker host or IP address.";
      return false;
    }
    next.moon.tls = s_server.arg("tls") == "1";
    const String port = s_server.arg("port");
    if (port.length()) {
      const long value = port.toInt();
      if (value < 1 || value > 65535) {
        error = "Port must be 1–65535.";
        return false;
      }
      next.moon.port = static_cast<uint16_t>(value);
    } else {
      next.moon.port = policy::defaultMoonrakerPort(next.moon.tls);
    }
    // A TLS handshake against the plain port blocks the main loop for up to
    // 120 s per attempt, so refuse the combination outright.
    if (next.moon.tls && next.moon.port == 7125) {
      error = "Port 7125 is Moonraker's plain port: turn TLS off, or use 443 through Mainsail.";
      return false;
    }
    // Secrets are write-only: a blank field keeps the stored key.
    const String apiKey = s_server.arg("apiKey");
    if (apiKey.length())
      next.moon.apiKey = apiKey;
  }

  if (regenToken)
    next.pairToken = generatePairToken();
  else if (token[0] != '\0')
    next.pairToken = token;

  // Every check has passed: save source and Moonraker settings, the token,
  // the range, then the transmit power, in that order.
  if (!saveSourceAndMoonraker(next)) {
    error = "Could not save the control source.";
    return false;
  }
  s_settings.source = next.source;
  s_settings.moon = next.moon;
  if (next.pairToken != s_settings.pairToken) {
    if (!savePairToken(next.pairToken.c_str())) {
      error = "Could not save the pairing token.";
      return false;
    }
    s_settings.pairToken = next.pairToken;
    char shown[policy::kPairTokenLen + 2];
    policy::formatPairToken(s_settings.pairToken.c_str(), shown);
    Serial.printf("[REMOTE] pairing token set to %s\n", shown);
  }
  if ((minC != s_range.minC || maxC != s_range.maxC) && !saveRange(minC, maxC)) {
    error = "Could not save the target range.";
    return false;
  }
  if (txPower.length() && txPower.toInt() != s_txPower) {
    if (!saveTxPower(txPower.toInt())) {
      error = "Could not save transmit power.";
      return false;
    }
    scheduleRestart();
  }

  if (klipperSource()) {
    s_moonraker.configure(s_settings.moon, s_settings.pairToken.c_str());
  } else {
    s_moonraker.stop();
    applyIntegrationTarget(0.0f, "binding disabled", "manual");
  }
  return true;
}

void handleIntegrationPost() {
  String error;
  if (!applySetupForm(error)) {
    s_server.send(400, "application/json", String("{\"error\":\"") + error + "\"}");
    return;
  }
  sendActionStatus();
}

void handleSetupPost() {
  String error;
  if (!applySetupForm(error)) {
    s_server.send(400, "text/plain", error);
    return;
  }

  String ssid = s_server.arg("ssid");
  const String password = s_server.arg("password");
  ssid.trim();
  if (ssid.length()) {
    if (ssid.length() > kMaxWifiSsidLen || password.length() > kMaxWifiPasswordLen ||
        (password.length() && password.length() < 8)) {
      s_server.send(400, "text/plain", "Invalid Wi-Fi credentials.");
      return;
    }
    // An empty Wi-Fi password preserves the stored password only when the SSID
    // is unchanged; a new network still needs its password.
    if (!password.length() && ssid != s_stationSsid) {
      s_server.send(400, "text/plain", "Enter the password for a new Wi-Fi network.");
      return;
    }
    String storedPassword;
    if (!password.length())
      loadStoredWifiCredentials(s_stationSsid, storedPassword);
    if (!saveStoredWifiCredentials(ssid, password.length() ? password : storedPassword)) {
      s_server.send(500, "text/plain", "Could not save Wi-Fi credentials.");
      return;
    }
    s_stationSsid = ssid;
    s_wifiCredentialSource = "stored";
    scheduleRestart();
  }
  s_server.sendHeader("Location", "/setup", true);
  s_server.send(303, "text/plain", "Saved");
}

void handleWifiPost() {
  if (!s_server.hasArg("ssid")) {
    s_server.send(400, "application/json", "{\"error\":\"missing ssid\"}");
    return;
  }

  String ssid = s_server.arg("ssid");
  String password = s_server.hasArg("password") ? s_server.arg("password") : "";
  ssid.trim();

  if (ssid.length() == 0 || ssid.length() > kMaxWifiSsidLen) {
    s_server.send(400, "application/json",
                  "{\"error\":\"ssid length must be 1..32\"}");
    return;
  }

  if (password.length() > kMaxWifiPasswordLen ||
      (password.length() > 0 && password.length() < 8)) {
    s_server.send(400, "application/json",
                  "{\"error\":\"password length must be 0 or 8..64\"}");
    return;
  }

  if (!saveStoredWifiCredentials(ssid, password)) {
    s_server.send(500, "application/json", "{\"error\":\"save failed\"}");
    return;
  }

  s_stationSsid = ssid;
  s_wifiCredentialSource = "stored";
  scheduleRestart();
  Serial.printf("[REMOTE] saved WiFi SSID=%s, restarting\n", ssid.c_str());
  sendActionStatus();
}

void handleWifiClearPost() {
  if (!clearStoredWifiCredentials()) {
    s_server.send(500, "application/json", "{\"error\":\"clear failed\"}");
    return;
  }

  s_stationSsid = "";
  s_wifiCredentialSource = "none";
  scheduleRestart();
  Serial.println("[REMOTE] cleared stored WiFi credentials, restarting");
  sendActionStatus();
}

void handleOtaPost() {
  if (Update.hasError() || !s_otaOk) {
    StaticJsonDocument<192> doc;
    doc["error"] = s_otaError.length() ? s_otaError : Update.errorString();
    doc["bytesWritten"] = s_otaBytesWritten;

    String out;
    serializeJson(doc, out);
    s_server.send(500, "application/json", out);
    return;
  }

  scheduleRestart();

  StaticJsonDocument<192> doc;
  doc["ok"] = true;
  doc["bytesWritten"] = s_otaBytesWritten;
  doc["restartPending"] = true;

  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

void handleOtaUpload() {
  HTTPUpload &upload = s_server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    s_otaActive = true;
    s_otaOk = false;
    s_otaBytesWritten = 0;
    s_otaError = "";

    applyMode(0, "ota:start", false);
    Serial.printf("[REMOTE] OTA upload start filename=%s\n",
                  upload.filename.c_str());

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      s_otaError = Update.errorString();
      Serial.printf("[REMOTE] OTA begin failed: %s\n", s_otaError.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError()) {
      const size_t written = Update.write(upload.buf, upload.currentSize);
      s_otaBytesWritten += written;
      if (written != upload.currentSize) {
        s_otaError = Update.errorString();
        Serial.printf("[REMOTE] OTA write failed: %s\n", s_otaError.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.hasError() && Update.end(true)) {
      s_otaOk = true;
      s_otaError = "";
      Serial.printf("[REMOTE] OTA upload complete bytes=%u\n",
                    (unsigned)s_otaBytesWritten);
    } else {
      s_otaOk = false;
      s_otaError = Update.errorString();
      Serial.printf("[REMOTE] OTA end failed: %s\n", s_otaError.c_str());
    }
    s_otaActive = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_otaActive = false;
    s_otaOk = false;
    s_otaError = "upload aborted";
    Serial.println("[REMOTE] OTA upload aborted");
  }
}

void handleRoot() {
  if (captiveRedirectIfNeeded())
    return;

  String page(FPSTR(kRemoteDashboardHtml));
  page.replace(
      "<div class=\"brand\" aria-label=\"iDryer\" style=\"width:60px;background:transparent;color:#eaf1fa;font-size:14px;letter-spacing:-1px\"><span style=\"color:#5ba9ff\">i</span>Dryer</div>",
      "<img class=\"brand\" src=\"/logo.png\" alt=\"iDryer\">");
  page.replace(
      "</style>",
      R"HTML(
/* Desktop refinement: anchor the interface, use the available canvas, and
   keep the navigation pictographic rather than repeating labels. */
.main{width:100%;max-width:1440px;margin:0 auto;padding:30px 42px}
.brand{object-fit:cover}
.connection{display:none}
.tab{font-size:0;position:relative;line-height:0}
.tab::before{content:"";position:absolute;inset:0;margin:auto;display:block;width:23px;height:23px;background:currentColor}
.tab[data-page=home]::before{-webkit-mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m3 10 9-7 9 7'/%3E%3Cpath d='M5 9v11h14V9'/%3E%3C/svg%3E") center/contain no-repeat;mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m3 10 9-7 9 7'/%3E%3Cpath d='M5 9v11h14V9'/%3E%3C/svg%3E") center/contain no-repeat}
.tab[data-page=heat]::before{-webkit-mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M12 2c3 3 3 6 0 9'/%3E%3Cpath d='M7 5c3 3 3 7 0 11'/%3E%3Cpath d='M17 5c3 3 3 7 0 11'/%3E%3Cpath d='M4 19c4 2 12 2 16 0'/%3E%3C/svg%3E") center/contain no-repeat;mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M12 2c3 3 3 6 0 9'/%3E%3Cpath d='M7 5c3 3 3 7 0 11'/%3E%3Cpath d='M17 5c3 3 3 7 0 11'/%3E%3Cpath d='M4 19c4 2 12 2 16 0'/%3E%3C/svg%3E") center/contain no-repeat}
.tab[data-page=timer]::before{-webkit-mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Ccircle cx='12' cy='12' r='9'/%3E%3Cpath d='M12 7v5l3 2'/%3E%3C/svg%3E") center/contain no-repeat;mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Ccircle cx='12' cy='12' r='9'/%3E%3Cpath d='M12 7v5l3 2'/%3E%3C/svg%3E") center/contain no-repeat}
.tab.settings::before{display:none}
.grid{grid-template-columns:minmax(0,1fr) minmax(0,1fr);align-items:start}
.grid>.card:first-child{grid-column:auto;display:block}
.grid>.card:first-child h2{margin-bottom:13px}
.grid>.card:first-child .details{margin-top:18px;grid-template-columns:repeat(2,minmax(0,1fr))}
.quick{grid-column:auto;align-self:stretch}
.quick .actions{grid-template-columns:repeat(2,minmax(0,1fr));align-content:start}
.footer a{color:inherit;text-decoration:none}.footer a:hover{color:var(--accent)}
@media(max-width:650px){.rail{height:58px;padding:7px;gap:8px}.brand{width:37px;height:37px;border-radius:10px}.tab{width:42px;min-width:42px;height:39px;flex:0 0 42px;padding:0}.tab::before{width:21px;height:21px}.tab.settings{margin:0 0 0 auto;font-size:22px!important}.main{padding:15px}.card{padding:13px}.state{font-size:22px}.details{gap:7px;margin-top:14px}.metric{padding:9px}.metric strong{font-size:18px}.quick .actions{gap:7px}}
@media(max-width:560px){.grid{grid-template-columns:1fr}.quick{grid-column:1/-1}.quick .actions{grid-template-columns:repeat(2,minmax(0,1fr))}}
</style>)HTML");
  page.replace("<button class=\"tab\" data-page=\"home\" aria-selected=\"true\">HOME</button>",
               "<button class=\"tab\" data-page=\"home\" aria-label=\"Dashboard\" title=\"Dashboard\" aria-selected=\"true\">HOME</button>");
  page.replace("<button class=\"tab\" data-page=\"heat\">HEAT</button>",
               "<button class=\"tab\" data-page=\"heat\" aria-label=\"Heat controls\" title=\"Heat controls\">HEAT</button>");
  page.replace("<button class=\"tab\" data-page=\"timer\">TIMER</button>",
               "<button class=\"tab\" data-page=\"timer\" aria-label=\"Timer\" title=\"Timer\">TIMER</button>");
  page.replace(
      "iDryer Remote <span id=\"version\">—</span> · This remote reports the command it sends to iHeater. Current temperatures and safety-chain state are not available on this hardware.",
      "<a href=\"https://github.com/justinh-rahb/iHeater-Remote\" target=\"_blank\" rel=\"noopener\">iHeater Remote <span id=\"version\">—</span></a>");
  s_server.send(200, "text/html", page);
}

void configureRoutes() {
  s_server.serveStatic("/logo.png", LittleFS, "/logo.png");
  s_server.on("/", HTTP_GET, handleRoot);
  s_server.on("/setup", HTTP_GET, handleSetupPage);
  s_server.on("/fw", HTTP_GET, handleFirmwarePage);
  s_server.on("/api/mode", HTTP_POST, handleModePost);
  s_server.on("/api/timer", HTTP_POST, handleTimerPost);
  s_server.on("/api/integration", HTTP_POST, handleIntegrationPost);
  s_server.on("/api/setup", HTTP_POST, handleSetupPost);
  s_server.on("/api/wifi", HTTP_POST, handleWifiPost);
  s_server.on("/api/wifi/clear", HTTP_POST, handleWifiClearPost);
  s_server.on("/api/ota", HTTP_POST, handleOtaPost, handleOtaUpload);
  s_server.on("/api/off", HTTP_POST, []() {
    applyMode(0, "web");
    sendActionStatus();
  });
  s_server.on("/generate_204", HTTP_GET, handleRoot);
  s_server.on("/gen_204", HTTP_GET, handleRoot);
  s_server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  s_server.on("/library/test/success.html", HTTP_GET, handleRoot);
  s_server.on("/ncsi.txt", HTTP_GET, handleRoot);
  s_server.on("/connecttest.txt", HTTP_GET, handleRoot);
  s_server.on("/redirect", HTTP_GET, handleRoot);
  s_server.onNotFound([]() {
    if (s_apMode) {
      handleRoot();
    } else {
      s_server.send(404, "application/json", "{\"error\":\"not found\"}");
    }
  });
}

void startAccessPoint() {
  uint64_t mac = ESP.getEfuseMac();
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "iHeater Remote %04X",
           static_cast<unsigned>((mac >> 32) & 0xFFFF));
  s_apSsid = ssid;

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApGateway, kApSubnet);
  const char *password = IHEATER_AP_PASSWORD;
  if (strlen(password) >= 8) {
    WiFi.softAP(ssid, password);
  } else {
    WiFi.softAP(ssid);
  }
  applyTxPower("remote-ap");
  s_apMode = true;
  s_dnsServer.start(kDnsPort, "*", kApIp);

  Serial.printf("[REMOTE] AP SSID=%s IP=%s\n", ssid,
                WiFi.softAPIP().toString().c_str());
  Serial.println("[REMOTE] Captive portal DNS started");
}

void configureWiFi() {
  WiFi.persistent(false);
  WiFi.setSleep(false);

  String wifiSsid;
  String wifiPassword;
  String credentialSource;
  if (loadWifiCredentials(wifiSsid, wifiPassword, credentialSource)) {
    s_stationSsid = wifiSsid;
    s_wifiCredentialSource = credentialSource;

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    applyTxPower("remote-sta");
    Serial.printf("[REMOTE] Connecting to WiFi SSID=%s source=%s\n",
                  wifiSsid.c_str(), credentialSource.c_str());

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (uint32_t)(millis() - start) < kWifiConnectTimeoutMs) {
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      s_apMode = false;
      Serial.printf("[REMOTE] WiFi connected IP=%s\n",
                    WiFi.localIP().toString().c_str());
      return;
    }

    Serial.println("[REMOTE] WiFi connect timeout, starting AP");
    WiFi.disconnect(true);
  } else {
    s_stationSsid = "";
    s_wifiCredentialSource = "none";
  }

  startAccessPoint();
}

void updateButton() {
  const uint32_t now = millis();
  const bool rawDown = digitalRead(IHEATER_BOOT_BUTTON_PIN) == LOW;

  if (rawDown != s_buttonLastRawDown) {
    s_buttonLastRawDown = rawDown;
    s_buttonLastRawChangeMs = now;
  }

  if ((uint32_t)(now - s_buttonLastRawChangeMs) >= kButtonDebounceMs &&
      rawDown != s_buttonStableDown) {
    s_buttonStableDown = rawDown;
    if (s_buttonStableDown) {
      s_buttonDownAtMs = now;
      s_longPressHandled = false;
    } else if (!s_longPressHandled) {
      if (s_pendingClicks < 255)
        ++s_pendingClicks;
      s_clickDeadlineMs = now + kMultiClickWindowMs;
    }
  }

  if (s_buttonStableDown && !s_longPressHandled &&
      (uint32_t)(now - s_buttonDownAtMs) >= kLongPressMs) {
    s_pendingClicks = 0;
    s_longPressHandled = true;
    applyMode(0, "button:long");
  }

  if (s_pendingClicks && !s_buttonStableDown &&
      (int32_t)(now - s_clickDeadlineMs) >= 0) {
    const uint8_t mode = s_pendingClicks > kMaxMode ? kMaxMode : s_pendingClicks;
    s_pendingClicks = 0;
    applyMode(mode, "button:click");
  }
}

void updateLed() {
  if (!ledEnabled())
    return;

  const uint32_t now = millis();
  if (s_mode == 0) {
    if (s_ledOn)
      writeLed(false);
    return;
  }

  if ((int32_t)(now - s_ledNextAtMs) < 0)
    return;

  if (s_ledOn) {
    writeLed(false);
    ++s_ledFlashesDone;
    if (s_ledFlashesDone >= s_mode) {
      s_ledFlashesDone = 0;
      s_ledNextAtMs = now + kLedPauseMs;
    } else {
      s_ledNextAtMs = now + kLedOffMs;
    }
  } else {
    writeLed(true);
    s_ledNextAtMs = now + kLedOnMs;
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("[REMOTE] iHeater Remote firmware starting");

  if (ledEnabled()) {
    pinMode(IHEATER_STATUS_LED_PIN, OUTPUT);
    writeLed(false);
  }
  pinMode(IHEATER_BOOT_BUTTON_PIN, INPUT_PULLUP);

  s_output.begin();
  applyMode(0, "boot", false); // output off first; the binding is untouched

  if (!LittleFS.begin()) {
    Serial.println("[REMOTE] LittleFS unavailable; logo asset will not load");
  }

  loadDeviceSettings(); // before Wi-Fi: carries the TX power setting
  configureWiFi();
  // After Wi-Fi: a first-boot pairing token needs esp_random() with RF entropy.
  loadRemoteSettings(s_settings);
  s_moonraker.onTarget(onKlipperTarget);
  if (klipperSource())
    s_moonraker.configure(s_settings.moon, s_settings.pairToken.c_str());
  configureRoutes();
  s_server.begin();
  s_statusSocket.begin();
  s_statusSocket.onEvent(onStatusSocketEvent);
  Serial.printf("[REMOTE] Web UI: http://%s/\n",
                localIpString().c_str());
}

void loop() {
  if (s_apMode)
    s_dnsServer.processNextRequest();
  s_server.handleClient();
  s_statusSocket.loop();
  updateButton();
  updateHeatTimer();
  updateLed();
  updateRestart();
  updateWifiSupervisor();
  s_moonraker.loop();
  updateLinkSafety();
  updateHeapLog();
  pushStatusIfChanged();
  delay(2);
}

} // namespace standalone
} // namespace iheaterlink
