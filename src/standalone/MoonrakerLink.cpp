#include "standalone/MoonrakerLink.h"

#include <string.h>

#include <AuthPolicy.h>
#include <WiFi.h>

#include "standalone/MoonrakerCa.h"
#include <UartRegisters.h>
#include "version.h"

namespace iheaterlink::standalone {

namespace {

constexpr const char *kPath = "/websocket";
constexpr const char *kClientName = "iheater-remote";
constexpr const char *kClientUrl = "https://github.com/Vylyne/iHeater-Remote";
constexpr size_t kParseDocBytes = 8192;

constexpr const char *kReportMethod = "server.iheater_remote.report";
constexpr int kMethodNotFound = -32601;
constexpr uint32_t kRefusedLogGapMs = 60000;

constexpr const char *kNotifyStatus = "notify_status_update";
constexpr const char *kNotifyReady = "notify_klippy_ready";
constexpr const char *kNotifyShutdown = "notify_klippy_shutdown";
constexpr const char *kNotifyDisconnected = "notify_klippy_disconnected";

bool containsBytes(const uint8_t *hay, size_t len, const char *needle) {
  const size_t n = strlen(needle);
  if (n == 0 || n > len) return false;
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) return true;
  }
  return false;
}

bool startsWith(const char *s, size_t len, const char *prefix) {
  const size_t n = strlen(prefix);
  return s != nullptr && len >= n && memcmp(s, prefix, n) == 0;
}

// Moonraker pushes many notifications we do not use (notify_proc_stat_update
// every second, gcode responses, ...). Skip those before parsing. Moonraker
// writes `"method": "..."` with a space, so match on the names themselves.
bool skipUnparsed(const uint8_t *payload, size_t length) {
  if (!containsBytes(payload, length, "\"method\"")) return false;
  return !containsBytes(payload, length, kNotifyStatus) &&
         !containsBytes(payload, length, kNotifyReady) &&
         !containsBytes(payload, length, kNotifyShutdown) &&
         !containsBytes(payload, length, kNotifyDisconnected);
}

// The status code in "HTTP <code>" or "WebSocket handshake failed - HTTP
// <code>"; 0 when the reason carries none.
int httpCodeFromReason(const char *reason, size_t len) {
  if (reason == nullptr) return 0;
  static const char kMarker[] = "HTTP ";
  const size_t n = sizeof(kMarker) - 1;
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(reason + i, kMarker, n) != 0) continue;
    int code = 0;
    size_t digits = 0;
    for (size_t j = i + n; j < len && reason[j] >= '0' && reason[j] <= '9' &&
                           digits < 4;
         ++j, ++digits) {
      code = code * 10 + (reason[j] - '0');
    }
    return code;
  }
  return 0;
}

const char *klippyName(policy::KlippyState s) {
  switch (s) {
    case policy::KlippyState::Ready: return "ready";
    case policy::KlippyState::NotReady: return "not ready";
    default: return "unknown";
  }
}

}  // namespace

MoonrakerLink::MoonrakerLink() {
  ws_.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
    onEvent(type, payload, length);
  });
}

void MoonrakerLink::onTarget(TargetCallback cb) { targetCb_ = cb; }

void MoonrakerLink::onUnpaired(UnpairedCallback cb) { unpairedCb_ = cb; }

