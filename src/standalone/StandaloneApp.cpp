#include "standalone/StandaloneApp.h"
#include "standalone/AuthPages.h"
#include "standalone/AuthScript.h"
#include "standalone/AuthStore.h"
#include "standalone/DiagPage.h"
#include "standalone/EventFeed.h"
#include "standalone/MoonrakerLink.h"
#include "standalone/RemoteDashboard.h"
#include "standalone/RemoteSettings.h"
#include "standalone/StatusSocket.h"
#include "standalone/UartLink.h"

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
#include <esp_system.h>
#include <string.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include "controller/RmtOutputAdapter.h"
#include <AuthCrypto.h>
#include <AuthPolicy.h>
#include <AuthState.h>
#include <EventTracker.h>
#include <FirmwareImage.h>
#include <KlipperOverride.h>
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
StatusSocket s_statusSocket(81);

// Headers from each socket's handshake, kept until it connects: login is
// judged from them once, at WStype_CONNECTED. Only the session token is kept
// from the Cookie header (other cookies can make it long). A secret: never
// log it.
struct SocketHandshake {
  bool hasToken = false;
  uint8_t token[policy::kSessionTokenLen] = {};
  String user;
  String groups;
};
SocketHandshake s_socketHandshake[StatusSocket::kMaxClients];
// The adapter's own window is widened to the full pulse range; the target
// range policy (s_range) does the clamping.
RmtOutputConfig remoteOutputConfig() {
  RmtOutputConfig config{};
  config.minTempC = policy::kRangeFloorC;
  config.maxTempC = policy::kRangeCeilC;
  return config;
}

RmtOutputAdapter s_rmt{remoteOutputConfig()};
UartLink s_uart;
OutputPort *s_output = &s_rmt;  // setup() points this at s_uart for OutputKind::Uart
OutputKind s_outputKind = OutputKind::Pulse;
RemoteSettings s_settings;
policy::KlipperOverride s_override;
MoonrakerLink s_moonraker;
// The web login. These tables aren't locked: WebServer handlers run in loop().
AuthSettings s_auth;
policy::SessionTable s_sessions;
policy::NonceTable s_nonces;
policy::LoginLockout s_lockout;
policy::RunningHours s_hours;

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
// A refused OTA upload (an iHeater update is running) must not touch Update.
bool s_otaRefused = false;
policy::TargetRange s_range{policy::kDefaultMinC, policy::kDefaultMaxC};
String s_targetSource = "manual";
// One-shot message from the last setup save, shown on the next Setup page and
// returned by /api/integration.
String s_setupNotice;
// Last loop() pass on which the active printer link was healthy.
uint32_t s_linkHealthyAtMs = 0;
uint32_t s_wifiLastAttemptMs = 0;
int s_txPower = kDefaultTxPower;
int s_wifiRssi = 0;
uint32_t s_wifiRssiAtMs = 0;
uint32_t s_heapLogAtMs = 0;

// The iHeater firmware image, held in RAM from upload until it is flashed. The
// flash job reads the buffer, so it is only freed while flashBusy() is false.
uint8_t *s_ihImage = nullptr;
size_t s_ihImageLen = 0;
policy::ImageInfo s_ihInfo;
String s_ihError;
bool s_ihUploading = false;  // this upload owns s_ihImage
bool s_ihImageReady = false;  // uploaded whole and checked Ok
uint32_t s_ihImageSeq = 0;     // bumped at each accepted upload start
uint32_t s_ihJobImageSeq = 0;  // s_ihImageSeq when the last job started
constexpr const char *kUpdateRunningMessage = "An iHeater update is running.";

// Manual-mode resume record. RTC no-init memory survives a software reset
// (crash, watchdog, OTA, Setup save) but is garbage after a power-on or
// brownout, so it is only trusted after a software reset (resumeValid).
RTC_NOINIT_ATTR policy::ResumeRecord s_resume;
constexpr uint32_t kResumeRefreshMs = 1000;
// Held from the top of setup() until the boot decision, so the early pulse
// boot applyMode(0) cannot overwrite the record before it is read.
bool s_resumeLocked = false;
uint32_t s_resumeRefreshAtMs = 0;
// Unhealthy time of the Klipper link on the UART output, carried across a
// software restart so a crash loop cannot restart the 10-minute link-loss
// grace at every boot. Same trust rule as s_resume (policy::carriedLinkLoss).
RTC_NOINIT_ATTR policy::LinkLossRecord s_linkLoss;
uint32_t s_linkLossRefreshAtMs = 0;
// Set once a Setup save has switched the Output: the restart is pending and no
// heat may be started on the old output in the meantime.
bool s_outputSwitchPending = false;
bool s_outputSwitchRefusalLogged = false;
// UART output, Klipper source: until Moonraker delivers a target (or anything
// else decides one), adopt the target the iHeater is already holding.
bool s_klipperRecoveryPending = false;

bool s_buttonStableDown = false;
bool s_buttonLastRawDown = false;
bool s_longPressHandled = false;  // heat-off step done for this press
bool s_authResetDone = false;
uint8_t s_pendingClicks = 0;
uint32_t s_buttonLastRawChangeMs = 0;
uint32_t s_buttonDownAtMs = 0;
uint32_t s_clickDeadlineMs = 0;

bool s_ledOn = false;
uint8_t s_ledFlashesDone = 0;
uint32_t s_ledNextAtMs = 0;
bool s_ledSolid = false;
uint32_t s_ledSolidUntilMs = 0;

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

uint32_t timerRemainingSeconds();

// Records the current manual mode and timer for a software restart. Quiet
// while the boot decision is pending and once an Output switch is pending (the
// record was invalidated and must stay so).
void writeResumeRecord() {
  if (s_resumeLocked || s_outputSwitchPending)
    return;
  s_resume.mode = s_mode;
  s_resume.timerMinutes = s_timerMinutes;
  s_resume.remainingS = timerRemainingSeconds();
  policy::sealResume(s_resume);
}

// True (and logged once) while an Output switch is pending: heat must not be
// raised on the old output during the restart delay.
bool refuseHeatForOutputSwitch() {
  if (!s_outputSwitchPending)
    return false;
  if (!s_outputSwitchRefusalLogged) {
    s_outputSwitchRefusalLogged = true;
    Serial.println("[REMOTE] output switch pending: heat request refused");
  }
  return true;
}

// An Action event for a target change, unless the policy says it is not worth
// one (the boot decision).
void recordTargetEvent(const char *source, float targetC) {
  char text[policy::kEventTextMax + 1];
  if (policy::targetEventText(source, targetC, text))
    feedRecord(policy::EventKind::Action, "%s", text);
}

void applyIntegrationTarget(float targetTempC, const char *source,
                            const char *targetSource) {
  // Any target decision supersedes the boot read-back of the iHeater's target.
  s_klipperRecoveryPending = false;
  if (targetTempC > 0.0f && refuseHeatForOutputSwitch())
    targetTempC = 0.0f;
  targetTempC = policy::clampTargetC(targetTempC, s_range);
  recordTargetEvent(source, targetTempC);
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
  writeResumeRecord();
  s_output->apply(cmd);
  s_output->forceFrame();
  resetLedPattern();
  if (s_outputKind == OutputKind::Pulse)
    Serial.printf("[REMOTE] %s target=%.1fC pulse=%u\n", source,
                  (double)cmd.targetTempC, s_rmt.getLastPulseCode());
  else
    Serial.printf("[REMOTE] %s target=%.1fC\n", source, (double)cmd.targetTempC);
}

// MoonrakerLink callback: the paired heater_iheater_remote's target (0 = off). The
// link holds the last target through outages; updateLinkSafety() cuts heat
// if the link stays down.
void onKlipperTarget(float targetC) {
  if (!klipperSource())
    return;
  const bool wasActive = s_override.active();
  if (s_override.target(targetC)) {
    if (wasActive && !s_override.active())
      feedRecord(policy::EventKind::Action,
                 "Manual override ended (Klipper changed its target)");
    applyIntegrationTarget(targetC, "klipper", "klipper");
  }
}

