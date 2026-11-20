#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure target-selection policy for iHeater Remote. No Arduino dependency.
namespace iheaterlink::policy {

constexpr uint8_t kRangeFloorC = 11;  // pulse codes 0 and 10 mean off
constexpr uint8_t kRangeCeilC = 90;   // stock iHeater MAX_AIR_TEMP
constexpr uint8_t kDefaultMinC = 45;  // stock iHeater PULSE_MIN_TARGET
constexpr uint8_t kDefaultMaxC = 85;

struct TargetRange {
  uint8_t minC;
  uint8_t maxC;
};

// kRangeFloorC <= minC <= maxC <= kRangeCeilC
bool validRange(int minC, int maxC);

// <= 0 means off (returns 0); otherwise clamped to [r.minC, r.maxC].
float clampTargetC(float requestedC, TargetRange r);

// --- Wi-Fi supervisor -------------------------------------------------------
constexpr uint32_t kStaReconnectMs = 30000;
constexpr uint32_t kApRetryMs = 120000;

enum class WifiAction : uint8_t { None, Reconnect, RetryFromAp };

// Station mode: reconnect every kStaReconnectMs while down. AP fallback with
// saved credentials: retry the saved network every kApRetryMs.
WifiAction wifiSupervisorAction(bool apMode, bool hasCredentials,
                                bool staConnected, uint32_t nowMs,
                                uint32_t lastAttemptMs);

// --- Printer link loss ------------------------------------------------------
// Backstop for a printer that never comes back: heat already commanded by the
// printer is held through short outages and cut after this long.
constexpr uint32_t kLinkLossGraceMs = 600000;

bool cutHeatForLinkLoss(bool printerSourceActive, bool heatingFromPrinter,
                        bool linkHealthy, uint32_t nowMs,
                        uint32_t linkDownSinceMs, uint32_t graceMs);

// --- Wi-Fi TX power ---------------------------------------------------------
// Arduino wifi_power_t values (quarter-dBm): 8.5, 11, 13, 15, 17, 19.5 dBm.
bool validTxPower(int quarterDbm);

// --- Pairing, discovery, link health ----------------------------------------
constexpr size_t kPairTokenLen = 8;

// Upper-cases and drops '-'. False for null, wrong length or characters
// outside the alphabet (Crockford base32: no I, L, O, U).
bool normalizePairToken(const char *in, char out[kPairTokenLen + 1]);

// Low 40 bits, 5 per character, most significant first.
void pairTokenFromBits(uint64_t bits, char out[kPairTokenLen + 1]);

// "7K3P9QXM" -> "7K3P-9QXM"
void formatPairToken(const char *normalized, char out[kPairTokenLen + 2]);

// "heater_virtual <name>" with a non-empty name.
bool isHeaterVirtualObject(const char *name);

constexpr int kHeaterNone = -1;
constexpr int kHeaterDuplicate = -2;

// Index of the single heater whose token matches myToken (both normalized;
// null or invalid entries skipped), else kHeaterNone or kHeaterDuplicate.
int matchPairedHeater(const char *const *tokens, size_t count,
                      const char *myToken);

enum class KlippyState : uint8_t { Unknown, Ready, NotReady };

bool moonrakerLinkHealthy(bool socketUp, bool subscribed, KlippyState klippy);

constexpr uint32_t kBackoffInitialMs = 2000;
constexpr uint32_t kBackoffMaxMs = 30000;

// 0 gives the initial value, then doubles up to the cap.
uint32_t nextBackoffMs(uint32_t currentMs);

// Wrap-safe: true once intervalMs has elapsed since sinceMs.
bool intervalElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs);

uint16_t defaultMoonrakerPort(bool tls);  // 443 with TLS, else 7125

constexpr uint32_t kPingIntervalMs = 10000;
constexpr uint32_t kPongTimeoutMs = 5000;
constexpr uint8_t kMissedPongs = 2;
constexpr uint32_t kQueryIntervalMs = 30000;
constexpr uint32_t kRequestTimeoutMs = 5000;

// --- Setup form and diagnostics ---------------------------------------------
// Keep the stored API key only when the form left the field blank and the
// endpoint (host, port, TLS) is unchanged: a key must never follow the Remote
// to a host it was not entered for.
bool keepStoredApiKey(bool endpointChanged, bool keyFieldBlank);

enum class HandshakeHint : uint8_t { ApiKey, TurnTlsOn, CheckEndpoint };

// What a rejected WebSocket upgrade most likely means. 401/403: API key.
// 400 on port 443 with TLS off: plain HTTP sent to an HTTPS proxy.
// Anything else: the host, port, path or proxy.
HandshakeHint handshakeFailureHint(int httpCode, uint16_t port, bool tls);

}  // namespace iheaterlink::policy