void MoonrakerLink::configure(const MoonrakerSettings &s,
                              const char *pairToken) {
  stop();
  if (s.host != settings_.host || s.port != settings_.port || s.tls != settings_.tls)
    connectedAddress_ = 0;
  settings_ = s;
  if (!policy::normalizePairToken(pairToken, token_)) token_[0] = '\0';

  if (settings_.host.isEmpty()) {
    Serial.println("[MOON] no host set, not connecting");
    return;
  }

  // An empty string also drops the library's default "Origin: file://",
  // which Moonraker refuses (403) unless it is listed in cors_domains.
  extraHeaders_ = "";
  if (!settings_.apiKey.isEmpty()) {
    extraHeaders_ = "X-Api-Key: ";
    for (size_t i = 0; i < settings_.apiKey.length(); ++i) {
      const char c = settings_.apiKey[i];
      if (c >= 0x20 && c != 0x7f) extraHeaders_ += c;  // no header injection
    }
  }

  if (settings_.tls) {
    // beginSslWithCA() with the client certificate and key explicitly null:
    // the library leaves those two pointers uninitialised otherwise.
    ws_.beginSslWithClientKey(settings_.host.c_str(), settings_.port, kPath,
                              kMoonrakerCaPem, nullptr, nullptr);
  } else {
    ws_.begin(settings_.host.c_str(), settings_.port, kPath);
  }
  ws_.setExtraHeaders(extraHeaders_.c_str());
  ws_.enableHeartbeat(policy::kPingIntervalMs, policy::kPongTimeoutMs,
                      policy::kMissedPongs);
  backoffMs_ = 0;
  ws_.setReconnectInterval(policy::nextBackoffMs(0));

  resetSession();
  attemptConnected_ = false;
  disconnectRequested_ = false;
  running_ = true;
  Serial.printf("[MOON] connecting %s://%s:%u%s key=%s\n",
                settings_.tls ? "wss" : "ws", settings_.host.c_str(),
                static_cast<unsigned>(settings_.port), kPath,
                settings_.apiKey.isEmpty() ? "no" : "yes");
}

void MoonrakerLink::stop() {
  const bool wasRunning = running_;
  running_ = false;  // first, so the disconnect event below is not a failure
  ws_.disconnect();
  socketUp_ = false;
  attemptConnected_ = false;
  disconnectRequested_ = false;
  resetSession();
  heater_ = "";
  lastError_ = "";
  if (wasRunning) Serial.println("[MOON] stopped");
}

void MoonrakerLink::loop() {
  if (!running_) return;
  ws_.loop();
  if (!running_) return;

  // Disconnects asked for while handling a frame run here, outside the
  // library's own call stack.
  if (disconnectRequested_) {
    disconnectRequested_ = false;
    ws_.disconnect();
    return;
  }
  if (!socketUp_) return;

  const uint32_t now = millis();
  for (Slot &slot : slots_) {
    if (slot.kind != Pending::None &&
        policy::intervalElapsed(now, slot.sentMs, policy::kRequestTimeoutMs)) {
      if (slot.kind == Pending::Report) {
        // A report is best effort: free its slot and let the next one go out.
        // The link and target following carry on. A late reply for this id
        // matches no slot and is ignored.
        scheduler_.onFailed();
        slot.kind = Pending::None;
        Serial.printf("[MOON] report %u unanswered, dropped\n",
                      static_cast<unsigned>(slot.id));
        continue;
      }
      lastError_ = "no reply from Moonraker";
      Serial.printf("[MOON] request %u timed out, reconnecting\n",
                    static_cast<unsigned>(slot.id));
      ws_.disconnect();
      return;
    }
  }

  if (policy::intervalElapsed(now, lastQueryMs_, policy::kQueryIntervalMs)) {
    lastQueryMs_ = now;
    if (subscribed_) {
      sendHeaterRequest("printer.objects.query", Pending::Query);
    } else if (phase_ != Phase::Identifying && !anyPending()) {
      startDiscovery();
    }
  }
}

bool MoonrakerLink::healthy() const {
  return running_ &&
         policy::moonrakerLinkHealthy(socketUp_, subscribed_, klippy_);
}

const char *MoonrakerLink::stateString() const {
  if (!running_ || !socketUp_) return "disconnected";
  if (healthy()) return "ready";
  if (klippy_ == policy::KlippyState::NotReady &&
      (subscribed_ || phase_ == Phase::Discovering)) {
    return "klippy-not-ready";
  }
  return "connecting";
}

const String &MoonrakerLink::heaterObject() const { return heater_; }

const String &MoonrakerLink::lastError() const { return lastError_; }

// --- WebSocket events --------------------------------------------------------

void MoonrakerLink::onEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      onConnected();
      break;
    case WStype_DISCONNECTED:
      onDisconnected(reinterpret_cast<const char *>(payload), length);
      break;
    case WStype_TEXT:
      if (payload != nullptr && length > 0) onText(payload, length);
      break;
    case WStype_ERROR:
      Serial.printf("[MOON] socket error: %.*s\n", static_cast<int>(length),
                    payload != nullptr ? reinterpret_cast<char *>(payload) : "");
      break;
    default:
      break;
  }
}