// Clears the override, logging its end when one was active.
void resetOverride(const char *reason) {
  const bool wasActive = s_override.active();
  s_override.reset();
  if (wasActive)
    feedRecord(policy::EventKind::Action, "Manual override ended (%s)", reason);
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

// Only user actions (web and button) pass userPress: a manual press while
// following Klipper starts an override and keeps the pairing. Boot, the timer
// and OTA are not presses.
void applyMode(uint8_t mode, const char *source, bool userPress = true) {
  if (mode > kMaxMode)
    mode = kMaxMode;
  s_klipperRecoveryPending = false;
  if (mode != 0 && refuseHeatForOutputSwitch())
    mode = 0;

  if (userPress) {
    const bool wasActive = s_override.active();
    if (s_override.press(klipperSource()) && !wasActive)
      feedRecord(policy::EventKind::Action, "Manual override of Klipper started");
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
  recordTargetEvent(source, cmd.targetTempC);
  s_targetSource = "manual";
  writeResumeRecord();
  s_output->apply(cmd);
  s_output->forceFrame();
  resetLedPattern();

  if (s_outputKind == OutputKind::Pulse)
    Serial.printf("[REMOTE] %s mode=%u target=%.1fC timer=%umin pulse=%u\n",
                  source, mode, (double)cmd.targetTempC, s_timerMinutes,
                  s_rmt.getLastPulseCode());
  else
    Serial.printf("[REMOTE] %s mode=%u target=%.1fC timer=%umin\n", source,
                  mode, (double)cmd.targetTempC, s_timerMinutes);
}

void updateHeatTimer() {
  if (s_mode == 0 || s_timerOffAtMs == 0)
    return;

  if ((int32_t)(millis() - s_timerOffAtMs) >= 0) {
    if (s_override.active())
      applyIntegrationTarget(s_override.end(), "timer: back to klipper",
                             "klipper");
    else
      applyMode(0, "timer", false);
  }
}

// While a Manual timer runs, keep the record's remaining seconds fresh.
void updateResumeRefresh() {
  if (s_mode == 0 || s_timerOffAtMs == 0)
    return;
  const uint32_t now = millis();
  if (!policy::intervalElapsed(now, s_resumeRefreshAtMs, kResumeRefreshMs))
    return;
  s_resumeRefreshAtMs = now;
  writeResumeRecord();
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
  if (ok) {
    s_range = policy::TargetRange{static_cast<uint8_t>(minC),
                                  static_cast<uint8_t>(maxC)};
    s_uart.setSetupRange(s_range);
  }
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
    if (s_linkLoss.magic != 0)
      policy::clearLinkLoss(s_linkLoss);
    return;
  }
  // Keep the unhealthy time in RTC memory while it runs, so it is already there
  // if the next restart is a crash. Pulse mode does not carry it.
  if (s_outputKind != OutputKind::Uart) {
    if (s_linkLoss.magic != 0)
      policy::clearLinkLoss(s_linkLoss);
  } else if (policy::intervalElapsed(now, s_linkLossRefreshAtMs,
                                     kResumeRefreshMs)) {
    s_linkLossRefreshAtMs = now;
    policy::sealLinkLoss(s_linkLoss, now - s_linkHealthyAtMs);
  }
  const bool heatingFromPrinter =
      s_output->getLastCommand().mode == ControllerOutputMode::TargetTemperature &&
      (s_targetSource == "klipper" || s_override.active());
  if (policy::cutHeatForLinkLoss(printerSource, heatingFromPrinter, healthy,
                                 now, s_linkHealthyAtMs,
                                 policy::kLinkLossGraceMs)) {
    s_override.end();
    applyIntegrationTarget(0.0f, "printer link lost 10 min", "link-lost");
  }
}

const char *uartLinkName(policy::LinkStatus status) {
  switch (status) {
    case policy::LinkStatus::Waiting:
      return "waiting";
    case policy::LinkStatus::Up:
      return "up";
    case policy::LinkStatus::Restarting:
      return "restarting";
    case policy::LinkStatus::NoReply:
      return "noreply";
    case policy::LinkStatus::Updating:
      return "updating";
  }
  return "unknown";
}

// Logs each phase change of a flash job, and its outcome. Idle is not logged:
// the task resets the status to it when it takes a new job.
void logUartFlash(const policy::FlashStatus &flash) {
  static policy::FlashPhase s_lastPhase = policy::FlashPhase::Idle;
  static policy::FlashOutcome s_lastOutcome = policy::FlashOutcome::None;
  if (flash.phase != s_lastPhase) {
    s_lastPhase = flash.phase;
    if (flash.phase != policy::FlashPhase::Idle)
      Serial.printf("[UART] flash phase=%s\n", policy::flashPhaseName(flash.phase));
  }
  if (flash.outcome != s_lastOutcome) {
    s_lastOutcome = flash.outcome;
    if (flash.outcome != policy::FlashOutcome::None) {
      Serial.printf("[UART] flash outcome=%s phase=%s retries=%u%s%s\n",
                    policy::flashOutcomeName(flash.outcome), policy::flashPhaseName(flash.failedPhase),
                    static_cast<unsigned>(flash.retries), flash.detail[0] ? " " : "", flash.detail);
      if (flash.outcome == policy::FlashOutcome::Updated)
        feedRecord(policy::EventKind::Info, "iHeater updated");
      else
        feedRecord(policy::EventKind::Error, "iHeater update failed: %s%s%s",
                   policy::flashOutcomeName(flash.outcome), flash.detail[0] ? " " : "",
                   flash.detail);
    }
  }
}

// Logs each change of the UART link status and of a flash job. Runs on
// loopTask against the task's published view, so the poll task itself never
// logs for this.
void updateUartLinkLog() {
  if (s_outputKind != OutputKind::Uart || !s_uart.isEnabled())
    return;
  static bool s_logged = false;
  static policy::LinkStatus s_lastLink = policy::LinkStatus::Waiting;
  const UartLinkView view = s_uart.view();
  logUartFlash(view.flash);
  if (s_logged && view.link == s_lastLink)
    return;
  s_logged = true;
  s_lastLink = view.link;
  Serial.printf("[UART] link=%s\n", uartLinkName(view.link));
}

// Sends the iHeater's readings to the Klipper plugin. Only while the Remote
// follows Klipper over UART; MoonrakerLink decides when a report is due.
void updateKlipperReport() {
  if (s_outputKind != OutputKind::Uart || !klipperSource())
    return;
  const UartLinkView view = s_uart.view();
  policy::ReportInputs in{};
  in.link = view.link;
  in.hasSnapshot = view.hasSnapshot;
  in.state = view.snap.state;
  in.airT10 = view.snap.airT10;
  in.heaterT10 = view.snap.heaterT10;
  in.pwmPermille = view.snap.pwmPermille;
  in.fanOn = view.snap.fanOn;
  in.fault = view.snap.activeFault;
  const auto last = s_uart.getLastCommand();
  in.targetT10 = last.mode == ControllerOutputMode::TargetTemperature
                     ? static_cast<uint16_t>(lroundf(last.targetTempC * 10.0f))
                     : 0;
  s_moonraker.report(in);
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

// Tenths of a degree as text for the Setup page: "45", "62.5".
String tenthsText(int16_t t10) {
  String text(static_cast<int>(t10 / 10));
  if (t10 % 10 != 0) {
    text += '.';
    text += static_cast<char>('0' + (t10 < 0 ? -(t10 % 10) : t10 % 10));
  }
  return text;
}

// The Security card on /setup. It sits in its own form, outside the main setup
// form, and posts to /api/auth/settings and /api/auth/password with fetch.
const char kSecurityCard[] = R"HTML(<form id=sec onsubmit="return false"><section class=card><h2>Security</h2><p class=note id=secMsg role=status></p><h2>Change password</h2><label>New password<input id=secPw type=password minlength=8 maxlength=64 autocomplete=new-password></label><label>Confirm password<input id=secPw2 type=password minlength=8 maxlength=64 autocomplete=new-password></label><button type=button id=secPwGo>Change password</button><p class=note>At least 8 characters. Changing it logs every other device out.</p><h2>Trusted proxies</h2><label class=check><input id=secAuto type=checkbox __AUTO_CHK__>Trust the Moonraker host automatically</label><p class=note>Moonraker host: __AUTO_ADDR__</p><label>Trusted proxy addresses<input id=secProxies value="__SEC_PROXIES__" maxlength=80 placeholder="192.168.1.10, 192.168.1.11" autocomplete=off></label><p class=note>Up to four IPv4 addresses, separated by commas. If nginx runs in Docker, add the bridge gateway address.</p><h2>Header login</h2><label class=check><input id=secHdr type=checkbox __HDR_CHK__>Log in from a proxy header</label><label>User header<input id=secUserH value="__HDR_USER__" maxlength=40 autocomplete=off></label><label>Allowed users (comma separated, blank allows any)<input id=secUsers value="__HDR_USERS__" maxlength=128 autocomplete=off></label><label>Groups header<input id=secGroupH value="__HDR_GRPH__" maxlength=40 autocomplete=off></label><label>Allowed groups (comma separated, blank allows any)<input id=secGroups value="__HDR_GROUPS__" maxlength=128 autocomplete=off></label><p class=note style="color:#ffb347">Every trusted proxy, the automatic Moonraker entry included, can then log anyone in.</p><h2>Proxy-only</h2><label class=check><input id=secOnly type=checkbox __PO_CHK__>Only allow actions through the HTTPS proxy</label><p class=note>Hold BOOT for 10 s to turn this off again. The Remote's own access point, which opens when it can't join Wi-Fi, ignores this setting.</p><button type=button id=secGo>Save security settings</button></section></form><script src="/auth.js"></script><script>(()=>{const $=i=>document.getElementById(i),say=t=>{$('secMsg').textContent=t};
$('secPwGo').onclick=()=>{const a=$('secPw').value;if(a.length<8){say('Use at least 8 characters.');return}if(a!==$('secPw2').value){say("The passwords don't match.");return}
const b=$('secPwGo');b.disabled=true;say('Working...');setTimeout(()=>{IHA.setPassword(a).then(()=>{$('secPw').value='';$('secPw2').value='';say('Password changed. Every other device is logged out.')},e=>say(e.message)).then(()=>{b.disabled=false})},30)};
$('secGo').onclick=()=>{const f=new URLSearchParams();f.set('proxies',$('secProxies').value);if($('secAuto').checked)f.set('autoProxy','1');if($('secHdr').checked)f.set('headerLogin','1');if($('secOnly').checked)f.set('proxyOnly','1');
f.set('userHeader',$('secUserH').value);f.set('allowedUsers',$('secUsers').value);f.set('groupsHeader',$('secGroupH').value);f.set('allowedGroups',$('secGroups').value);
const b=$('secGo');b.disabled=true;say('Saving...');
fetch('/api/auth/settings',{method:'POST',credentials:'same-origin',body:f}).then(r=>r.json().catch(()=>({error:'Unexpected reply ('+r.status+').'})).then(j=>say(j.error?j.error:'Security settings saved.')),()=>say("Couldn't reach the Remote.")).then(()=>{b.disabled=false})}})()</script>)HTML";

