#include "AuthState.h"

#include <cstring>

#include "RemotePolicy.h"

namespace iheaterlink::policy {

bool validIterations(uint32_t n) {
  return n >= kMinIterations && n <= kMaxIterations;
}

bool proofMatches(const uint8_t verifier[kVerifierLen],
                  const uint8_t nonce[kNonceLen],
                  const uint8_t proof[kSha256Len]) {
  uint8_t expected[kSha256Len];
  hmacSha256(verifier, kVerifierLen, nonce, kNonceLen, expected);
  return equalConstantTime(expected, proof, kSha256Len);
}

// ---- NonceTable ----

void NonceTable::add(const uint8_t nonce[kNonceLen], uint32_t nowMs) {
  if (count_ == kMaxNonces) {
    memmove(&slots_[0], &slots_[1], (kMaxNonces - 1) * sizeof(Slot));
    count_--;
  }
  memcpy(slots_[count_].nonce, nonce, kNonceLen);
  slots_[count_].addedMs = nowMs;
  count_++;
}

bool NonceTable::take(const uint8_t nonce[kNonceLen], uint32_t nowMs) {
  for (size_t i = 0; i < count_; i++) {
    if (!equalConstantTime(slots_[i].nonce, nonce, kNonceLen)) continue;
    const bool fresh = !intervalElapsed(nowMs, slots_[i].addedMs, kNonceLifeMs);
    memmove(&slots_[i], &slots_[i + 1], (count_ - i - 1) * sizeof(Slot));
    count_--;
    return fresh;
  }
  return false;
}

void NonceTable::clear() {
  memset(slots_, 0, sizeof(slots_));
  count_ = 0;
}

// ---- LoginLockout ----

bool LoginLockout::lockedNow(const Entry &e, uint32_t nowMs) const {
  return e.failures >= kLockoutFailures &&
         !intervalElapsed(nowMs, e.lastFailureMs, kLockoutMs);
}

uint32_t LoginLockout::retryAfterMs(uint32_t client, uint32_t nowMs) const {
  for (const Entry &e : entries_) {
    if (e.used && e.client == client) {
      if (!lockedNow(e, nowMs)) return 0;
      return kLockoutMs - static_cast<uint32_t>(nowMs - e.lastFailureMs);
    }
  }
  return 0;
}

void LoginLockout::failure(uint32_t client, uint32_t nowMs) {
  Entry *target = nullptr;
  for (Entry &e : entries_) {
    if (e.used && e.client == client) {
      target = &e;
      break;
    }
  }
  if (target == nullptr) {
    // A free slot, else the unlocked entry whose last failure is oldest.
    // Only when every entry is locked is the oldest locked one replaced.
    Entry *oldest = nullptr;
    bool oldestLocked = true;
    uint32_t oldestAge = 0;
    for (Entry &e : entries_) {
      if (!e.used) {
        target = &e;
        break;
      }
      const bool locked = lockedNow(e, nowMs);
      const uint32_t age = static_cast<uint32_t>(nowMs - e.lastFailureMs);
      const bool better = oldest == nullptr ||
                          (oldestLocked && !locked) ||
                          (locked == oldestLocked && age > oldestAge);
      if (better) {
        oldest = &e;
        oldestLocked = locked;
        oldestAge = age;
      }
    }
    if (target == nullptr) target = oldest;
    *target = Entry{};
    target->used = true;
    target->client = client;
  }
  if (lockedNow(*target, nowMs)) return;  // already locked: no change
  if (target->failures >= kLockoutFailures) target->failures = 0;  // lockout ended
  target->failures++;
  target->lastFailureMs = nowMs;
}

void LoginLockout::success(uint32_t client) {
  for (Entry &e : entries_) {
    if (e.used && e.client == client) e = Entry{};
  }
}

void LoginLockout::clear() { memset(entries_, 0, sizeof(entries_)); }

// ---- SessionTable ----

int SessionTable::find(const uint8_t token[kSessionTokenLen]) const {
  uint8_t hash[kSha256Len];
  sha256(token, kSessionTokenLen, hash);
  int found = -1;
  for (size_t i = 0; i < count_; i++) {
    if (equalConstantTime(slots_[i].hash, hash, kSha256Len)) {
      found = static_cast<int>(i);
    }
  }
  return found;
}

void SessionTable::removeAt(size_t i) {
  memmove(&slots_[i], &slots_[i + 1], (count_ - i - 1) * sizeof(Slot));
  count_--;
  memset(&slots_[count_], 0, sizeof(Slot));
}

void SessionTable::add(const uint8_t token[kSessionTokenLen], uint32_t nowHour) {
  const int existing = find(token);
  if (existing >= 0) removeAt(static_cast<size_t>(existing));
  if (count_ == kMaxSessions) removeAt(0);
  sha256(token, kSessionTokenLen, slots_[count_].hash);
  slots_[count_].loginHour = nowHour;
  count_++;
}

SessionTable::Check SessionTable::check(const uint8_t token[kSessionTokenLen],
                                        uint32_t nowHour) {
  const int i = find(token);
  if (i < 0) return Check::Unknown;
  const uint32_t age = static_cast<uint32_t>(nowHour - slots_[i].loginHour);
  if (age >= kSessionMaxHours) {
    removeAt(static_cast<size_t>(i));
    return Check::Expired;
  }
  return Check::Valid;
}

bool SessionTable::remove(const uint8_t token[kSessionTokenLen]) {
  const int i = find(token);
  if (i < 0) return false;
  removeAt(static_cast<size_t>(i));
  return true;
}

void SessionTable::clear() {
  memset(slots_, 0, sizeof(slots_));
  count_ = 0;
}

void SessionTable::toBlob(uint8_t out[kBlobLen]) const {
  memset(out, 0, kBlobLen);
  for (size_t i = 0; i < count_; i++) {
    uint8_t *p = out + i * (1 + kSha256Len + 4);
    p[0] = 1;
    memcpy(p + 1, slots_[i].hash, kSha256Len);
    const uint32_t h = slots_[i].loginHour;
    p += 1 + kSha256Len;
    p[0] = static_cast<uint8_t>(h);
    p[1] = static_cast<uint8_t>(h >> 8);
    p[2] = static_cast<uint8_t>(h >> 16);
    p[3] = static_cast<uint8_t>(h >> 24);
  }
}

bool SessionTable::fromBlob(const uint8_t *in, size_t len) {
  clear();
  if (in == nullptr || len != kBlobLen) return false;
  for (size_t i = 0; i < kMaxSessions; i++) {
    const uint8_t *p = in + i * (1 + kSha256Len + 4);
    if (p[0] != 1) continue;
    memcpy(slots_[count_].hash, p + 1, kSha256Len);
    p += 1 + kSha256Len;
    slots_[count_].loginHour = static_cast<uint32_t>(p[0]) |
                               static_cast<uint32_t>(p[1]) << 8 |
                               static_cast<uint32_t>(p[2]) << 16 |
                               static_cast<uint32_t>(p[3]) << 24;
    count_++;
  }
  return true;
}

// ---- RunningHours ----

void RunningHours::begin(uint32_t storedHours, uint32_t nowMs) {
  hours_ = storedHours;
  lastMs_ = nowMs;
}

bool RunningHours::tick(uint32_t nowMs) {
  if (!intervalElapsed(nowMs, lastMs_, kHourMs)) return false;
  hours_++;
  lastMs_ += kHourMs;
  return true;
}

}  // namespace iheaterlink::policy
