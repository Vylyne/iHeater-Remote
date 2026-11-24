#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "AuthCrypto.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

static const uint8_t *u8(const char *s) {
  return reinterpret_cast<const uint8_t *>(s);
}

static void expectHex(const char *expected, const uint8_t *d, size_t len) {
  char hex[129];
  toHex(d, len, hex);
  TEST_ASSERT_EQUAL_STRING(expected, hex);
}

static void expectSha(const char *msg, const char *expected) {
  uint8_t out[kSha256Len];
  sha256(u8(msg), strlen(msg), out);
  expectHex(expected, out, kSha256Len);
}

void test_sha256_empty() {
  expectSha("",
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void test_sha256_abc() {
  expectSha("abc",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void test_sha256_two_block() {
  expectSha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void test_sha256_million_a() {
  uint8_t chunk[1000];
  memset(chunk, 'a', sizeof(chunk));
  Sha256 h;
  for (int i = 0; i < 1000; i++) h.update(chunk, sizeof(chunk));
  uint8_t out[kSha256Len];
  h.finish(out);
  expectHex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            out, kSha256Len);
}

void test_sha256_split_updates() {
  Sha256 h;
  h.update(u8("a"), 1);
  h.update(u8("bc"), 2);
  uint8_t out[kSha256Len];
  h.finish(out);
  expectHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            out, kSha256Len);
}

void test_hmac_rfc4231_case1() {
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  uint8_t out[kSha256Len];
  hmacSha256(key, sizeof(key), u8("Hi There"), 8, out);
  expectHex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
            out, kSha256Len);
}

void test_hmac_rfc4231_case2() {
  const char *data = "what do ya want for nothing?";
  uint8_t out[kSha256Len];
  hmacSha256(u8("Jefe"), 4, u8(data), strlen(data), out);
  expectHex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            out, kSha256Len);
}

void test_hmac_rfc4231_case6() {
  uint8_t key[131];
  memset(key, 0xaa, sizeof(key));
  const char *data = "Test Using Larger Than Block-Size Key - Hash Key First";
  uint8_t out[kSha256Len];
  hmacSha256(key, sizeof(key), u8(data), strlen(data), out);
  expectHex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
            out, kSha256Len);
}

void test_pbkdf2_c1() {
  uint8_t out[32];
  pbkdf2Sha256(u8("password"), 8, u8("salt"), 4, 1, out, sizeof(out));
  expectHex("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
            out, sizeof(out));
}

void test_pbkdf2_c2() {
  uint8_t out[32];
  pbkdf2Sha256(u8("password"), 8, u8("salt"), 4, 2, out, sizeof(out));
  expectHex("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43",
            out, sizeof(out));
}

void test_pbkdf2_c4096() {
  uint8_t out[32];
  pbkdf2Sha256(u8("password"), 8, u8("salt"), 4, 4096, out, sizeof(out));
  expectHex("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
            out, sizeof(out));
}

void test_pbkdf2_long_output() {
  const char *p = "passwordPASSWORDpassword";
  const char *s = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
  uint8_t out[40];
  pbkdf2Sha256(u8(p), strlen(p), u8(s), strlen(s), 4096, out, sizeof(out));
  expectHex(
      "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c63551"
      "8c7dac47e9",
      out, sizeof(out));
}

void test_equal_constant_time() {
  uint8_t a[4] = {1, 2, 3, 4};
  uint8_t b[4] = {1, 2, 3, 4};
  TEST_ASSERT_TRUE(equalConstantTime(a, b, 4));
  b[3] ^= 0x01;
  TEST_ASSERT_FALSE(equalConstantTime(a, b, 4));
  TEST_ASSERT_TRUE(equalConstantTime(a, b, 0));
}

void test_hex_round_trip() {
  const uint8_t in[3] = {0x00, 0xAB, 0xFF};
  char hex[7];
  toHex(in, 3, hex);
  TEST_ASSERT_EQUAL_STRING("00abff", hex);
  uint8_t back[3];
  TEST_ASSERT_TRUE(fromHex("00ABff", back, 3));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(in, back, 3);
}

void test_hex_rejects_bad_input() {
  uint8_t out[2];
  TEST_ASSERT_FALSE(fromHex("0g", out, 1));
  TEST_ASSERT_FALSE(fromHex("abc", out, 2));
  TEST_ASSERT_FALSE(fromHex("", out, 1));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_empty);
  RUN_TEST(test_sha256_abc);
  RUN_TEST(test_sha256_two_block);
  RUN_TEST(test_sha256_million_a);
  RUN_TEST(test_sha256_split_updates);
  RUN_TEST(test_hmac_rfc4231_case1);
  RUN_TEST(test_hmac_rfc4231_case2);
  RUN_TEST(test_hmac_rfc4231_case6);
  RUN_TEST(test_pbkdf2_c1);
  RUN_TEST(test_pbkdf2_c2);
  RUN_TEST(test_pbkdf2_c4096);
  RUN_TEST(test_pbkdf2_long_output);
  RUN_TEST(test_equal_constant_time);
  RUN_TEST(test_hex_round_trip);
  RUN_TEST(test_hex_rejects_bad_input);
  return UNITY_END();
}