void handleSetupPage() {
  if (captiveRedirectIfNeeded())
    return;
  String page = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><meta name=referrer content=same-origin><title>iHeater setup</title><style>:root{color-scheme:dark;--bg:#090d14;--p:#101722;--l:#29384b;--t:#eaf1fa;--m:#91a2b8;--a:#5ba9ff;font-family:system-ui,sans-serif}body{margin:0;background:var(--bg);color:var(--t)}main{max-width:620px;margin:auto;padding:24px 15px}.card{background:var(--p);border:1px solid var(--l);border-radius:12px;padding:16px;margin:14px 0}h1{font-size:23px}h2{font-size:15px;font-weight:650;margin:0 0 4px}label{display:grid;gap:6px;margin:12px 0;color:var(--m);font-size:13px}input,select,button{min-height:43px;border-radius:8px;border:1px solid #38506b;background:#0c131d;color:var(--t);font:inherit;padding:0 10px}button{background:#1d5d99;border-color:#69b4ff;font-weight:700;width:100%;margin-top:8px}.check{display:flex;align-items:center;gap:10px}.check input{min-height:0;width:18px;height:18px;margin:0;padding:0}.note{font-size:13px;line-height:1.45;color:var(--m)}a{color:var(--a)}</style><main><p><a href="/">← Dashboard</a></p><h1>Wi-Fi &amp; printer setup</h1><p class=note>Blank password and API key fields keep their stored values.</p><form method=post action="/api/setup"><section class=card><h2>Wi-Fi</h2><label>Network name<input name=ssid value="__SSID__" maxlength=32></label><label>Password <input name=password type=password placeholder="(unchanged)" maxlength=64></label><label>Transmit power<select name=txPower><option value=34 __TXP_34__>8.5 dBm (default)</option><option value=44 __TXP_44__>11 dBm</option><option value=52 __TXP_52__>13 dBm</option><option value=60 __TXP_60__>15 dBm</option><option value=68 __TXP_68__>17 dBm</option><option value=78 __TXP_78__>19.5 dBm</option></select></label><p class=note>Higher power can help a weak signal; some C3 Super Mini boards fail to connect above 8.5 dBm. Changing it restarts the device.</p><p class=note>Signal: __RSSI__</p></section><section class=card><h2>Control source</h2><label>Bind this heater to<select name=type id=source onchange=showSource()><option value=manual __MANUAL__>Manual controls only</option><option value=moonraker __MOON__>Klipper (Moonraker)</option></select></label><div id=moon><label>Host / IP<input name=host value="__MOON_HOST__" maxlength=63></label><label>Port<input name=port id=port type=number value="__MOON_PORT__" min=1 max=65535></label><label class=check><input name=tls id=tls type=checkbox value=1 __MOON_TLS__>Use TLS (wss://, e.g. through Mainsail on 443)</label><label>API key <input name=apiKey type=password placeholder="(unchanged)" maxlength=64></label></div></section><section class=card><h2>Pairing</h2><label>Pairing token<input name=pairToken value="__PAIR_TOKEN__" maxlength=16 autocomplete=off autocapitalize=characters spellcheck=false></label><label class=check><input name=regenToken type=checkbox value=1>Generate a new token on save</label><p class=note>Put this in your [heater_iheater_remote] section as remote_token: __PAIR_TOKEN__. After a wipe, type your old token here to keep your printer.cfg unchanged.</p></section><section class=card><h2>Output</h2><label>iHeater output<select name=output><option value=pulse __OUT_PULSE__>Pulse on __PULSE_PIN__</option><option value=uart __OUT_UART__>UART on __UART_PINS__</option></select></label><p class=note>Switching restarts the Remote. The iHeater must run the matching build (pulse or UART).</p></section><section class=card><h2>iHeater target range</h2>__RANGE_NOTES__<label>Min target (C)<input name=minTargetC type=number value="__MIN_TARGET__" min=11 max=90></label><label>Max target (C)<input name=maxTargetC type=number value="__MAX_TARGET__" min=11 max=90></label></section><button>Save local setup</button></form></main><script>function showSource(){document.getElementById('moon').style.display=document.getElementById('source').value==='moonraker'?'block':'none'}showSource();document.getElementById('tls').onchange=e=>{let p=document.getElementById('port');if(e.target.checked&&p.value==='7125')p.value='443';else if(!e.target.checked&&p.value==='443')p.value='7125'}</script></html>)HTML";
  page.replace("</form></main>",
               String("</form>") + kSecurityCard + String("<p class=note><a href='/fw'>Firmware update</a> · iHeater Remote v") + VERSION_STR + "</p></main>");
  for (const int txp : {34, 44, 52, 60, 68, 78})
    page.replace(String("__TXP_") + txp + "__", s_txPower == txp ? "selected" : "");
  page.replace("__OUT_PULSE__", s_outputKind == OutputKind::Pulse ? "selected" : "");
  page.replace("__OUT_UART__", s_outputKind == OutputKind::Uart ? "selected" : "");
  // Pin labels come from the build flags so they always match the firmware.
  page.replace("__PULSE_PIN__", String("GPIO") + String((int)IHEATER_TRIGGER_OUTPUT_PIN));
  page.replace("__UART_PINS__", String("GPIO") + String((int)IHEATER_UART_TX_PIN) +
                                    " (TX) / GPIO" + String((int)IHEATER_UART_RX_PIN) + " (RX)");
  if (s_outputKind == OutputKind::Uart) {
    const UartLinkView view = s_uart.view();
    String note = "<p class=note>These values do not change any setting on the iHeater. ";
    if (view.hasLimits)
      note += String("The iHeater accepts ") + tenthsText(view.limits.minTargetT10) + "–" +
              tenthsText(view.limits.maxAirTargetT10) +
              " °C: targets outside it are clamped, and a range that does not overlap it is refused.";
    else
      note += "The iHeater's range has not been read yet; it appears here once the link is up.";
    note += "</p>";
    page.replace("__RANGE_NOTES__", note);
  } else {
    page.replace("__RANGE_NOTES__",
                 "<p class=note>These values do not change any setting on the iHeater. Stock v0.5.0 firmware accepts 45–90 and faults when the air reaches 90.</p><p class=note>Min must not be lower than your iHeater's minimum: lower targets are ignored by the iHeater, which keeps its previous target.</p><p class=note>We recommend a Max no higher than 80 (10 °C below the iHeater's MAX_AIR_TEMP).</p>");
  }
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
  // The Security card: its stored values are escaped and go in last too.
  page.replace("__AUTO_CHK__", s_auth.autoProxy ? "checked" : "");
  page.replace("__HDR_CHK__", s_auth.headerLogin ? "checked" : "");
  page.replace("__PO_CHK__", s_auth.proxyOnly ? "checked" : "");
  String autoText = "none yet";
  if (!klipper) {
    autoText = "not used in Manual mode";
  } else if (s_moonraker.connectedAddress() != 0) {
    char addr[16];
    policy::formatIpv4(s_moonraker.connectedAddress(), addr);
    autoText = addr;
  }
  String proxies;
  for (size_t i = 0; i < s_auth.proxyCount; ++i) {
    char addr[16];
    policy::formatIpv4(s_auth.proxies[i], addr);
    if (i) proxies += ", ";
    proxies += addr;
  }
  page.replace("__AUTO_ADDR__", autoText);
  page.replace("__SEC_PROXIES__", htmlEscape(proxies.c_str()));
  page.replace("__HDR_USER__", htmlEscape(s_auth.userHeader.c_str()));
  page.replace("__HDR_USERS__", htmlEscape(s_auth.allowedUsers.c_str()));
  page.replace("__HDR_GRPH__", htmlEscape(s_auth.groupsHeader.c_str()));
  page.replace("__HDR_GROUPS__", htmlEscape(s_auth.allowedGroups.c_str()));
  if (s_setupNotice.length()) {
    page.replace("<h1>Wi-Fi &amp; printer setup</h1>",
                 "<h1>Wi-Fi &amp; printer setup</h1><p class=note style='color:#ffb347'>" +
                     s_setupNotice + "</p>");
    s_setupNotice = "";
  }
  s_server.send(200, "text/html", page);
}

// The iHeater update card for /fw (UART output only). It follows the status
// WebSocket and posts the image as multipart field "firmware".
const char kIheaterCard[] = R"HTML(<section class=card id=ih data-auth=out><style>#ih[data-auth=out] .ctl{display:none}#ih h2{font-size:16px;margin:0 0 6px}#ih button:disabled{opacity:.45;cursor:default}#ih .row{display:flex;gap:8px}#ih progress{width:100%}</style><h2>iHeater firmware</h2><p class=note id=ihVer>Running: not read yet</p><p class="note warning" id=ihBoot hidden></p><input class=ctl id=ihFile accept=".bin,application/octet-stream" type=file><p class=note id=ihImg>Choose the iHeater UART build .bin.</p><div class="row ctl"><button id=ihGo disabled>Update iHeater</button><button id=ihCancel disabled>Cancel</button><button id=ihRetry disabled>Retry</button></div><progress id=ihBar max=100 value=0 hidden></progress><p class=note id=ihPhase></p><p class="note warning" id=ihReplace hidden>The iHeater's firmware is being replaced. Keep the Remote and the iHeater powered.</p><p class="note warning" id=ihWarn hidden></p><p class=note id=ihMsg></p><script>(()=>{const $=i=>document.getElementById(i),I='/api/iheater/',busyP=['entering','erasing','writing','starting','restarting'];let u={},up=false;function say(id,t,w){$(id).textContent=t||'';$(id).classList.toggle('warning',!!w)}
function show(s){const i=s.iheater||{};u=s.iheaterUpdate||{};if(!s.iheaterUpdate)return;const im=u.image||{},r=u.running,p=u.phase;
say('ihVer',i.fw?'Running: v'+i.fw+', protocol '+i.proto:'Running: not read yet');
if(!up){say('ihImg',im.error||im.line||'Choose the iHeater app build .bin (build_type 2).',im.error)}
$('ihGo').disabled=!u.canUpdate||up;$('ihCancel').disabled=!u.canCancel;$('ihRetry').disabled=!u.canRetry||up;$('ihFile').disabled=!!r||up;
$('ihBar').hidden=!r||p==='cooling'||p==='idle';$('ihBar').value=u.progress||0;
say('ihPhase',r?(p==='cooling'?'Cooling down'+(typeof u.heaterC==='number'?': heater at '+u.heaterC.toFixed(1)+' °C':''):p==='idle'?'Starting…':p[0].toUpperCase()+p.slice(1)+' ('+(u.progress||0)+'%)'):'');
$('ihReplace').hidden=!(r&&['erasing','writing','starting'].includes(p));
const b=r?null:u.bootloader,bt=b?(b.why===2||b.why===1&&!b.timeoutS?"The iHeater is in its bootloader with no valid firmware. Upload the app build and press Update iHeater.":b.why===1?"The iHeater is in its bootloader and returns to its firmware within "+b.timeoutS+" s.":"The iHeater is in its bootloader."):'';
$('ihBoot').textContent=bt;$('ihBoot').hidden=!bt;
const bad=!r&&u.outcome&&u.outcome!=='none'&&u.outcome!=='updated'&&u.outcome!=='cancelled';
{const w=[];if(bad&&u.entryUnconfirmed)w.push("The iHeater didn't confirm the bootloader request.");if(bad&&u.appIntact===false)w.push("The iHeater is in its bootloader with the heater and fan off, waiting for firmware. Press Retry.");$('ihWarn').textContent=w.join(' ');$('ihWarn').hidden=!w.length}
say('ihMsg',r?'':u.message||u.hint||'',bad);
if(!r&&u.detail)$('ihMsg').textContent+=' ('+u.detail+')';
if(bad&&(u.lastError||u.appCheck))$('ihMsg').textContent+=' [bootloader error '+(u.lastError||0)+', app check '+(u.appCheck||0)+']'}
function au(){fetch('/api/auth/me',{cache:'no-store'}).then(r=>r.json()).then(d=>d.loggedIn===true,()=>false).then(v=>{$('ih').dataset.auth=v?'in':'out'})}au();
function post(path,done){const x=new XMLHttpRequest();x.open('POST',I+path);x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(_){}if(x.status===401||x.status===403)au();if(x.status>=300)say('ihMsg',r.error||'Request failed.',1);else if(r.iheaterUpdate)show(r);if(done)done()};x.onerror=()=>say('ihMsg','Request failed.',1);x.send()}
$('ihFile').onchange=()=>{const file=$('ihFile').files[0];if(!file)return;up=true;say('ihImg','Uploading…');$('ihGo').disabled=true;const x=new XMLHttpRequest(),d=new FormData();d.append('firmware',file,file.name);x.open('POST',I+'image');x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(_){}up=false;if(x.status===401||x.status===403)au();if(x.status>=300)say('ihImg',r.error||'Upload failed.',1);else say('ihImg',r.line);$('ihFile').value=''};x.onerror=()=>{up=false;say('ihImg','Upload failed.',1)};x.send(d)};
$('ihGo').onclick=()=>post('update');$('ihRetry').onclick=()=>post('retry');$('ihCancel').onclick=()=>post('cancel');
let ws;function conn(){ws=new WebSocket(location.protocol==='https:'?'wss://'+location.host+'/ws':'ws://'+location.hostname+':81/');ws.onmessage=e=>{try{const m=JSON.parse(e.data);if(m&&typeof m==='object'&&m.t===undefined)show(m)}catch(_){}};ws.onclose=e=>setTimeout(conn,e.code===4001?10000:2000)}conn()})()</script></section>)HTML";