void MoonrakerLink::onConnected() {
  socketUp_ = true;
  attemptConnected_ = true;
  resetSession();
  lastQueryMs_ = millis();
  // The address the library just connected to: a literal, or the cached lookup.
  IPAddress addr;
  if (!addr.fromString(settings_.host)) {
    if (!WiFi.hostByName(settings_.host.c_str(), addr)) addr = IPAddress();
  }
  const uint32_t value = (static_cast<uint32_t>(addr[0]) << 24) |
                         (static_cast<uint32_t>(addr[1]) << 16) |
                         (static_cast<uint32_t>(addr[2]) << 8) | static_cast<uint32_t>(addr[3]);
  // An address that cannot be resolved clears the entry: a stale one would
  // keep trusting whatever device now holds the old address.
  if (value != connectedAddress_) {
    connectedAddress_ = value;
    if (value != 0) {
      char text[16];
      policy::formatIpv4(value, text);
      Serial.printf("[MOON] connected address %s\n", text);
    } else {
      Serial.println("[MOON] connected address unknown");
    }
  }
  Serial.println("[MOON] connected, identifying");
  if (running_) sendIdentify();
}

void MoonrakerLink::onDisconnected(const char *reason, size_t length) {
  const bool wasConnected = attemptConnected_;
  socketUp_ = false;
  attemptConnected_ = false;
  resetSession();
  if (!running_) return;

  if (!wasConnected) {
    // The library reports an HTTP rejection of the upgrade (e.g. 401 for a
    // wrong X-Api-Key) as "HTTP <code>" or "WebSocket handshake failed[ -
    // HTTP <code>]"; TCP/TLS failures carry other reasons.
    if (startsWith(reason, length, "HTTP ") ||
        startsWith(reason, length, "WebSocket handshake failed")) {
      lastError_ = "Moonraker refused the connection (";
      for (size_t i = 0; i < length; ++i) lastError_ += reason[i];
      switch (policy::handshakeFailureHint(httpCodeFromReason(reason, length),
                                           settings_.port, settings_.tls)) {
        case policy::HandshakeHint::ApiKey:
          lastError_ += "): check the API key";
          break;
        case policy::HandshakeHint::TurnTlsOn:
          lastError_ += "): port 443 expects TLS, turn TLS on";
          break;
        default:
          lastError_ += "): check host, port and TLS";
          break;
      }
    } else {
      lastError_ = "could not connect";
      if (settings_.tls)
        lastError_ += " (TLS: check certificate and host name)";
      else if (policy::suggestTlsForPort(settings_.port, settings_.tls))
        lastError_ += " (port 443 usually needs TLS: turn TLS on)";
    }
  }
  backoffMs_ = policy::nextBackoffMs(backoffMs_);
  ws_.setReconnectInterval(backoffMs_);
  Serial.printf("[MOON] disconnected (%.*s), retry in %u ms\n",
                reason != nullptr ? static_cast<int>(length) : 0,
                reason != nullptr ? reason : "",
                static_cast<unsigned>(backoffMs_));
}

