#include "RemotePolicy.h"

namespace iheaterlink::policy {

bool validRange(int minC, int maxC) {
  return minC >= kRangeFloorC && minC <= maxC && maxC <= kRangeCeilC;
}

float clampTargetC(float requestedC, TargetRange r) {
  if (requestedC <= 0.0f) {
    return 0.0f;
  }
  if (requestedC < r.minC) {
    return r.minC;
  }
  if (requestedC > r.maxC) {
    return r.maxC;
  }
  return requestedC;
}

WifiAction wifiSupervisorAction(bool apMode, bool hasCredentials,
                                bool staConnected, uint32_t nowMs,
                                uint32_t lastAttemptMs) {
  if (staConnected) {
    return WifiAction::None;
  }
  const uint32_t elapsed = static_cast<uint32_t>(nowMs - lastAttemptMs);
  if (!apMode) {
    return elapsed >= kStaReconnectMs ? WifiAction::Reconnect : WifiAction::None;
  }
  if (hasCredentials && elapsed >= kApRetryMs) {
    return WifiAction::RetryFromAp;
  }
  return WifiAction::None;
}

bool cutHeatForLinkLoss(bool printerSourceActive, bool heatingFromPrinter,
                        bool linkHealthy, uint32_t nowMs,
                        uint32_t linkDownSinceMs, uint32_t graceMs) {
  if (!printerSourceActive || !heatingFromPrinter || linkHealthy) {
    return false;
  }
  return static_cast<uint32_t>(nowMs - linkDownSinceMs) >= graceMs;
}

bool validTxPower(int quarterDbm) {
  switch (quarterDbm) {
  case 34:
  case 44:
  case 52:
  case 60:
  case 68:
  case 78:
    return true;
  default:
    return false;
  }
}

// --- Pairing, discovery, link health ----------------------------------------
static const char kPairAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool normalizePairToken(const char *in, char out[kPairTokenLen + 1]) {
  if (in == nullptr) return false;
  size_t n = 0;
  for (; *in != '\0'; ++in) {
    char c = *in;
    if (c == '-') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    bool ok = false;
    for (size_t i = 0; i < 32; ++i) {
      if (kPairAlphabet[i] == c) {
        ok = true;
        break;
      }
    }
    if (!ok || n >= kPairTokenLen) return false;
    out[n++] = c;
  }
  if (n != kPairTokenLen) return false;
  out[n] = '\0';
  return true;
}

void pairTokenFromBits(uint64_t bits, char out[kPairTokenLen + 1]) {
  for (size_t i = 0; i < kPairTokenLen; ++i) {
    out[i] = kPairAlphabet[(bits >> (35 - 5 * i)) & 31];
  }
  out[kPairTokenLen] = '\0';
}

void formatPairToken(const char *normalized, char out[kPairTokenLen + 2]) {
  for (size_t i = 0; i < 4; ++i) out[i] = normalized[i];
  out[4] = '-';
  for (size_t i = 4; i < kPairTokenLen; ++i) out[i + 1] = normalized[i];
  out[kPairTokenLen + 1] = '\0';
}

bool isHeaterVirtualObject(const char *name) {
  static const char prefix[] = "heater_virtual ";
  if (name == nullptr) return false;
  size_t i = 0;
  for (; prefix[i] != '\0'; ++i) {
    if (name[i] != prefix[i]) return false;
  }
  return name[i] != '\0';
}

int matchPairedHeater(const char *const *tokens, size_t count,
                      const char *myToken) {
  char mine[kPairTokenLen + 1];
  if (!normalizePairToken(myToken, mine)) return kHeaterNone;
  int found = kHeaterNone;
  for (size_t i = 0; i < count; ++i) {
    char theirs[kPairTokenLen + 1];
    if (!normalizePairToken(tokens[i], theirs)) continue;
    size_t j = 0;
    while (j <= kPairTokenLen && mine[j] == theirs[j]) ++j;
    if (j <= kPairTokenLen) continue;
    if (found != kHeaterNone) return kHeaterDuplicate;
    found = static_cast<int>(i);
  }
  return found;
}

bool moonrakerLinkHealthy(bool socketUp, bool subscribed, KlippyState klippy) {
  return socketUp && subscribed && klippy == KlippyState::Ready;
}

uint32_t nextBackoffMs(uint32_t currentMs) {
  if (currentMs == 0) return kBackoffInitialMs;
  if (currentMs >= kBackoffMaxMs / 2) return kBackoffMaxMs;
  return currentMs * 2;
}

bool intervalElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs) {
  return static_cast<uint32_t>(nowMs - sinceMs) >= intervalMs;
}

uint16_t defaultMoonrakerPort(bool tls) { return tls ? 443 : 7125; }

bool keepStoredApiKey(bool endpointChanged, bool keyFieldBlank) {
  return keyFieldBlank && !endpointChanged;
}

HandshakeHint handshakeFailureHint(int httpCode, uint16_t port, bool tls) {
  if (httpCode == 401 || httpCode == 403) return HandshakeHint::ApiKey;
  if (httpCode == 400 && port == 443 && !tls) return HandshakeHint::TurnTlsOn;
  return HandshakeHint::CheckEndpoint;
}

}  // namespace iheaterlink::policy