void handleFirmwarePage() {
  if (captiveRedirectIfNeeded())
    return;
  String page = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><meta name=referrer content=same-origin><title>iHeater firmware update</title><style>:root{color-scheme:dark;--bg:#090d14;--p:#101722;--l:#29384b;--t:#eaf1fa;--m:#91a2b8;--a:#5ba9ff;--bad:#ff8893;font-family:system-ui,sans-serif}body{margin:0;background:var(--bg);color:var(--t)}main{max-width:560px;margin:auto;padding:36px 15px}.logo{font-size:24px;font-weight:800;letter-spacing:-1px}.logo i{font-style:normal;color:var(--a)}.card{background:var(--p);border:1px solid var(--l);border-radius:12px;padding:18px;margin:16px 0}h1{font-size:22px;margin:14px 0 7px}.note{color:var(--m);font-size:13px;line-height:1.5}input,button{box-sizing:border-box;width:100%;min-height:45px;border-radius:8px;border:1px solid #38506b;background:#0c131d;color:var(--t);font:inherit;padding:8px;margin:8px 0}button{background:#1d5d99;border-color:#69b4ff;font-weight:700;cursor:pointer}.warning{color:var(--bad)}a{color:var(--a)}</style><main><p><a href="/">← Dashboard</a></p><div class=logo><i>i</i>Heater</div><h1>Firmware update</h1><p class=note>Installed: iHeater Remote v__VERSION__</p><section class=card><p class="note warning"><strong>__HEATNOTE__</strong> Do not disconnect power while the upload is in progress.</p><input id=file accept=".bin,application/octet-stream" type=file><button id=install>Install update</button><p class=note id=msg></p></section>__IHCARD__</main><script>const f=document.getElementById('file'),m=document.getElementById('msg');document.getElementById('install').onclick=()=>{let file=f.files[0];if(!file){m.textContent='Choose a firmware .bin first.';return}let x=new XMLHttpRequest(),d=new FormData();d.append('firmware',file,file.name);x.open('POST','/api/ota');x.upload.onprogress=e=>m.textContent=e.lengthComputable?'Uploading '+Math.round(e.loaded/e.total*100)+'%':'Uploading…';x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(_){}if(x.status>=200&&x.status<300){m.textContent='Update written. Rebooting…';setTimeout(waitForDevice,1500)}else m.textContent=r.error||'Update failed.'};x.onerror=()=>m.textContent='Update failed.';x.send(d)};function waitForDevice(){let end=Date.now()+120000,t=setInterval(async()=>{try{let r=await fetch('/',{cache:'no-store'});if(r.ok){clearInterval(t);location.href='/'}}catch(_){}if(Date.now()>end){clearInterval(t);m.textContent='The device is restarting. Reopen the dashboard when it is back online.'}},1000)}</script></main>)HTML";
  page.replace("__VERSION__", VERSION_STR);
  page.replace("__IHCARD__", s_outputKind == OutputKind::Uart ? kIheaterCard : "");
  page.replace("__HEATNOTE__",
               s_outputKind == OutputKind::Uart
                   ? "Heating continues: the iHeater holds its target while the Remote restarts."
                   : "Heating is stopped before the update is written.");
  s_server.send(200, "text/html", page);
}

// A temperature in tenths as degrees C, or null for "no reading".
void setTempC(JsonObject obj, const char *key, int16_t t10) {
  if (t10 == policy::kNoReading)
    obj[key] = static_cast<const char *>(nullptr);
  else
    obj[key] = t10 / 10.0f;
}

const char *uartClearName(policy::ClearPhase phase) {
  switch (phase) {
    case policy::ClearPhase::Idle:
      return "idle";
    case policy::ClearPhase::AwaitRestart:
      return "waiting-restart";
    case policy::ClearPhase::Blocked:
      return "blocked";
  }
  return "idle";
}

// Frees the image and forgets its check. Callers make sure no job holds it.
void freeIhImage() {
  free(s_ihImage);
  s_ihImage = nullptr;
  s_ihImageLen = 0;
  s_ihInfo = policy::ImageInfo{};
  s_ihUploading = false;
  s_ihImageReady = false;
}

// What the Update iHeater button would do right now.
policy::FlashRefusal ihStartDecision(const UartLinkView &view, bool retry) {
  const auto last = s_output->getLastCommand();
  policy::FlashStartInputs in;
  in.uartOutput = s_outputKind == OutputKind::Uart && s_uart.isEnabled();
  in.imageOk = s_ihImageReady;
  in.heatWanted = policy::heatWanted(last.mode == ControllerOutputMode::TargetTemperature,
                                     last.targetTempC);
  in.jobRunning = s_uart.flashBusy();
  // A restart already scheduled (OTA, setup save, output switch) would cut the job off.
  in.restartPending = s_restartAtMs != 0 || s_outputSwitchPending;
  in.link = view.link;
  in.inBootloader = view.inBootloader;
  in.bootloaderWhy = view.boot.why;
  in.bootloaderTimeoutS = view.boot.timeoutS;
  in.lastOutcome = view.flash.outcome;
  return policy::decideFlashStart(in, retry);
}

// A cancel is honoured until the job sends the erase: while it is still
// starting (Idle), cooling, or entering the bootloader.
bool ihCanCancel(policy::FlashPhase phase) {
  return phase == policy::FlashPhase::Idle || phase == policy::FlashPhase::Cooling ||
         phase == policy::FlashPhase::Entering;
}

// The iHeater update card's state. While a job is only queued, the view still
// holds the previous job's result, so the job fields stay empty until it runs.
void fillUpdateJson(JsonObject out) {
  const UartLinkView view = s_uart.view();
  const bool busy = s_uart.flashBusy();
  const bool stale = busy && !view.flashRunning;
  const policy::FlashStatus fs = stale ? policy::FlashStatus{} : view.flash;
  out["phase"] = policy::flashPhaseName(fs.phase);
  out["outcome"] = policy::flashOutcomeName(fs.outcome);
  if (fs.phase == policy::FlashPhase::Finished)
    out["message"] = policy::flashStatusMessage(fs);
  out["failedPhase"] = policy::flashPhaseName(fs.failedPhase);
  out["detail"] = String(fs.detail);  // a const char array would be stored by pointer; fs dies on return
  out["progress"] = static_cast<int>(fs.progressPermille / 10);
  setTempC(out, "heaterC", fs.heaterT10);
  out["entryUnconfirmed"] = fs.entryUnconfirmed;
  out["retries"] = fs.retries;
  out["lastError"] = fs.lastError;
  out["appCheck"] = fs.appCheck;
  out["appIntact"] = policy::applicationIntact(fs);
  if (view.inBootloader) {
    JsonObject boot = out.createNestedObject("bootloader");
    boot["why"] = view.boot.why;
    boot["appCheck"] = view.boot.appCheck;
    boot["timeoutS"] = view.boot.timeoutS;
  }
  out["running"] = busy;
  out["canCancel"] = busy && !stale && ihCanCancel(fs.phase);
  const policy::FlashRefusal refusal = ihStartDecision(view, false);
  out["canUpdate"] = !busy && s_ihImageReady && refusal == policy::FlashRefusal::None;
  out["canRetry"] = !busy && s_ihImageReady && policy::retryAllowed(fs.outcome) &&
                    ihStartDecision(view, true) != policy::FlashRefusal::RestartPending;
  if (!busy && s_ihImageReady && refusal != policy::FlashRefusal::None)
    out["hint"] = policy::flashRefusalMessage(refusal);
  JsonObject image = out.createNestedObject("image");
  if (s_ihImageReady) {
    char line[160];
    policy::imageSummary(s_ihInfo, line, sizeof(line));
    image["loaded"] = true;
    image["size"] = s_ihImageLen;
    image["line"] = line;  // copied into the document
  } else if (s_ihError.length()) {
    image["error"] = s_ihError;
  } else {
    image["loaded"] = false;
  }
}

// The raw register fields for the /diag register table, each keyed by its
// C++ field name: the input snapshot, the settings once read, and the link
// bootloader's own fields while it runs. The bootloader replaces the
// snapshot, so "regs" appears when any of the three is known. Keys are
// string literals, stored by pointer.
void fillRegsJson(JsonObject iheater, const UartLinkView &view) {
  if (!view.hasSnapshot && !view.hasLimits && !view.inBootloader)
    return;
  JsonObject r = iheater.createNestedObject("regs");
  if (view.hasSnapshot) {
    const policy::IheaterSnapshot &s = view.snap;
    r["proto"] = s.proto;
    r["fwVersion"] = s.fwVersion;
    r["fwPatch"] = s.fwPatch;
    r["state"] = static_cast<int>(s.state);
    r["activeFault"] = s.activeFault;
    r["clearableNow"] = s.clearableNow ? 1 : 0;
    r["clearAllowMask"] = s.clearAllowMask;
    r["clearsLeft"] = s.clearsLeft;
    r["lastClearResult"] = s.lastClearResult;
    r["airT10"] = s.airT10;
    r["heaterT10"] = s.heaterT10;
    r["auxT10"] = s.auxT10;
    r["airTargetT10"] = s.airTargetT10;
    r["heaterSetT10"] = s.heaterSetT10;
    r["pwmPermille"] = s.pwmPermille;
    r["fanOn"] = s.fanOn ? 1 : 0;
    r["uptimeS"] = s.uptimeS;
    r["linkLossCount"] = s.linkLossCount;
    r["holdRemainingS"] = s.holdRemainingS;
    r["resetCause"] = s.resetCause;
    r["lastFault"] = s.lastFault;
  }
  if (view.hasLimits) {
    const policy::IheaterLimits &l = view.limits;
    r["maxHeaterT10"] = l.maxHeaterT10;
    r["maxAirT10"] = l.maxAirT10;
    r["minTargetT10"] = l.minTargetT10;
    r["maxAirTargetT10"] = l.maxAirTargetT10;
    r["linkHoldS"] = l.linkHoldS;
    r["deltaT10"] = l.deltaT10;
    r["fanOnT10"] = l.fanOnT10;
    r["fanOffT10"] = l.fanOffT10;
    r["fanOnActiveT10"] = l.fanOnActiveT10;
    r["clearMarginT10"] = l.clearMarginT10;
  }
  if (view.inBootloader) {
    r["why"] = view.boot.why;
    r["appCheck"] = view.boot.appCheck;
    r["timeoutS"] = view.boot.timeoutS;
  }
}

// The iHeater's live state for the dashboard. The state fields appear once a
// snapshot exists and the range fields once the settings have been read.
void fillIheaterJson(JsonObject out) {
  const UartLinkView view = s_uart.view();
  out["link"] = policy::linkStatusName(view.link);
  // Link and report diagnostics for bench checks; the dashboard shows them
  // behind the link details toggle.
  JsonObject diag = out.createNestedObject("diag");
  diag["uptimeMs"] = millis();
  diag["firstOkMs"] = view.firstOkMs;
  diag["timeouts"] = view.timeouts;
  diag["badReplies"] = view.badReplies;
  diag["stackFree"] = s_uart.stackHighWaterBytes();
  const policy::ReportScheduler &reports = s_moonraker.reports();
  diag["reportsSent"] = reports.sent();
  diag["reportsRefused"] = reports.refused();
  diag["reportsFailed"] = reports.failed();
  diag["plugin"] = policy::pluginStateName(reports.plugin());
  if (view.hasSnapshot) {
    const policy::IheaterSnapshot &s = view.snap;
    out["state"] = policy::iheaterStateName(s.state);
    setTempC(out, "airC", s.airT10);
    setTempC(out, "heaterC", s.heaterT10);
    setTempC(out, "auxC", s.auxT10);
    setTempC(out, "targetC", s.airTargetT10);
    setTempC(out, "setpointC", s.heaterSetT10);
    out["pwmPct"] = static_cast<int>(s.pwmPermille) / 10.0f;
    out["fan"] = s.fanOn;
    out["holdS"] = s.holdRemainingS;
    out["fault"] = s.activeFault;
    out["faultName"] = policy::faultName(s.activeFault);
    out["clearable"] = s.clearableNow;
    out["allowed"] = s.activeFault < 16 && ((s.clearAllowMask >> s.activeFault) & 1u) != 0;
    out["clearsLeft"] = s.clearsLeft;
    out["lastClearResult"] = s.lastClearResult;
    out["clear"] = uartClearName(view.clear);
    char fw[24];
    snprintf(fw, sizeof(fw), "%u.%u.%u", static_cast<unsigned>(s.fwVersion >> 8),
             static_cast<unsigned>(s.fwVersion & 0xFF), static_cast<unsigned>(s.fwPatch));
    out["fw"] = fw;  // copied into the document
    out["proto"] = s.proto;
    out["protoOk"] = view.protocolSupported;
  }
  if (view.hasLimits) {
    setTempC(out, "minC", view.limits.minTargetT10);
    setTempC(out, "maxC", view.limits.maxAirTargetT10);
    out["rangeEmpty"] = view.rangeEmpty;
  }
  fillRegsJson(out, view);
}

String statusJson() {
  updateHeatTimer();

  // Static so the 4 KB document stays off the 8 KB loopTask stack; every
  // caller (HTTP handlers, the WebSocket push) runs on that one task.
  static StaticJsonDocument<4096> doc;
  doc.clear();
  doc["mode"] = s_mode;
  const auto output = s_output->getLastCommand();
  doc["targetTempC"] = output.targetTempC;
  doc["heating"] = output.mode == ControllerOutputMode::TargetTemperature;
  doc["timerMinutes"] = s_timerMinutes;
  doc["timerActive"] = s_mode != 0 && s_timerOffAtMs != 0;
  doc["timerRemainingSeconds"] = timerRemainingSeconds();
  if (s_outputKind == OutputKind::Pulse)
    doc["pulseCode"] = s_rmt.getLastPulseCode();
  doc["output"] = s_outputKind == OutputKind::Uart ? "uart" : "pulse";
  doc["minTargetC"] = s_range.minC;
  doc["maxTargetC"] = s_range.maxC;
  doc["targetSource"] = s_targetSource;
  doc["rmtEnabled"] = s_output->isEnabled();
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
  doc["override"] = s_override.active();
  if (s_override.hasKlipperTarget())
    doc["klipperTargetC"] = s_override.klipperTargetC();
  else
    doc["klipperTargetC"] = nullptr;
  if (s_outputKind == OutputKind::Uart) {
    fillIheaterJson(doc.createNestedObject("iheater"));
    fillUpdateJson(doc.createNestedObject("iheaterUpdate"));
  }
  static bool overflowLogged = false;
  if (doc.overflowed() && !overflowLogged) {
    overflowLogged = true;
    Serial.println("[REMOTE] status JSON overflowed its document; some fields are missing");
  }

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
  const bool changed = force || status != lastStatus;
  s_statusSocket.broadcastStatus(status, changed);
  if (changed) lastStatus = status;
}

bool socketLoggedIn(uint8_t client);

// Login header values longer than this are not a user name or groups.
constexpr size_t kMaxSocketHeaderBytes = 512;

void onStatusSocketHeader(uint8_t client, const String &name, const String &value) {
  if (client >= StatusSocket::kMaxClients) return;
  SocketHandshake &h = s_socketHandshake[client];
  if (name.equalsIgnoreCase("Cookie")) {
    uint8_t token[policy::kSessionTokenLen];
    if (policy::sessionTokenFromCookie(value.c_str(), token)) {
      memcpy(h.token, token, sizeof token);
      h.hasToken = true;
    }
    memset(token, 0, sizeof token);
    return;
  }
  if (value.length() > kMaxSocketHeaderBytes) return;
  if (s_auth.userHeader.length() > 0 && name.equalsIgnoreCase(s_auth.userHeader))
    h.user = value;
  else if (s_auth.groupsHeader.length() > 0 && name.equalsIgnoreCase(s_auth.groupsHeader))
    h.groups = value;
}

void onStatusSocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t len) {
  if (type == WStype_CONNECTED) {
    const bool loggedIn = socketLoggedIn(client);
    if (client < StatusSocket::kMaxClients) s_socketHandshake[client] = SocketHandshake{};
    // Closed to keep a slot free for a login: it gets nothing.
    if (!s_statusSocket.admit(client, loggedIn)) return;
    s_statusSocket.sendStatus(client, statusJson());
  } else if (type == WStype_DISCONNECTED) {
    if (client < StatusSocket::kMaxClients) s_socketHandshake[client] = SocketHandshake{};
  } else if (!s_statusSocket.clientIsOpen(client)) {
    return;  // a client being closed: its messages are ignored
  }
  feedOnSocketEvent(client, type, payload, len);
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

  writeResumeRecord();
  if (s_timerMinutes > 0)
    feedRecord(policy::EventKind::Action, "Timer set to %u min", static_cast<unsigned>(s_timerMinutes));
  else
    feedRecord(policy::EventKind::Action, "Timer off");
  Serial.printf("[REMOTE] web timer=%umin\n", s_timerMinutes);
  sendActionStatus();
}