void MoonrakerLink::onText(uint8_t *payload, size_t length) {
  if (skipUnparsed(payload, length)) return;

  DynamicJsonDocument doc(kParseDocBytes);
  if (doc.capacity() == 0) {
    Serial.printf("[MOON] no memory to parse %u-byte frame, ignored\n",
                  static_cast<unsigned>(length));
    return;
  }
  // Zero-copy: strings in doc point into payload, valid for this call only.
  const DeserializationError err =
      deserializeJson(doc, reinterpret_cast<char *>(payload), length);
  if (err) {
    Serial.printf("[MOON] bad frame (%s, %u bytes), ignored\n", err.c_str(),
                  static_cast<unsigned>(length));
    return;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  const char *method = root["method"].as<const char *>();
  if (method != nullptr) {
    handleNotification(method, root["params"]);
    return;
  }

  JsonVariantConst idValue = root["id"];
  if (!idValue.is<uint32_t>()) return;
  const uint32_t id = idValue.as<uint32_t>();
  Pending kind = Pending::None;
  for (Slot &slot : slots_) {
    if (slot.kind != Pending::None && slot.id == id) {
      kind = slot.kind;
      slot.kind = Pending::None;
      break;
    }
  }
  if (kind == Pending::None) return;  // stale reply from before a reconnect

  JsonVariantConst error = root["error"];
  if (!error.isNull()) {
    handleError(kind, error["code"] | 0, error["message"] | "error");
  } else {
    handleResult(kind, root["result"]);
  }
}

// --- JSON-RPC handling -------------------------------------------------------

void MoonrakerLink::handleNotification(const char *method,
                                       JsonVariantConst params) {
  if (strcmp(method, kNotifyStatus) == 0) {
    JsonObjectConst status = params[0].as<JsonObjectConst>();
    if (status.isNull() || !following()) return;  // unpaired: not ours
    subscribed_ = true;
    scheduler_.forceNext();
    phase_ = Phase::Subscribed;
    applyStatus(status);
  } else if (strcmp(method, kNotifyReady) == 0) {
    Serial.println("[MOON] Klipper ready, rediscovering");
    if (phase_ == Phase::Identifying) return;
    subscribed_ = false;
    startDiscovery();
  } else if (strcmp(method, kNotifyShutdown) == 0 ||
             strcmp(method, kNotifyDisconnected) == 0) {
    Serial.printf("[MOON] %s, holding target\n", method);
    subscribed_ = false;
    klippy_ = policy::KlippyState::NotReady;
    if (phase_ != Phase::Identifying) phase_ = Phase::Discovering;
  }
}

void MoonrakerLink::handleResult(Pending kind, JsonVariantConst result) {
  switch (kind) {
    case Pending::Identify:
      // Backoff resets once Moonraker accepts us, not on the bare socket
      // connect, so a rejected key or a silent server is not retried every 2 s.
      backoffMs_ = 0;
      Serial.println("[MOON] identified");
      startDiscovery();
      break;

    case Pending::List: {
      JsonArrayConst objects = result["objects"].as<JsonArrayConst>();
      sendTokenQuery(objects);
      break;
    }

    case Pending::Discover: {
      const char *names[kMaxHeaters] = {};
      const char *tokens[kMaxHeaters] = {};
      size_t n = 0;
      for (JsonPairConst kv : result["status"].as<JsonObjectConst>()) {
        if (n >= kMaxHeaters) break;
        if (!policy::isRemoteHeaterObject(kv.key().c_str())) continue;
        names[n] = kv.key().c_str();
        tokens[n] = kv.value()["remote_token"].as<const char *>();
        ++n;
      }
      subscribed_ = false;
      if (n == 0) {
        goUnpaired("no heater_iheater_remote in Klipper");
        break;
      }
      const int idx = policy::matchPairedHeater(tokens, n, token_);
      if (idx == policy::kHeaterNone) {
        // The token stays out of the error and the log; the Setup page
        // shows it.
        goUnpaired("not paired: add the token from the Setup page as "
                   "remote_token in your [heater_iheater_remote] section");
        break;
      }
      if (idx == policy::kHeaterDuplicate) {
        goUnpaired("remote_token used by more than one heater");
        break;
      }
      heater_ = names[idx];
      phase_ = Phase::Subscribed;
      Serial.printf("[MOON] paired with %s, subscribing\n", heater_.c_str());
      sendHeaterRequest("printer.objects.subscribe", Pending::Subscribe);
      break;
    }

    case Pending::Subscribe:
    case Pending::Query: {
      // A late reply after discovery went unpaired must not mark the link
      // subscribed (and so healthy).
      if (!following()) break;
      JsonObjectConst status = result["status"].as<JsonObjectConst>();
      subscribed_ = true;
      scheduler_.forceNext();
      phase_ = Phase::Subscribed;
      applyStatus(status);
      break;
    }

    case Pending::Report:
      scheduler_.onAccepted();
      break;

    case Pending::None:
      break;
  }
}

void MoonrakerLink::handleError(Pending kind, int code, const char *message) {
  switch (kind) {
    case Pending::Identify:
      Serial.printf("[MOON] identify failed: %s\n", message);
      lastError_ = String("auth failed: ") + message;
      requestDisconnect();
      break;

    case Pending::List:
    case Pending::Discover:
    case Pending::Subscribe:
    case Pending::Query:
      // Klipper not ready (or gone): wait for notify_klippy_ready; the 30 s
      // tick rediscovers as a backstop because subscribed_ is now false.
      Serial.printf("[MOON] request failed, Klipper not ready: %s\n", message);
      subscribed_ = false;
      klippy_ = policy::KlippyState::NotReady;
      phase_ = Phase::Discovering;
      break;

    case Pending::Report:
      // Never touches subscribed_, klippy_ or phase_: a refused report says
      // nothing about the target link.
      if (code == kMethodNotFound) {
        scheduler_.onMethodNotFound();
        Serial.println("[MOON] Klipper plugin not installed: reports stopped until reconnect");
      } else {
        scheduler_.onRefused();
        const uint32_t now = millis();
        if (!refusedLogged_ ||
            policy::intervalElapsed(now, lastRefusedLogMs_, kRefusedLogGapMs)) {
          refusedLogged_ = true;
          lastRefusedLogMs_ = now;
          Serial.printf("[MOON] report refused (%d): %s\n", code, message);
        }
      }
      break;

    case Pending::None:
      break;
  }
}

uint32_t MoonrakerLink::call(const char *method, JsonVariantConst params,
                             Pending kind) {
  Slot *slot = nullptr;
  for (Slot &s : slots_) {
    if (s.kind == Pending::None) {
      slot = &s;
      break;
    }
  }
  if (slot == nullptr) {
    Serial.printf("[MOON] no free request slot for %s, reconnecting\n", method);
    requestDisconnect();
    return 0;
  }

  const uint32_t id = nextId_++;
  if (nextId_ == 0) nextId_ = 1;

  String frame;
  frame.reserve(64 + strlen(method) + (params.isNull() ? 0 : measureJson(params)));
  frame += "{\"jsonrpc\":\"2.0\",\"method\":\"";
  frame += method;
  frame += '"';
  if (!params.isNull()) {
    String body;
    serializeJson(params, body);
    frame += ",\"params\":";
    frame += body;
  }
  frame += ",\"id\":";
  frame += id;
  frame += '}';

  if (!ws_.sendTXT(frame)) {
    Serial.printf("[MOON] send failed for %s, reconnecting\n", method);
    requestDisconnect();
    return 0;
  }
  slot->id = id;
  slot->kind = kind;
  slot->sentMs = millis();
  return id;
}

void MoonrakerLink::sendIdentify() {
  StaticJsonDocument<256> params;
  params["client_name"] = kClientName;
  params["version"] = VERSION_STR;
  params["type"] = "other";
  params["url"] = kClientUrl;
  if (!settings_.apiKey.isEmpty()) params["api_key"] = settings_.apiKey.c_str();
  call("server.connection.identify", params.as<JsonVariantConst>(),
       Pending::Identify);
}

void MoonrakerLink::startDiscovery() {
  phase_ = Phase::Discovering;
  call("printer.objects.list", JsonVariantConst(), Pending::List);
}

void MoonrakerLink::sendTokenQuery(JsonArrayConst objects) {
  DynamicJsonDocument params(1024);
  JsonObject query = params.createNestedObject("objects");
  size_t found = 0;
  for (JsonVariantConst v : objects) {
    const char *name = v.as<const char *>();
    if (!policy::isRemoteHeaterObject(name)) continue;
    ++found;
    if (found > kMaxHeaters) continue;
    query.createNestedArray(name).add("remote_token");
  }

  if (found == 0) {
    subscribed_ = false;
    goUnpaired("no heater_iheater_remote in Klipper");
    return;
  }
  if (found > kMaxHeaters) {
    Serial.printf("[MOON] %u heater_iheater_remote objects, checking the first %u\n",
                  static_cast<unsigned>(found),
                  static_cast<unsigned>(kMaxHeaters));
  }
  if (params.overflowed()) {
    Serial.println("[MOON] token query too large, some heaters skipped");
  }
  call("printer.objects.query", params.as<JsonVariantConst>(),
       Pending::Discover);
}

void MoonrakerLink::sendHeaterRequest(const char *method, Pending kind) {
  if (heater_.isEmpty()) {
    Serial.printf("[MOON] %s skipped: no paired heater\n", method);
    return;
  }
  StaticJsonDocument<256> params;
  JsonObject objects = params.createNestedObject("objects");
  objects.createNestedArray(heater_.c_str()).add("target");
  objects.createNestedArray("webhooks").add("state");
  call(method, params.as<JsonVariantConst>(), kind);
}

void MoonrakerLink::report(const policy::ReportInputs &in) {
  if (!running_ || !socketUp_ || !subscribed_ || token_[0] == '\0') return;
  const uint32_t now = millis();
  if (scheduler_.inFlight() || !scheduler_.due(in, now)) return;

  const policy::ReportValues v = policy::reportValues(in);
  StaticJsonDocument<384> params;
  params["remote_token"] = token_;
  params["link"] = policy::linkStatusName(in.link);
  if (in.hasSnapshot) {
    params["state"] = policy::iheaterStateName(in.state);
    params["fan"] = in.fanOn;
    params["fault"] = in.fault;
    params["fault_name"] = policy::faultName(in.fault);
  } else {
    params["state"] = "unknown";
    params["fan"] = false;
    params["fault"] = 0;
    params["fault_name"] = policy::faultName(0);
  }
  if (v.hasAir) params["air"] = v.airT10 / 10.0f; else params["air"] = static_cast<const char *>(nullptr);
  if (v.hasElement) params["element"] = v.elementT10 / 10.0f; else params["element"] = static_cast<const char *>(nullptr);
  if (v.hasPower) params["power"] = v.powerPermille / 1000.0f; else params["power"] = static_cast<const char *>(nullptr);
  params["next_within"] = v.nextWithinS;
  if (params.overflowed()) {
    Serial.println("[MOON] report frame too large, not sent");
    return;
  }

  const uint32_t id = call(kReportMethod, params.as<JsonVariantConst>(), Pending::Report);
  scheduler_.onSent(in, now);
  if (id == 0) scheduler_.onFailed();
}

void MoonrakerLink::applyStatus(JsonObjectConst status) {
  if (status.isNull()) return;

  JsonVariantConst state = status["webhooks"]["state"];
  if (state.is<const char *>()) {
    const policy::KlippyState next = strcmp(state.as<const char *>(), "ready") == 0
                                         ? policy::KlippyState::Ready
                                         : policy::KlippyState::NotReady;
    if (next != klippy_) {
      Serial.printf("[MOON] Klipper %s\n", klippyName(next));
      klippy_ = next;
    }
  }

  // Applied whatever the Klipper state.
  if (!heater_.isEmpty()) {
    JsonVariantConst target = status[heater_.c_str()]["target"];
    if (target.is<float>() && targetCb_ != nullptr) {
      targetCb_(target.as<float>());
    }
  }

  if (subscribed_ && klippy_ == policy::KlippyState::Ready) lastError_ = "";
}

// --- Helpers -----------------------------------------------------------------

void MoonrakerLink::resetSession() {
  for (Slot &slot : slots_) slot = Slot{0, Pending::None, 0};
  subscribed_ = false;
  phase_ = Phase::Identifying;
  klippy_ = policy::KlippyState::Unknown;
  scheduler_.reset();
}

// Discovery ended with no single paired heater. Any heat commanded through an
// earlier pairing is no longer ours to hold: command off ("never heats from
// Klipper" without a match) rather than waiting out the link-loss backstop.
void MoonrakerLink::goUnpaired(const char *error) {
  heater_ = "";
  lastError_ = error;
  phase_ = Phase::Unpaired;
  Serial.printf("[MOON] unpaired: %s, target off\n", error);
  if (unpairedCb_ != nullptr) unpairedCb_();
  if (targetCb_ != nullptr) targetCb_(0.0f);
}

bool MoonrakerLink::following() const {
  return !heater_.isEmpty() && phase_ != Phase::Unpaired;
}

bool MoonrakerLink::anyPending() const {
  for (const Slot &slot : slots_) {
    if (slot.kind != Pending::None) return true;
  }
  return false;
}

void MoonrakerLink::requestDisconnect() { disconnectRequested_ = true; }

}  // namespace iheaterlink::standalone
