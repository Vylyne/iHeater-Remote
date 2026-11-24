#include "AuthCrypto.h"

#include <cstring>

namespace iheaterlink::policy {

namespace {

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}

constexpr size_t kBlock = 64;

}  // namespace

Sha256::Sha256() : used_(0), bits_(0) {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                   0xa54ff53a, 0x510e527f, 0x9b05688c,
                                   0x1f83d9ab, 0x5be0cd19};
  memcpy(h_, init, sizeof(h_));
  memset(buf_, 0, sizeof(buf_));
}

void Sha256::block(const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
           (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
  uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + kK[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const uint8_t *data, size_t len) {
  bits_ += uint64_t(len) * 8;
  while (len > 0) {
    size_t n = kBlock - used_;
    if (n > len) n = len;
    memcpy(buf_ + used_, data, n);
    used_ += n;
    data += n;
    len -= n;
    if (used_ == kBlock) {
      block(buf_);
      used_ = 0;
    }
  }
}

void Sha256::finish(uint8_t out[kSha256Len]) {
  uint64_t bits = bits_;
  buf_[used_++] = 0x80;
  if (used_ > kBlock - 8) {
    memset(buf_ + used_, 0, kBlock - used_);
    block(buf_);
    used_ = 0;
  }
  memset(buf_ + used_, 0, kBlock - 8 - used_);
  for (int i = 0; i < 8; i++) buf_[kBlock - 1 - i] = uint8_t(bits >> (8 * i));
  block(buf_);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = uint8_t(h_[i] >> 24);
    out[4 * i + 1] = uint8_t(h_[i] >> 16);
    out[4 * i + 2] = uint8_t(h_[i] >> 8);
    out[4 * i + 3] = uint8_t(h_[i]);
  }
}

void sha256(const uint8_t *data, size_t len, uint8_t out[kSha256Len]) {
  Sha256 h;
  h.update(data, len);
  h.finish(out);
}

void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data,
                size_t len, uint8_t out[kSha256Len]) {
  uint8_t k[kBlock] = {0};
  if (keyLen > kBlock) {
    sha256(key, keyLen, k);
  } else if (keyLen > 0) {
    memcpy(k, key, keyLen);
  }
  uint8_t pad[kBlock];
  for (size_t i = 0; i < kBlock; i++) pad[i] = k[i] ^ 0x36;
  uint8_t inner[kSha256Len];
  Sha256 hi;
  hi.update(pad, kBlock);
  hi.update(data, len);
  hi.finish(inner);
  for (size_t i = 0; i < kBlock; i++) pad[i] = k[i] ^ 0x5c;
  Sha256 ho;
  ho.update(pad, kBlock);
  ho.update(inner, kSha256Len);
  ho.finish(out);
}

void pbkdf2Sha256(const uint8_t *pw, size_t pwLen, const uint8_t *salt,
                  size_t saltLen, uint32_t iterations, uint8_t *out,
                  size_t outLen) {
  // A zero count is treated as one so that T_i is never left unset.
  if (iterations == 0) iterations = 1;
  uint32_t index = 1;
  while (outLen > 0) {
    // U_1 = HMAC(P, S || INT(i)), with the salt streamed in two pieces.
    uint8_t k[kBlock] = {0};
    if (pwLen > kBlock) {
      sha256(pw, pwLen, k);
    } else if (pwLen > 0) {
      memcpy(k, pw, pwLen);
    }
    uint8_t pad[kBlock];
    uint8_t be[4] = {uint8_t(index >> 24), uint8_t(index >> 16),
                     uint8_t(index >> 8), uint8_t(index)};
    for (size_t i = 0; i < kBlock; i++) pad[i] = k[i] ^ 0x36;
    Sha256 hi;
    hi.update(pad, kBlock);
    hi.update(salt, saltLen);
    hi.update(be, 4);
    uint8_t inner[kSha256Len];
    hi.finish(inner);
    for (size_t i = 0; i < kBlock; i++) pad[i] = k[i] ^ 0x5c;
    Sha256 ho;
    ho.update(pad, kBlock);
    ho.update(inner, kSha256Len);
    uint8_t u[kSha256Len];
    ho.finish(u);

    uint8_t t[kSha256Len];
    memcpy(t, u, kSha256Len);
    for (uint32_t c = 1; c < iterations; c++) {
      hmacSha256(pw, pwLen, u, kSha256Len, u);
      for (size_t i = 0; i < kSha256Len; i++) t[i] ^= u[i];
    }
    size_t n = outLen < kSha256Len ? outLen : kSha256Len;
    memcpy(out, t, n);
    out += n;
    outLen -= n;
    index++;
  }
}

bool equalConstantTime(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}

void toHex(const uint8_t *in, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 0x0f];
  }
  out[2 * len] = '\0';
}

namespace {
int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

bool fromHex(const char *in, uint8_t *out, size_t len) {
  for (size_t i = 0; i < 2 * len; i++) {
    if (in[i] == '\0') return false;
  }
  if (in[2 * len] != '\0') return false;
  for (size_t i = 0; i < len; i++) {
    int hi = hexValue(in[2 * i]);
    int lo = hexValue(in[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

}  // namespace iheaterlink::policy