bool applySetupForm(String &error) {
  const String txPower = s_server.arg("txPower");
  if (txPower.length() && !policy::validTxPower(txPower.toInt())) {
    error = "Invalid transmit power.";
    return false;
  }

  const String outputArg = s_server.arg("output");
  OutputKind nextOutput = s_outputKind;
  if (outputArg.length()) {
    if (outputArg == "pulse") {
      nextOutput = OutputKind::Pulse;
    } else if (outputArg == "uart") {
      nextOutput = OutputKind::Uart;
    } else {
      error = "Unknown output.";
      return false;
    }
  }

  const String type = s_server.arg("type");
  RemoteSettings next = s_settings;
  const bool wasKlipper = klipperSource();
  bool endpointChanged = false;
  bool apiKeyCleared = false;
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
  // With the iHeater's range known, a range that does not overlap it would
  // leave heating permanently off, so refuse it.
  if (s_outputKind == OutputKind::Uart) {
    const UartLinkView view = s_uart.view();
    policy::RangeT10 effective{};
    if (view.hasLimits &&
        !policy::effectiveRangeT10(
            policy::TargetRange{static_cast<uint8_t>(minC), static_cast<uint8_t>(maxC)},
            view.limits.minTargetT10, view.limits.maxAirTargetT10, effective)) {
      error = String("Range must overlap the iHeater's ") +
              tenthsText(view.limits.minTargetT10) + "–" +
              tenthsText(view.limits.maxAirTargetT10) + " °C.";
      return false;
    }
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
    // Secrets are write-only: a blank field keeps the stored key, but only
    // for the endpoint it was entered for. Otherwise a LAN POST naming a new
    // host would make the Remote send the key to that host, so it is cleared.
    endpointChanged = next.moon.host != s_settings.moon.host ||
                      next.moon.port != s_settings.moon.port ||
                      next.moon.tls != s_settings.moon.tls;
    const String apiKey = s_server.arg("apiKey");
    if (apiKey.length()) {
      next.moon.apiKey = apiKey;
    } else if (!policy::keepStoredApiKey(endpointChanged, true) &&
               !next.moon.apiKey.isEmpty()) {
      next.moon.apiKey = "";
      apiKeyCleared = true;
    }
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
  s_setupNotice = apiKeyCleared
      ? "API key cleared because the host, port or TLS changed: re-enter it if this Moonraker needs one."
      : "";
  // Apply the saved source live now, so a later failed save cannot leave the
  // output or the link following a source that is no longer stored.
  // Pulse keeps today's rule (stop on any save that is not a Klipper no-op);
  // the UART output leaves the target alone when nothing that matters changed.
  const bool bindingChanged =
      endpointChanged || next.pairToken != s_settings.pairToken;
  const bool stopHeat = policy::setupSaveStopsHeat(
      s_outputKind == OutputKind::Uart, wasKlipper, klipperSource(),
      bindingChanged, nextOutput != s_outputKind);
  // An override survives a save that changes neither the source nor the
  // binding; otherwise Klipper's target must not be held against the new one.
  if (stopHeat || bindingChanged || wasKlipper != klipperSource())
    resetOverride("setup changed");
  if (klipperSource()) {
    // Heat from before the change (manual, or another printer or pairing)
    // must not be held: Klipper's own target replaces it once subscribed.
    if (stopHeat)
      applyIntegrationTarget(0.0f, "binding changed", "klipper");
    s_moonraker.configure(s_settings.moon, s_settings.pairToken.c_str());
  } else {
    s_moonraker.stop();
    if (stopHeat)
      applyIntegrationTarget(0.0f, "binding disabled", "manual");
  }
  if (next.pairToken != s_settings.pairToken) {
    if (!savePairToken(next.pairToken.c_str())) {
      error = "Could not save the pairing token.";
      return false;
    }
    s_settings.pairToken = next.pairToken;
    Serial.println("[REMOTE] pairing token changed");
    if (klipperSource())  // follow the link with the newly saved token
      s_moonraker.configure(s_settings.moon, s_settings.pairToken.c_str());
  }
  if ((minC != s_range.minC || maxC != s_range.maxC) && !saveRange(minC, maxC)) {
    error = "Could not save the target range.";
    return false;
  }
  // A lowered maximum must show in the status target of a running Manual
  // mode: apply it again, keeping the timer and the printer binding.
  if (!klipperSource() && s_mode != 0) {
    const uint32_t timerOffAtMs = s_timerOffAtMs;
    applyMode(s_mode, "range", false);
    s_timerOffAtMs = timerOffAtMs;
    writeResumeRecord();
  }
  if (txPower.length() && txPower.toInt() != s_txPower) {
    if (!saveTxPower(txPower.toInt())) {
      error = "Could not save transmit power.";
      return false;
    }
    scheduleRestart();
  }
  if (nextOutput != s_outputKind) {
    // Stop heating under the old output before the restart switches it.
    const String keepSource = s_targetSource;
    applyIntegrationTarget(0.0f, "output changed", keepSource.c_str());
    if (!saveOutputKind(nextOutput)) {
      error = "Could not save the output.";
      return false;
    }
    if (s_setupNotice.length())
      s_setupNotice += " ";
    s_setupNotice += "Restarting to switch the output.";
    // The record belongs to the old output, and nothing may heat until the
    // restart (a Klipper target or a Manual POST could still arrive).
    s_resume.magic = 0;
    s_outputSwitchPending = true;
    scheduleRestart();
  }
  return true;
}

void handleOverrideEnd() {
  if (s_override.active())
    applyIntegrationTarget(s_override.end(), "back to klipper", "klipper");
  sendActionStatus();
}

void handleIntegrationPost() {
  if (s_uart.flashBusy()) {
    s_server.send(409, "application/json", String("{\"error\":\"") + kUpdateRunningMessage + "\"}");
    return;
  }
  String error;
  if (!applySetupForm(error)) {
    s_server.send(400, "application/json", String("{\"error\":\"") + error + "\"}");
    return;
  }
  String status = statusJson();
  if (s_setupNotice.length() && status.endsWith("}")) {
    // The notice text is fixed ASCII without quotes, so it needs no escaping.
    status.remove(status.length() - 1);
    status += ",\"notice\":\"" + s_setupNotice + "\"}";
  }
  s_setupNotice = "";
  s_server.send(200, "application/json", status);
}

void sendJsonError(int code, const char *message) {
  StaticJsonDocument<384> doc;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

void handleSetupPost() {
  if (s_uart.flashBusy()) {
    s_server.send(409, "text/plain", kUpdateRunningMessage);
    return;
  }
  String error;
  if (!applySetupForm(error)) {
    s_server.send(400, "text/plain", error);
    return;
  }
  feedRecord(policy::EventKind::Action, "Setup saved");

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
  if (s_uart.flashBusy()) {
    sendJsonError(409, kUpdateRunningMessage);
    return;
  }
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
  feedRecord(policy::EventKind::Action, "Wi-Fi settings saved, restarting");
  Serial.printf("[REMOTE] saved WiFi SSID=%s, restarting\n", ssid.c_str());
  sendActionStatus();
}

void handleWifiClearPost() {
  if (s_uart.flashBusy()) {
    sendJsonError(409, kUpdateRunningMessage);
    return;
  }
  if (!clearStoredWifiCredentials()) {
    s_server.send(500, "application/json", "{\"error\":\"clear failed\"}");
    return;
  }

  s_stationSsid = "";
  s_wifiCredentialSource = "none";
  scheduleRestart();
  feedRecord(policy::EventKind::Action, "Wi-Fi forgotten, restarting");
  Serial.println("[REMOTE] cleared stored WiFi credentials, restarting");
  sendActionStatus();
}

void handleOtaPost() {
  if (s_otaRefused) {
    sendJsonError(409, kUpdateRunningMessage);
    return;
  }
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
    s_otaOk = false;
    s_otaBytesWritten = 0;
    s_otaRefused = s_uart.flashBusy();
    if (s_otaRefused) {
      // The Remote must not restart under a running iHeater update.
      s_otaError = kUpdateRunningMessage;
      return;
    }
    s_otaActive = true;
    s_otaError = "";

    // Pulse stops heat before the image is written. The UART output keeps its
    // target: the iHeater holds it while the Remote restarts, and the boot
    // recovery (Klipper read-back, Manual RTC record) picks it up again.
    if (s_outputKind == OutputKind::Pulse)
      applyMode(0, "ota:start", false);
    feedRecord(policy::EventKind::Action, "Remote update started");
    Serial.printf("[REMOTE] OTA upload start filename=%s\n",
                  upload.filename.c_str());

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      s_otaError = Update.errorString();
      Serial.printf("[REMOTE] OTA begin failed: %s\n", s_otaError.c_str());
      feedRecord(policy::EventKind::Error, "Remote update failed: %s", s_otaError.c_str());
    }
  } else if (s_otaRefused) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError()) {
      const size_t written = Update.write(upload.buf, upload.currentSize);
      s_otaBytesWritten += written;
      if (written != upload.currentSize) {
        s_otaError = Update.errorString();
        Serial.printf("[REMOTE] OTA write failed: %s\n", s_otaError.c_str());
        feedRecord(policy::EventKind::Error, "Remote update failed: %s", s_otaError.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.hasError() && Update.end(true)) {
      s_otaOk = true;
      s_otaError = "";
      Serial.printf("[REMOTE] OTA upload complete bytes=%u\n",
                    (unsigned)s_otaBytesWritten);
      feedRecord(policy::EventKind::Info, "Remote updated, restarting");
    } else {
      s_otaOk = false;
      s_otaError = Update.errorString();
      Serial.printf("[REMOTE] OTA end failed: %s\n", s_otaError.c_str());
      feedRecord(policy::EventKind::Error, "Remote update failed: %s", s_otaError.c_str());
    }
    s_otaActive = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_otaActive = false;
    s_otaOk = false;
    s_otaError = "upload aborted";
    Serial.println("[REMOTE] OTA upload aborted");
    feedRecord(policy::EventKind::Error, "Remote update failed: %s", s_otaError.c_str());
  }
}

// The image upload. A refused upload (busy, no memory) copies nothing and never
// touches the buffer, which a running job may be reading.
void handleIheaterImageUpload() {
  HTTPUpload &upload = s_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    s_ihUploading = false;
    if (s_uart.flashBusy()) {
      s_ihError = kUpdateRunningMessage;
      return;
    }
    freeIhImage();
    s_ihError = "";
    ++s_ihImageSeq;
    s_ihImage = static_cast<uint8_t *>(malloc(policy::kMaxImageBytes));
    if (!s_ihImage) {
      s_ihError = "Not enough memory for the image.";
      return;
    }
    s_ihUploading = true;
  } else if (!s_ihUploading) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (s_ihError.length())
      return;
    if (s_ihImageLen + upload.currentSize > policy::kMaxImageBytes) {
      s_ihError = policy::imageCheckMessage(policy::ImageCheck::TooLarge);
      return;
    }
    memcpy(s_ihImage + s_ihImageLen, upload.buf, upload.currentSize);
    s_ihImageLen += upload.currentSize;
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!s_ihError.length()) {
      s_ihInfo = policy::checkImage(s_ihImage, s_ihImageLen);
      if (s_ihInfo.result == policy::ImageCheck::Ok)
        s_ihImageReady = true;
      else {
        char detail[160];
        policy::imageCheckDetail(s_ihInfo, detail, sizeof(detail));
        s_ihError = detail;
      }
    }
    if (!s_ihImageReady)
      freeIhImage();
    s_ihUploading = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    freeIhImage();
    s_ihError = "Upload aborted.";
  }
}

