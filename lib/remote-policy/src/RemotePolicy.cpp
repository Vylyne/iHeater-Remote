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

bool isRemoteHeaterObject(const char *name) {
  static const char prefix[] = "heater_iheater_remote ";
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

bool suggestTlsForPort(uint16_t port, bool tls) { return !tls && port == 443; }

static uint32_t resumeCheck(const ResumeRecord &r) {
  uint32_t h = 2166136261u;
  auto feed = [&h](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 16777619u;
    }
  };
  feed(r.magic, 4);
  feed(r.mode, 1);
  feed(r.timerMinutes, 2);
  feed(r.remainingS, 4);
  return h;
}

void sealResume(ResumeRecord &r) {
  r.magic = kResumeMagic;
  r.check = resumeCheck(r);
}

bool resumeValid(const ResumeRecord &r, bool softwareReset, uint8_t maxMode) {
  if (!softwareReset) return false;
  if (r.magic != kResumeMagic || r.check != resumeCheck(r)) return false;
  if (r.mode > maxMode) return false;
  if (r.timerMinutes > 0 && r.remainingS == 0) return false;
  return true;
}

static uint32_t linkLossCheck(const LinkLossRecord &r) {
  uint32_t h = 2166136261u;
  auto feed = [&h](uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 16777619u;
    }
  };
  feed(r.magic, 4);
  feed(r.unhealthyMs, 4);
  return h;
}

void sealLinkLoss(LinkLossRecord &r, uint32_t unhealthyMs) {
  r.magic = kLinkLossMagic;
  r.unhealthyMs = unhealthyMs > kLinkLossGraceMs ? kLinkLossGraceMs : unhealthyMs;
  r.check = linkLossCheck(r);
}

void clearLinkLoss(LinkLossRecord &r) {
  r.magic = 0;
  r.unhealthyMs = 0;
  r.check = 0;
}

bool carriedLinkLoss(const LinkLossRecord &r, bool softwareReset, uint32_t &carriedMs) {
  carriedMs = 0;
  if (!softwareReset) return false;
  if (r.magic != kLinkLossMagic || r.check != linkLossCheck(r)) return false;
  if (r.unhealthyMs > kLinkLossGraceMs) return false;
  carriedMs = r.unhealthyMs;
  return true;
}

bool linkLossExpired(uint32_t carriedMs, uint32_t graceMs) { return carriedMs >= graceMs; }

uint32_t linkLossCarryPlus(uint32_t carriedMs, uint32_t elapsedMs) {
  if (carriedMs >= kLinkLossGraceMs) return kLinkLossGraceMs;
  if (elapsedMs >= kLinkLossGraceMs - carriedMs) return kLinkLossGraceMs;
  return carriedMs + elapsedMs;
}

uint32_t linkLossBaselineMs(uint32_t carriedMs) { return 0u - carriedMs; }

bool setupSaveStopsHeat(bool uartOutput, bool wasKlipper, bool nowKlipper,
                        bool bindingChanged, bool outputChanged) {
  if (outputChanged) return true;
  if (nowKlipper) return !wasKlipper || bindingChanged;
  return uartOutput ? wasKlipper : true;
}

}  // namespace iheaterlink::policy
