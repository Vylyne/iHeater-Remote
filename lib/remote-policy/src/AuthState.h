#pragma once

#include <cstddef>
#include <cstdint>

#include "AuthCrypto.h"
#include "AuthPolicy.h"

// Stateful tables for the web login: login nonces, the failed-login lockout,
// the session table and the running-hours counter that dates sessions. Fixed
// arrays and no heap use, so the native tests cover exactly what the
// firmware runs. All time comparisons are wrap-safe.
namespace iheaterlink::policy {

constexpr size_t kNonceLen = 16, kSaltLen = 16, kVerifierLen = 32;
constexpr uint32_t kMinIterations = 5000, kMaxIterations = 20000,
                   kDefaultIterations = 10000;
bool validIterations(uint32_t n);

// HMAC-SHA-256(verifier, nonce) against the client's proof, in constant time.
bool proofMatches(const uint8_t verifier[kVerifierLen],
                  const uint8_t nonce[kNonceLen],
                  const uint8_t proof[kSha256Len]);

constexpr size_t kMaxNonces = 4;
constexpr uint32_t kNonceLifeMs = 60000;

// Single-use login nonces. Slots are kept oldest first.
class NonceTable {
 public:
  void add(const uint8_t nonce[kNonceLen], uint32_t nowMs);  // drops the oldest when full
  // True once, if the nonce is younger than kNonceLifeMs. A known nonce is
  // removed whether it was fresh or expired.
  bool take(const uint8_t nonce[kNonceLen], uint32_t nowMs);
  void clear();

 private:
  struct Slot {
    uint8_t nonce[kNonceLen];
    uint32_t addedMs;
  };
  Slot slots_[kMaxNonces] = {};
  size_t count_ = 0;
};

constexpr uint8_t kLockoutFailures = 5;
constexpr uint32_t kLockoutMs = 30000;
constexpr size_t kLockoutClients = 8;

// Per-client failure counts. The fifth failure in a row locks that client
// for kLockoutMs, and the count starts again from 0 once the lockout ends.
// When the table is full, a new client replaces the unlocked entry whose last
// failure is oldest; a locked entry is replaced only when all are locked.
class LoginLockout {
 public:
  uint32_t retryAfterMs(uint32_t client, uint32_t nowMs) const;  // 0: may try now
  void failure(uint32_t client, uint32_t nowMs);
  void success(uint32_t client);
  void clear();

 private:
  struct Entry {
    uint32_t client;
    uint32_t lastFailureMs;
    uint8_t failures;
    bool used;
  };
  Entry entries_[kLockoutClients] = {};
  bool lockedNow(const Entry &e, uint32_t nowMs) const;
};

constexpr size_t kMaxSessions = 4;
constexpr uint32_t kSessionMaxHours = 720;

// Sessions are stored as SHA-256(token) with the running hour of the login,
// oldest first. The blob keeps that order and is what the firmware keeps in
// NVS: per slot a used byte, the 32-byte hash and the hour (little endian).
class SessionTable {
 public:
  enum class Check : uint8_t { Valid, Unknown, Expired };
  void add(const uint8_t token[kSessionTokenLen], uint32_t nowHour);  // evicts the oldest
  Check check(const uint8_t token[kSessionTokenLen], uint32_t nowHour);  // Expired also removes it
  bool remove(const uint8_t token[kSessionTokenLen]);
  void clear();
  size_t count() const { return count_; }
  static constexpr size_t kBlobLen = kMaxSessions * (1 + kSha256Len + 4);
  void toBlob(uint8_t out[kBlobLen]) const;
  bool fromBlob(const uint8_t *in, size_t len);  // false and empty on a wrong length

 private:
  struct Slot {
    uint8_t hash[kSha256Len];
    uint32_t loginHour;
  };
  Slot slots_[kMaxSessions] = {};
  size_t count_ = 0;
  int find(const uint8_t token[kSessionTokenLen]) const;
  void removeAt(size_t i);
};

constexpr uint32_t kHourMs = 3600000;

// Counts hours of uptime on top of the stored count, to date sessions without
// a clock. A partial hour at a restart is lost; that is accepted.
class RunningHours {
 public:
  void begin(uint32_t storedHours, uint32_t nowMs);
  bool tick(uint32_t nowMs);  // true once per elapsed hour, already counted: save hours()
  uint32_t hours() const { return hours_; }

 private:
  uint32_t hours_ = 0;
  uint32_t lastMs_ = 0;
};

}  // namespace iheaterlink::policy