void handleIheaterImagePost() {
  if (s_ihError.length() || !s_ihImageReady) {
    sendJsonError(400, s_ihError.length() ? s_ihError.c_str() : "No file received.");
    return;
  }
  char line[160];
  policy::imageSummary(s_ihInfo, line, sizeof(line));
  StaticJsonDocument<384> doc;
  doc["ok"] = true;
  doc["line"] = line;
  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

void handleIheaterStart(bool retry) {
  const UartLinkView view = s_uart.view();
  const policy::FlashRefusal refusal = ihStartDecision(view, retry);
  if (refusal != policy::FlashRefusal::None) {
    sendJsonError(409, policy::flashRefusalMessage(refusal));
    return;
  }
  if (!s_uart.startFlash(s_ihImage, s_ihImageLen)) {
    sendJsonError(409, policy::flashRefusalMessage(policy::FlashRefusal::JobRunning));
    return;
  }
  s_ihJobImageSeq = s_ihImageSeq;
  feedRecord(policy::EventKind::Action, "iHeater update started");
  Serial.printf("[REMOTE] iHeater update queued (%s)\n", retry ? "retry" : "update");
  sendActionStatus();
}

void handleIheaterCancel() {
  if (!s_uart.flashBusy() || !ihCanCancel(s_uart.view().flash.phase)) {
    sendJsonError(409, "Cancel is only possible until the erase starts.");
    return;
  }
  s_uart.cancelFlash();
  sendActionStatus();
}

void handleIheaterDiscard() {
  if (s_uart.flashBusy()) {
    sendJsonError(409, kUpdateRunningMessage);
    return;
  }
  freeIhImage();
  s_ihError = "";
  sendActionStatus();
}

// Once a job has put the image on the iHeater, the buffer is not needed. A
// newer upload (different sequence) is never freed here.
void updateIheaterImageHousekeeping() {
  static bool s_wasBusy = false;
  const bool busy = s_uart.flashBusy();
  if (s_wasBusy && !busy && s_ihImage && !s_ihUploading && s_ihImageSeq == s_ihJobImageSeq) {
    const policy::FlashOutcome outcome = s_uart.view().flash.outcome;
    if (outcome == policy::FlashOutcome::Updated) {
      freeIhImage();
      s_ihError = "";
    }
  }
  s_wasBusy = busy;
}

// ---- Web login: the request gate, the login endpoints and the pages ----

constexpr const char *kSessionCookie = "iheater_session";
constexpr const char *kSessionCookieAttrs = "; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000";

// IPv4 only. A v4-mapped peer already arrives as IPv4 from remoteIP(). Any
// other IPv6 peer is 0, which isTrustedProxy() never trusts; reading its first
// four bytes instead would let a crafted IPv6 address equal a trusted proxy.
uint32_t ipValue(const IPAddress &ip) {
  if (ip.type() != IPv4)
    return 0;
  return (static_cast<uint32_t>(ip[0]) << 24) | (static_cast<uint32_t>(ip[1]) << 16) |
         (static_cast<uint32_t>(ip[2]) << 8) | static_cast<uint32_t>(ip[3]);
}

String ipText(uint32_t ip) {
  char text[16];
  policy::formatIpv4(ip, text);
  return String(text);
}

struct RequestAuth {
  policy::RequestClass cls;
  policy::AuthVerdict verdict = policy::AuthVerdict::NeedLogin;  // refuse until computed
  policy::AuthMethod method;
  bool viaHttpsProxy;
  uint32_t client;
  String user;
};

// The request the gate last let through; the login handlers read it.
RequestAuth s_req{};
// The upload routes decide once, at UPLOAD_FILE_START. s_uploadRoute names the
// route that computed s_uploadAuth, and is null when no upload is pending.
RequestAuth s_uploadAuth{};
const char *s_uploadRoute = nullptr;

bool peerTrusted(uint32_t peer) {
  // The automatic entry is the Moonraker host, and only while following Klipper.
  const uint32_t automatic =
      (klipperSource() && s_auth.autoProxy) ? s_moonraker.connectedAddress() : 0;
  return policy::isTrustedProxy(peer, s_auth.proxies, s_auth.proxyCount, automatic);
}

bool sessionTokenValid(const uint8_t token[policy::kSessionTokenLen]) {
  const policy::SessionTable::Check check = s_sessions.check(token, s_hours.hours());
  if (check == policy::SessionTable::Check::Expired) saveSessions(s_sessions);
  return check == policy::SessionTable::Check::Valid;
}

bool sessionCookieValid(const char *cookie) {
  uint8_t token[policy::kSessionTokenLen];
  if (!policy::sessionTokenFromCookie(cookie, token)) return false;
  const bool valid = sessionTokenValid(token);
  memset(token, 0, sizeof token);
  return valid;
}

// The socket counts as logged in by the same rules as an HTTP request: a
// valid session cookie, or an accepted proxy login from a trusted peer. With
// no password and no header login there is no login to tell pages apart, so
// every socket counts.
bool socketLoggedIn(uint8_t client) {
  if (!s_auth.passwordSet && !s_auth.headerLogin) return true;
  if (client >= StatusSocket::kMaxClients) return false;
  const SocketHandshake &h = s_socketHandshake[client];
  if (h.hasToken && sessionTokenValid(h.token)) return true;
  const bool trusted = peerTrusted(ipValue(s_statusSocket.remoteIP(client)));
  if (!trusted || !s_auth.headerLogin) return false;
  String user = h.user;
  user.trim();
  return policy::headerLoginOk(s_auth.headerLogin, trusted, user.c_str(),
                               s_auth.allowedUsers.c_str(), h.groups.c_str(),
                               s_auth.allowedGroups.c_str());
}

RequestAuth checkRequest() {
  RequestAuth a{};
  const uint32_t peer = ipValue(s_server.client().remoteIP());
  const bool trusted = peerTrusted(peer);

  // Forwarded values count only from a trusted peer.
  String fwdProto, fwdHost, fwdFor;
  if (trusted) {
    fwdProto = s_server.header("X-Forwarded-Proto");
    fwdHost = s_server.header("X-Forwarded-Host");
    fwdFor = s_server.header("X-Forwarded-For");
  }
  a.client = policy::clientAddress(peer, trusted, fwdFor.c_str());
  a.viaHttpsProxy = policy::viaHttpsProxy(trusted, fwdProto.c_str());

  const String uri = s_server.uri();
  const bool isPost = s_server.method() == HTTP_POST;
  a.cls = policy::classifyRequest(uri.c_str(), isPost, s_auth.passwordSet);

  const bool cookieValid = sessionCookieValid(s_server.header("Cookie").c_str());

  String user;
  bool headerOk = false;
  if (trusted && s_auth.headerLogin) {
    user = s_server.header(s_auth.userHeader);
    user.trim();
    headerOk = policy::headerLoginOk(s_auth.headerLogin, trusted, user.c_str(),
                                     s_auth.allowedUsers.c_str(),
                                     s_server.header(s_auth.groupsHeader).c_str(),
                                     s_auth.allowedGroups.c_str());
  }
  a.method = policy::authMethod(cookieValid, headerOk);
  if (a.method == policy::AuthMethod::Header) {
    a.user = user;
    static String s_lastHeaderUser;
    if (user != s_lastHeaderUser) {
      s_lastHeaderUser = user;
      Serial.printf("[AUTH] header login user=%s client=%s\n", user.c_str(),
                    ipText(a.client).c_str());
    }
  }

  policy::RequestFacts facts{};
  facts.cls = a.cls;
  facts.apMode = s_apMode;
  facts.proxyOnly = s_auth.proxyOnly;
  facts.viaHttpsProxy = a.viaHttpsProxy;
  facts.loggedIn = a.method != policy::AuthMethod::None;
  facts.originOk = a.cls == policy::RequestClass::Public ||
                   policy::originAllowed(s_server.header("Origin").c_str(),
                                         s_server.header("Referer").c_str(),
                                         s_server.hostHeader().c_str(), trusted,
                                         fwdProto.c_str(), fwdHost.c_str());
  a.verdict = policy::decideRequest(facts);
  return a;
}

// Keeps control characters out of the Serial log.
String logSafe(const String &raw) {
  String out = raw;
  for (size_t i = 0; i < out.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(out[i]);
    if (c < 0x20 || c == 0x7F)
      out.setCharAt(i, '?');
  }
  return out;
}

// Everything except letters, digits, "-_.~" and "/" becomes %XX.
String percentEncode(const String &raw) {
  static const char kHex[] = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < raw.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

void sendAuthJson(int code, const String &body) {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(code, "application/json", body);
}

void sendAuthError(int code, const char *message) {
  StaticJsonDocument<192> doc;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  sendAuthJson(code, out);
}

void sendRefusal(const RequestAuth &a) {
  const char *why = "login";
  if (a.verdict == policy::AuthVerdict::BadOrigin)
    why = "origin";
  else if (a.verdict == policy::AuthVerdict::ProxyOnly)
    why = "proxy-only";
  const String uri = s_server.uri();
  Serial.printf("[AUTH] refused %s %s: %s client=%s\n",
                s_server.method() == HTTP_POST ? "POST" : "GET", logSafe(uri).c_str(), why,
                ipText(a.client).c_str());

  const bool page = a.cls == policy::RequestClass::ProtectedPage;
  if (a.verdict == policy::AuthVerdict::NeedLogin) {
    if (page) {
      s_server.sendHeader("Location", String("/login?next=") + percentEncode(uri));
      s_server.sendHeader("Cache-Control", "no-store");
      s_server.send(302, "text/plain", "");
    } else {
      // The text is fixed, so the body is written out whole.
      sendAuthJson(401, String("{\"error\":\"") +
                            (s_auth.passwordSet ? "Log in first." : "Set a password first.") +
                            "\",\"login\":\"/login\"}");
    }
    return;
  }
  const char *message = a.verdict == policy::AuthVerdict::ProxyOnly
                            ? "This Remote only takes changes through its HTTPS proxy."
                            : "Refused: the request did not come from this Remote's page.";
  if (a.verdict == policy::AuthVerdict::ProxyOnly && page) {
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(403, "text/html",
                  String("<!doctype html><title>Refused</title><p>") + message + "</p>");
    return;
  }
  sendAuthError(403, message);
}

// Every route goes through this, public ones included: the policy classifies
// by path, so a route added later is protected by default.
std::function<void()> guarded(std::function<void()> handler) {
  return [handler]() {
    const RequestAuth a = checkRequest();
    if (a.verdict == policy::AuthVerdict::Allow) {
      s_req = a;
      handler();
    } else {
      sendRefusal(a);
    }
  };
}

// The upload function runs only while the verdict taken at UPLOAD_FILE_START
// is Allow, for every upload status, so a refused upload never reaches
// Update.begin, applyMode or the image buffer.
std::function<void()> guardedUpload(const char *route, std::function<void()> upload) {
  return [route, upload]() {
    if (s_server.upload().status == UPLOAD_FILE_START) {
      s_uploadAuth = checkRequest();
      s_uploadRoute = route;
    }
    if (s_uploadRoute == route && s_uploadAuth.verdict == policy::AuthVerdict::Allow)
      upload();
    if (s_server.upload().status == UPLOAD_FILE_ABORTED && s_uploadRoute == route)
      s_uploadRoute = nullptr;  // no final handler follows an abort
  };
}

// The final handler uses the verdict the upload took and never recomputes it,
// so a stale s_otaOk from an earlier upload can't restart the Remote. A
// request that carried no upload at all (no START) has no verdict yet and is
// checked here, once.
std::function<void()> guardedAfterUpload(const char *route, std::function<void()> handler) {
  return [route, handler]() {
    if (s_uploadRoute != route)
      s_uploadAuth = checkRequest();
    const RequestAuth a = s_uploadAuth;
    s_uploadRoute = nullptr;
    if (a.verdict == policy::AuthVerdict::Allow) {
      s_req = a;
      handler();
    } else {
      sendRefusal(a);
    }
  };
}

// 16 random bytes in a cookie that lasts for 30 real days; the table keeps
// only the hash. Secure is added when the request came through the HTTPS proxy.
void startSession(const RequestAuth &a) {
  uint8_t token[policy::kSessionTokenLen];
  esp_fill_random(token, sizeof(token));
  s_sessions.add(token, s_hours.hours());
  if (!saveSessions(s_sessions))
    Serial.println("[AUTH] could not save the sessions; this one ends at a restart");
  char hex[policy::kSessionTokenLen * 2 + 1];
  policy::toHex(token, sizeof(token), hex);
  String cookie = String(kSessionCookie) + "=" + hex + kSessionCookieAttrs;
  if (a.viaHttpsProxy)
    cookie += "; Secure";
  s_server.sendHeader("Set-Cookie", cookie);
}

void handleLoginPage() {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send_P(200, "text/html", kLoginHtml);
}

void handleAuthScript() { s_server.send_P(200, "application/javascript", kAuthJs); }

void handleAuthChallenge() {
  uint8_t nonce[policy::kNonceLen];
  uint8_t newSalt[policy::kSaltLen];
  esp_fill_random(nonce, sizeof(nonce));
  esp_fill_random(newSalt, sizeof(newSalt));
  s_nonces.add(nonce, millis());

  char nonceHex[policy::kNonceLen * 2 + 1];
  char newSaltHex[policy::kSaltLen * 2 + 1];
  char saltHex[policy::kSaltLen * 2 + 1];
  policy::toHex(nonce, sizeof(nonce), nonceHex);
  policy::toHex(newSalt, sizeof(newSalt), newSaltHex);

  StaticJsonDocument<512> doc;
  doc["passwordSet"] = s_auth.passwordSet;
  if (s_auth.passwordSet) {
    policy::toHex(s_auth.salt, sizeof(s_auth.salt), saltHex);
    doc["salt"] = saltHex;
    doc["iterations"] = s_auth.iterations;
  }
  doc["nonce"] = nonceHex;
  doc["newSalt"] = newSaltHex;
  doc["newIterations"] = policy::kDefaultIterations;
  String out;
  serializeJson(doc, out);
  sendAuthJson(200, out);
}

void handleAuthLogin() {
  const uint32_t now = millis();
  const uint32_t client = s_req.client;

  // A locked client is refused before anything else is touched.
  const uint32_t waitMs = s_lockout.retryAfterMs(client, now);
  if (waitMs != 0) {
    const uint32_t waitS = (waitMs + 999) / 1000;
    StaticJsonDocument<192> doc;
    doc["error"] = String("Too many tries. Wait ") + waitS + " s and try again.";
    doc["retryAfterS"] = waitS;
    String out;
    serializeJson(doc, out);
    sendAuthJson(429, out);
    return;
  }

  uint8_t nonce[policy::kNonceLen];
  uint8_t proof[policy::kSha256Len];
  if (!policy::fromHex(s_server.arg("nonce").c_str(), nonce, sizeof(nonce)) ||
      !policy::fromHex(s_server.arg("proof").c_str(), proof, sizeof(proof))) {
    sendAuthError(400, "Bad request.");
    return;
  }
  if (!s_auth.passwordSet) {
    sendAuthError(400, "No password is set yet.");
    return;
  }
  // The nonce is burned whether or not the proof is right.
  if (!s_nonces.take(nonce, now)) {
    sendAuthError(401, "The login expired. Try again.");
    return;
  }
  if (!policy::proofMatches(s_auth.verifier, nonce, proof)) {
    s_lockout.failure(client, now);
    Serial.printf("[AUTH] login failed client=%s (%u/%u)\n", ipText(client).c_str(),
                  static_cast<unsigned>(s_lockout.failureCount(client)),
                  static_cast<unsigned>(policy::kLockoutFailures));
    if (s_lockout.retryAfterMs(client, now) != 0)
      Serial.printf("[AUTH] locked out client=%s for %u s\n", ipText(client).c_str(),
                    static_cast<unsigned>(policy::kLockoutMs / 1000));
    sendAuthError(401, "Wrong password.");
    return;
  }
  s_lockout.success(client);
  startSession(s_req);
  Serial.printf("[AUTH] login ok client=%s\n", ipText(client).c_str());
  sendAuthJson(200, "{\"ok\":true}");
}

// Setting the first password (no login needed, once) and changing it (needs a
// login): the gate has already told the two apart. Either way every session
// ends and this request gets a new one.
void handleAuthPassword() {
  uint8_t salt[policy::kSaltLen];
  uint8_t verifier[policy::kVerifierLen];
  const String iterText = s_server.arg("iterations");
  bool digits = iterText.length() > 0 && iterText.length() <= 6;
  for (size_t i = 0; digits && i < iterText.length(); ++i)
    digits = iterText[i] >= '0' && iterText[i] <= '9';
  const uint32_t iterations = digits ? static_cast<uint32_t>(iterText.toInt()) : 0;
  if (!policy::fromHex(s_server.arg("salt").c_str(), salt, sizeof(salt)) ||
      !policy::fromHex(s_server.arg("verifier").c_str(), verifier, sizeof(verifier)) ||
      !digits || !policy::validIterations(iterations)) {
    sendAuthError(400, "Bad request.");
    return;
  }

  const bool changing = s_auth.passwordSet;
  AuthSettings next = s_auth;
  memcpy(next.salt, salt, sizeof(salt));
  memcpy(next.verifier, verifier, sizeof(verifier));
  next.iterations = iterations;
  next.passwordSet = true;
  if (!savePassword(next)) {
    sendAuthError(500, "The password could not be saved.");
    return;
  }
  s_auth = next;
  s_sessions.clear();
  s_nonces.clear();
  startSession(s_req);  // adds the new session and saves the table
  Serial.println(changing ? "[AUTH] password changed, sessions ended" : "[AUTH] password set");
  sendAuthJson(200, "{\"ok\":true}");
}

void handleAuthLogout() {
  uint8_t token[policy::kSessionTokenLen];
  const String cookie = s_server.header("Cookie");
  if (policy::sessionTokenFromCookie(cookie.c_str(), token) && s_sessions.remove(token))
    saveSessions(s_sessions);
  Serial.printf("[AUTH] logout client=%s\n", ipText(s_req.client).c_str());
  s_server.sendHeader("Set-Cookie", String(kSessionCookie) + "=; Path=/; Max-Age=0");
  sendAuthJson(200, "{\"ok\":true}");
}

void handleAuthMe() {
  const policy::AuthMethod method = s_req.method;
  StaticJsonDocument<384> doc;
  doc["loggedIn"] = method != policy::AuthMethod::None;
  doc["method"] = method == policy::AuthMethod::Password ? "password"
                  : method == policy::AuthMethod::Header ? "header"
                                                         : "none";
  doc["user"] = s_req.user;
  doc["passwordSet"] = s_auth.passwordSet;
  // softAP() is called without a password unless it has at least 8 characters.
  doc["apOpen"] = s_apMode && strlen(IHEATER_AP_PASSWORD) < 8;
  String out;
  serializeJson(doc, out);
  sendAuthJson(200, out);
}

// Set when a save changes a header name; loop() re-registers the headers,
// because collectHeaders() clears the current request's headers.
bool s_headersDirty = false;

// Security settings: every field is checked before anything changes, so a 400
// leaves memory and NVS as they were.
void handleAuthSettingsPost() {
  uint32_t proxies[policy::kMaxTrustedProxies] = {};
  size_t proxyCount = 0;
  if (!policy::parseProxyList(s_server.arg("proxies").c_str(), proxies, proxyCount)) {
    sendAuthError(400, "Trusted proxies: up to four IPv4 addresses, separated by commas.");
    return;
  }
  String userHeader = s_server.arg("userHeader");
  String groupsHeader = s_server.arg("groupsHeader");
  userHeader.trim();
  groupsHeader.trim();
  if (!policy::validHeaderName(userHeader.c_str()) ||
      !policy::validHeaderName(groupsHeader.c_str())) {
    sendAuthError(400, "Header names may use letters, digits and dashes only.");
    return;
  }
  const String allowedUsers = s_server.arg("allowedUsers");
  const String allowedGroups = s_server.arg("allowedGroups");
  if (allowedUsers.length() > 128 || allowedGroups.length() > 128) {
    sendAuthError(400, "The allowed lists can be at most 128 characters each.");
    return;
  }
  const bool proxyOnly = s_server.arg("proxyOnly") == "1";
  const bool autoProxy = s_server.arg("autoProxy") == "1";
  // Proxy-only must hold under the new MANUAL proxy list alone. The automatic
  // Moonraker entry can vanish and would lock the owner out. The access point
  // ignores proxy-only, so a save made there skips the check.
  switch (policy::checkProxyOnlySave(proxyOnly, s_apMode, s_req.viaHttpsProxy,
                                     ipValue(s_server.client().remoteIP()), proxies,
                                     proxyCount)) {
    case policy::ProxyOnlySave::Allowed:
      break;
    case policy::ProxyOnlySave::NeedListedProxy:
      sendAuthError(400,
                    "Proxy-only mode needs the HTTPS proxy's address in the trusted proxy list. "
                    "The automatic Moonraker entry isn't enough.");
      return;
    case policy::ProxyOnlySave::NeedHttps:
      sendAuthError(400, "Turn on proxy-only mode from the HTTPS address, so you know it works.");
      return;
  }

  // Nothing is committed to s_auth until the save succeeds.
  AuthSettings next = s_auth;
  memcpy(next.proxies, proxies, sizeof(proxies));
  next.proxyCount = proxyCount;
  next.autoProxy = autoProxy;
  next.headerLogin = s_server.arg("headerLogin") == "1";
  next.proxyOnly = proxyOnly;
  next.userHeader = userHeader;
  next.allowedUsers = allowedUsers;
  next.groupsHeader = groupsHeader;
  next.allowedGroups = allowedGroups;
  if (!saveAuthOptions(next)) {
    saveAuthOptions(s_auth);  // put back anything a partial write changed
    sendAuthError(500, "Could not save the security settings.");
    return;
  }
  const bool namesChanged =
      s_auth.userHeader != next.userHeader || s_auth.groupsHeader != next.groupsHeader;
  s_auth = next;
  if (namesChanged)
    s_headersDirty = true;
  Serial.println("[AUTH] security settings saved");
  sendAuthJson(200, "{\"ok\":true}");
}

// Registers the request headers the login gate reads, and no others: the
// framework heap-allocates every header it collects, so collecting them all
// would let any client exhaust the heap. Host is read by the framework itself.
// Call it before s_server.begin(), and again when Setup changes the user or
// groups header names. It clears the current request's headers, so it must
// not run inside a request handler.
void registerAuthHeaders() {
  const String userHeader = s_auth.userHeader;
  const String groupsHeader = s_auth.groupsHeader;
  const char *keys[8];
  size_t count = 0;
  const auto add = [&](const char *name) {
    if (!name || !*name)
      return;
    for (size_t i = 0; i < count; ++i) {
      if (strcasecmp(keys[i], name) == 0)
        return;
    }
    keys[count++] = name;
  };
  add("Origin");
  add("Referer");
  add("Cookie");
  add("X-Forwarded-Proto");
  add("X-Forwarded-Host");
  add("X-Forwarded-For");
  add(userHeader.c_str());
  add(groupsHeader.c_str());
  s_server.collectHeaders(keys, count);
}

void handleRoot() {
  if (captiveRedirectIfNeeded())
    return;

  s_server.send_P(200, "text/html", kRemoteDashboardHtml);
}

void handleDiagPage() {
  if (captiveRedirectIfNeeded())
    return;
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send_P(200, "text/html", kDiagHtml);
}

// A one-time ticket that lets this logged-in page subscribe to the console
// over the socket, which carries no session of its own. The ticket goes only
// into this reply: never print it.
void handleDiagTicket() {
  char hex[2 * policy::kTicketLen + 1];
  feedIssueTicket(hex);
  String body = String("{\"ticket\":\"") + hex + "\"}";
  memset(hex, 0, sizeof(hex));
  sendAuthJson(200, body);
}

void configureRoutes() {
  s_server.serveStatic("/logo.png", LittleFS, "/logo.png");
  s_server.on("/", HTTP_GET, guarded(handleRoot));
  s_server.on("/login", HTTP_GET, guarded(handleLoginPage));
  s_server.on("/auth.js", HTTP_GET, guarded(handleAuthScript));
  s_server.on("/api/auth/challenge", HTTP_GET, guarded(handleAuthChallenge));
  s_server.on("/api/auth/me", HTTP_GET, guarded(handleAuthMe));
  s_server.on("/api/auth/login", HTTP_POST, guarded(handleAuthLogin));
  s_server.on("/api/auth/password", HTTP_POST, guarded(handleAuthPassword));
  s_server.on("/api/auth/logout", HTTP_POST, guarded(handleAuthLogout));
  s_server.on("/api/auth/settings", HTTP_POST, guarded(handleAuthSettingsPost));
  s_server.on("/setup", HTTP_GET, guarded(handleSetupPage));
  s_server.on("/fw", HTTP_GET, guarded(handleFirmwarePage));
  s_server.on("/diag", HTTP_GET, guarded(handleDiagPage));
  s_server.on("/api/diag/ticket", HTTP_POST, guarded(handleDiagTicket));
  s_server.on("/api/mode", HTTP_POST, guarded(handleModePost));
  s_server.on("/api/override/end", HTTP_POST, guarded(handleOverrideEnd));
  s_server.on("/api/timer", HTTP_POST, guarded(handleTimerPost));
  s_server.on("/api/integration", HTTP_POST, guarded(handleIntegrationPost));
  s_server.on("/api/setup", HTTP_POST, guarded(handleSetupPost));
  s_server.on("/api/wifi", HTTP_POST, guarded(handleWifiPost));
  s_server.on("/api/wifi/clear", HTTP_POST, guarded(handleWifiClearPost));
  s_server.on("/api/ota", HTTP_POST, guardedAfterUpload("ota", handleOtaPost),
              guardedUpload("ota", handleOtaUpload));
  s_server.on("/api/iheater/image", HTTP_POST,
              guardedAfterUpload("iheater-image", handleIheaterImagePost),
              guardedUpload("iheater-image", handleIheaterImageUpload));
  s_server.on("/api/iheater/update", HTTP_POST, guarded([]() { handleIheaterStart(false); }));
  s_server.on("/api/iheater/retry", HTTP_POST, guarded([]() { handleIheaterStart(true); }));
  s_server.on("/api/iheater/cancel", HTTP_POST, guarded(handleIheaterCancel));
  s_server.on("/api/iheater/image/discard", HTTP_POST, guarded(handleIheaterDiscard));
  s_server.on("/api/off", HTTP_POST, guarded([]() {
    applyMode(0, "web");
    sendActionStatus();
  }));
  s_server.on("/generate_204", HTTP_GET, guarded(handleRoot));
  s_server.on("/gen_204", HTTP_GET, guarded(handleRoot));
  s_server.on("/hotspot-detect.html", HTTP_GET, guarded(handleRoot));
  s_server.on("/library/test/success.html", HTTP_GET, guarded(handleRoot));
  s_server.on("/ncsi.txt", HTTP_GET, guarded(handleRoot));
  s_server.on("/connecttest.txt", HTTP_GET, guarded(handleRoot));
  s_server.on("/redirect", HTTP_GET, guarded(handleRoot));
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
      s_authResetDone = false;
    } else if (!s_longPressHandled) {
      if (s_pendingClicks < 255)
        ++s_pendingClicks;
      s_clickDeadlineMs = now + kMultiClickWindowMs;
    }
  }

  if (s_buttonStableDown) {
    const policy::HoldStep step = policy::holdStep(
        (uint32_t)(now - s_buttonDownAtMs), s_longPressHandled, s_authResetDone);
    if (step == policy::HoldStep::HeatOff) {
      s_pendingClicks = 0;
      s_longPressHandled = true;
      applyMode(0, "button:long");
    } else if (step == policy::HoldStep::AuthReset) {
      s_authResetDone = true;
      if (!resetAuth(s_auth, s_sessions))
        Serial.println("[AUTH] BOOT reset: could not clear the saved login; it will return at a restart");
      s_nonces.clear();
      s_lockout.clear();
      Serial.println("[AUTH] BOOT reset: password, sessions, header login and proxy-only cleared");
      s_ledSolid = true;
      s_ledSolidUntilMs = now + 3000;
    }
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
  if (s_ledSolid) {
    if ((int32_t)(s_ledSolidUntilMs - now) > 0) {
      if (!s_ledOn)
        writeLed(true);
      return;
    }
    s_ledSolid = false;
    resetLedPattern();
  }
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

// Klipper source on the UART output: until Moonraker delivers a target, adopt
// the one the iHeater is holding, so the existing 10-minute link-loss rule
// covers it. Any applyMode/applyIntegrationTarget (Moonraker's target, a
// Setup-save stop, the link-loss cut) ends the wait first.
void updateKlipperRecovery() {
  if (!s_klipperRecoveryPending)
    return;
  if (!klipperSource() || s_outputKind != OutputKind::Uart) {
    s_klipperRecoveryPending = false;
    return;
  }
  const UartLinkView view = s_uart.view();
  if (!view.hasSnapshot)
    return;
  applyIntegrationTarget(view.snap.airTargetT10 / 10.0f, "recovered", "klipper");
}

// True after a crash, watchdog, ESP.restart() (OTA, Setup save) and similar;
// false for power-on, brownout and anything else, where RTC memory is garbage.
bool softwareReset() {
  switch (esp_reset_reason()) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

// Short name of the reset reason, for the "Remote started" event.
const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_BROWNOUT:
      return "brownout";
    default:
      return "other";
  }
}

// Every 250 ms: compares the Wi-Fi, Moonraker and iHeater link state with the
// last look and records what changed. The raw lastError() never goes in an
// event (it can carry the pairing token).
void updateEventTracker() {
  static policy::EventTracker s_tracker;
  static uint32_t s_checkedAtMs = 0;
  static policy::WifiState s_wifi = policy::WifiState::Connecting;
  // The last snapshot's values. The tracker overwrites its baseline on every
  // call, so a gap without a snapshot must not read as "fault cleared".
  static uint8_t s_fault = 0;
  static uint16_t s_proto = 0;
  static bool s_protoOk = true;
  static uint32_t s_ihUptimeS = 0;
  static uint16_t s_resetCause = 0;
  const uint32_t now = millis();
  if (now - s_checkedAtMs < 250)
    return;
  s_checkedAtMs = now;

  policy::TrackedState st;
  if (s_apMode)
    s_wifi = policy::WifiState::Ap;
  else if (WiFi.status() == WL_CONNECTED)
    s_wifi = policy::WifiState::Joined;
  else if (s_wifi == policy::WifiState::Joined || s_wifi == policy::WifiState::Lost)
    s_wifi = policy::WifiState::Lost;
  else
    s_wifi = policy::WifiState::Connecting;
  st.wifi = s_wifi;

  if (klipperSource()) {
    const char *moon = s_moonraker.stateString();
    if (strcmp(moon, "ready") == 0)
      st.moon = policy::MoonState::Ready;
    else if (strcmp(moon, "klippy-not-ready") == 0)
      st.moon = policy::MoonState::NotReady;
    else if (strcmp(moon, "connecting") == 0)
      st.moon = policy::MoonState::Connecting;
    else
      st.moon = policy::MoonState::Disconnected;
    st.paired = s_moonraker.heaterObject().length() > 0;
  }

  if (s_outputKind == OutputKind::Uart && s_uart.isEnabled()) {
    const UartLinkView view = s_uart.view();
    st.uart = true;
    st.link = view.link;
    st.hasSnapshot = view.hasSnapshot;
    if (view.hasSnapshot) {
      s_fault = view.snap.activeFault;
      s_proto = view.snap.proto;
      s_protoOk = view.protocolSupported;
      s_ihUptimeS = view.snap.uptimeS;
      s_resetCause = view.snap.resetCause;
    }
    st.fault = s_fault;
    st.proto = s_proto;
    st.protocolSupported = s_protoOk;
    st.ihUptimeS = s_ihUptimeS;
    st.resetCause = s_resetCause;
    st.clear = view.clear;
    st.lastClearOk = view.lastClearOk;
  }

  policy::TrackedEvent events[8];
  const size_t n = s_tracker.update(st, events, 8);
  for (size_t i = 0; i < n; i++)
    feedRecord(events[i].kind, "%s", events[i].text);
}

// The newest graph sample: the iHeater's air and heater readings while the UART
// link is up, and the target the Remote is asking for. Pulse output has no
// readings and no data.
policy::HistSample currentHistSample() {
  policy::HistSample sample;
  if (s_outputKind != OutputKind::Uart || !s_uart.isEnabled())
    return sample;
  const UartLinkView view = s_uart.view();
  if (view.hasSnapshot && view.link == policy::LinkStatus::Up) {
    sample.airT10 = view.snap.airT10;
    sample.heaterT10 = view.snap.heaterT10;
  }
  const auto last = s_uart.getLastCommand();
  if (last.mode == ControllerOutputMode::TargetTemperature)
    sample.targetT10 = static_cast<int16_t>(lroundf(last.targetTempC * 10.0f));
  return sample;
}

// After loadRemoteSettings(): decide what the output does at boot.
void bootDecision() {
  s_resumeLocked = false;
  if (klipperSource()) {
    // Pulse is already off (early boot applyMode). UART writes nothing until
    // the iHeater's own target has been read back, or Moonraker delivers one.
    if (s_outputKind == OutputKind::Uart) {
      // Continue the link-loss count from before a software restart. Sealed
      // straight away so a crash before the first refresh keeps it.
      // The time already spent in setup() (Wi-Fi connect blocks for seconds)
      // counts too, so every boot of a crash loop moves the record forward even
      // if it panics before the first refresh.
      uint32_t carriedMs = 0;
      const bool carried =
          policy::carriedLinkLoss(s_linkLoss, softwareReset(), carriedMs);
      s_linkHealthyAtMs = policy::linkLossBaselineMs(carriedMs);
      const uint32_t unhealthyMs = policy::linkLossCarryPlus(carriedMs, millis());
      policy::sealLinkLoss(s_linkLoss, unhealthyMs);
      if (carried && policy::linkLossExpired(unhealthyMs, policy::kLinkLossGraceMs)) {
        // The printer has been gone for the whole grace: do not adopt the
        // iHeater's target, switch it off.
        Serial.printf("[REMOTE] link loss carried over restart: %us, heat off\n",
                      (unsigned)(unhealthyMs / 1000u));
        applyIntegrationTarget(0.0f, "link lost 10 min (carried)", "link-lost");
        return;
      }
      if (carried)
        Serial.printf("[REMOTE] link loss carried over restart: %us\n",
                      (unsigned)(unhealthyMs / 1000u));
      s_klipperRecoveryPending = true;
    } else {
      s_klipperRecoveryPending = false;
    }
    writeResumeRecord();  // mode 0: a stale Manual record must not outlive this
    return;
  }
  if (policy::resumeValid(s_resume, softwareReset(), kMaxMode)) {
    const policy::ResumeRecord rec = s_resume;  // applyMode rewrites the record
    s_timerMinutes = rec.timerMinutes;
    applyMode(rec.mode, "resume", false);
    uint32_t remainingS = 0;
    if (rec.mode != 0 && rec.timerMinutes > 0 && rec.remainingS > 0) {
      remainingS = rec.remainingS;
      if (remainingS > (uint32_t)rec.timerMinutes * 60u)
        remainingS = (uint32_t)rec.timerMinutes * 60u;
      s_timerOffAtMs = millis() + remainingS * 1000u;
      writeResumeRecord();
    }
    Serial.printf("[REMOTE] resumed mode=%u remaining=%us\n", rec.mode,
                  (unsigned)remainingS);
    return;
  }
  applyMode(0, "boot", false);
}

} // namespace

