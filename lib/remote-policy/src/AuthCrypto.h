#pragma once

#include <cstddef>
#include <cstdint>

// SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 with no heap use. One natively
// tested copy defines the vectors that the browser copy (auth.js) must match.
namespace iheaterlink::policy {

constexpr size_t kSha256Len = 32;

class Sha256 {
 public:
  Sha256();
  void update(const uint8_t *data, size_t len);
  void finish(uint8_t out[kSha256Len]);  // then the object is spent

 private:
  void block(const uint8_t *p);

  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_;
  uint64_t bits_;
};

void sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Len]);

void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data,
                size_t len, uint8_t out[kSha256Len]);

void pbkdf2Sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt,
                  size_t saltLen, uint32_t iterations, uint8_t *out,
                  size_t outLen);

bool equalConstantTime(const uint8_t *a, const uint8_t *b, size_t len);

// Lower case; out holds 2 * len + 1 bytes.
void toHex(const uint8_t *in, size_t len, char *out);
// Exactly 2 * len hex digits, either case.
bool fromHex(const char *in, uint8_t *out, size_t len);

}  // namespace iheaterlink::policy
