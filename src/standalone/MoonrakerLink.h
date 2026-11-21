#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <RemotePolicy.h>
#include <ReportPolicy.h>
#include <WebSocketsClient.h>

namespace iheaterlink::standalone {

struct MoonrakerSettings {
  String host;
  uint16_t port = 7125;
  bool tls = false;
  String apiKey;
};

// The Remote's own Moonraker client: one WebSocket (ws:// or certificate-
// checked wss://), JSON-RPC identify, then discovery of the heater_iheater_remote
// whose remote_token matches this Remote's pairing token, then a target-only
// subscription. Reconnects with backoff; a 30 s query re-syncs the target and
// doubles as a liveness check.
class MoonrakerLink {
 public:
  using TargetCallback = void (*)(float targetC);

  MoonrakerLink();
  MoonrakerLink(const MoonrakerLink &) = delete;
  MoonrakerLink &operator=(const MoonrakerLink &) = delete;

  void onTarget(TargetCallback cb);
  void configure(const MoonrakerSettings &s, const char *pairToken);  // (re)starts
  void stop();  // disconnect, no reconnect
  void loop();  // call every loop() pass
  bool healthy() const;
  const char *stateString() const;  // disconnected/connecting/klippy-not-ready/ready
  const String &heaterObject() const;
  const String &lastError() const;

  // Sends the iHeater's readings to the Klipper plugin when one is due. Call
  // every loop() pass; it does nothing until the link is subscribed.
  void report(const policy::ReportInputs &in);
  const policy::ReportScheduler &reports() const { return scheduler_; }

 private:
  enum class Pending : uint8_t { None, Identify, List, Discover, Subscribe, Query, Report };

  // Where the session is between socket connect and a live subscription.
  enum class Phase : uint8_t {
    Identifying,  // identify sent, no reply yet
    Discovering,  // list/token query in flight, or waiting for Klipper ready
    Unpaired,     // discovery finished without a single matching heater
    Subscribed,   // subscribe sent or live
  };

  struct Slot {
    uint32_t id;
    Pending kind;
    uint32_t sentMs;
  };

  static constexpr size_t kMaxPending = 4;
  static constexpr size_t kMaxHeaters = 8;

  void onEvent(WStype_t type, uint8_t *payload, size_t length);
  void onConnected();
  void onDisconnected(const char *reason, size_t length);
  void onText(uint8_t *payload, size_t length);
  void handleNotification(const char *method, JsonVariantConst params);
  void handleResult(Pending kind, JsonVariantConst result);
  void handleError(Pending kind, int code, const char *message);

  uint32_t call(const char *method, JsonVariantConst params, Pending kind);
  void sendIdentify();
  void startDiscovery();  // step 2: printer.objects.list
  void sendTokenQuery(JsonArrayConst objects);
  void sendHeaterRequest(const char *method, Pending kind);
  void applyStatus(JsonObjectConst status);
  void resetSession();
  void goUnpaired(const char *error);  // clears the heater and commands off
  bool following() const;  // a paired heater is known (not Unpaired)
  bool anyPending() const;
  void requestDisconnect();  // runs from loop(), outside the library's stack

  WebSocketsClient ws_;
  MoonrakerSettings settings_;
  char token_[policy::kPairTokenLen + 1] = {};
  String rawToken_;
  String extraHeaders_;
  TargetCallback targetCb_ = nullptr;

  bool running_ = false;
  bool socketUp_ = false;
  bool attemptConnected_ = false;  // this connection attempt reached CONNECTED
  bool disconnectRequested_ = false;
  bool subscribed_ = false;
  Phase phase_ = Phase::Identifying;
  policy::KlippyState klippy_ = policy::KlippyState::Unknown;
  uint32_t backoffMs_ = 0;
  uint32_t nextId_ = 1;
  uint32_t lastQueryMs_ = 0;
  Slot slots_[kMaxPending] = {};

  String heater_;
  String lastError_;

  policy::ReportScheduler scheduler_;
  uint32_t lastRefusedLogMs_ = 0;
  bool refusedLogged_ = false;
};

}  // namespace iheaterlink::standalone