void setup() {
  s_resumeLocked = true;  // see bootDecision()
  Serial.begin(115200);
  // Arduino-ESP32 3.x waits up to 20 x the TX timeout (2 s at the default
  // 100 ms) for each write when a PC has the USB port open but isn't reading
  // it. That stalls the loop and the web UI. With 0, a full buffer drops the
  // log text instead.
  Serial.setTxTimeoutMs(0);
  delay(100);
  Serial.println();
  Serial.println("[REMOTE] iHeater Remote firmware starting");
  // First, so the events of the boot decision below are in the log too.
  feedBegin(s_statusSocket);
  feedRecord(policy::EventKind::Info, "Remote started (%s)", resetReasonName());

  if (ledEnabled()) {
    pinMode(IHEATER_STATUS_LED_PIN, OUTPUT);
    writeLed(false);
  }
  pinMode(IHEATER_BOOT_BUTTON_PIN, INPUT_PULLUP);

  s_outputKind = loadOutputKind();
  feedBeginConsole(s_outputKind == OutputKind::Uart ? &s_uart : nullptr);
  if (s_outputKind == OutputKind::Uart) {
    s_output = &s_uart;
    s_output->begin();
    // No boot applyMode(0, ...) here: in UART mode nothing is written to the
    // iHeater until the app sets a desired target, and bootDecision() (after
    // loadRemoteSettings) decides what that target is. Do not add a boot Off
    // write here.
  } else {
    s_output = &s_rmt;
    s_output->begin();
    applyMode(0, "boot", false); // output off first; the binding is untouched
  }

  if (!LittleFS.begin()) {
    Serial.println("[REMOTE] LittleFS unavailable; logo asset will not load");
  }

  loadDeviceSettings(); // before Wi-Fi: carries the TX power setting
  // Unconditional: loadDeviceSettings() returns early on first boot, before
  // NVS exists. Only stored; the UART task uses it once a target is set.
  s_uart.setSetupRange(s_range);
  configureWiFi();
  // After Wi-Fi: a first-boot pairing token needs esp_random() with RF entropy.
  loadRemoteSettings(s_settings);
  bootDecision();
  {
    uint32_t storedHours = 0;
    loadAuth(s_auth, s_sessions, storedHours);
    s_hours.begin(storedHours, millis());
  }
  s_moonraker.onTarget(onKlipperTarget);
  s_moonraker.onUnpaired([]() {
    resetOverride("unpaired");
    // A fixed text for the error's kind, not the error itself.
    feedRecord(policy::EventKind::Warn, "%s",
               policy::unpairReasonText(s_moonraker.lastError().c_str()));
  });
  if (klipperSource())
    s_moonraker.configure(s_settings.moon, s_settings.pairToken.c_str());
  configureRoutes();
  registerAuthHeaders();
  s_server.begin();
  s_statusSocket.onHandshakeHeader(onStatusSocketHeader);
  s_statusSocket.begin();
  s_statusSocket.onEvent(onStatusSocketEvent);
  Serial.printf("[REMOTE] Web UI: http://%s/\n",
                localIpString().c_str());
}

void loop() {
  if (s_apMode)
    s_dnsServer.processNextRequest();
  s_server.handleClient();
  if (s_headersDirty) {
    s_headersDirty = false;
    registerAuthHeaders();
  }
  if (s_hours.tick(millis()))
    saveRunningHours(s_hours.hours());
  s_statusSocket.loop();
  feedLoop(millis(), currentHistSample());
  updateButton();
  updateHeatTimer();
  updateResumeRefresh();
  updateKlipperRecovery();
  updateLed();
  updateRestart();
  updateWifiSupervisor();
  // Before s_moonraker.loop(): refresh the carried link-loss time ahead of the
  // call most likely to crash (TLS handshake, heap) while the printer is down.
  updateLinkSafety();
  s_moonraker.loop();
  updateKlipperReport();
  updateUartLinkLog();
  updateEventTracker();
  updateIheaterImageHousekeeping();
  updateHeapLog();
  pushStatusIfChanged();
  delay(2);
}

} // namespace standalone
} // namespace iheaterlink
