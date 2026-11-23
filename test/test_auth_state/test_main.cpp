#include <unity.h>

#include <cstdint>
#include <cstring>

#include "AuthState.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

static void fill(uint8_t *p, size_t n, uint8_t seed) {
  for (size_t i = 0; i < n; i++) p[i] = static_cast<uint8_t>(seed + i * 7);
}

// ---- Iterations and proof ----

void test_valid_iterations() {
  TEST_ASSERT_FALSE(validIterations(4999));
  TEST_ASSERT_TRUE(validIterations(5000));
  TEST_ASSERT_TRUE(validIterations(10000));
  TEST_ASSERT_TRUE(validIterations(20000));
  TEST_ASSERT_FALSE(validIterations(20001));
}

void test_proof_matches() {
  uint8_t v[kVerifierLen], nonce[kNonceLen], proof[kSha256Len];
  fill(v, sizeof v, 1);
  fill(nonce, sizeof nonce, 50);
  hmacSha256(v, sizeof v, nonce, sizeof nonce, proof);
  TEST_ASSERT_TRUE(proofMatches(v, nonce, proof));
  proof[5] ^= 0x04;
  TEST_ASSERT_FALSE(proofMatches(v, nonce, proof));
}

// ---- Nonces ----

void test_nonce_single_use() {
  NonceTable t;
  uint8_t n[kNonceLen];
  fill(n, sizeof n, 1);
  t.add(n, 0);
  TEST_ASSERT_TRUE(t.take(n, 1000));
  TEST_ASSERT_FALSE(t.take(n, 1001));
}

void test_nonce_expires() {
  NonceTable t;
  uint8_t n[kNonceLen];
  fill(n, sizeof n, 1);
  t.add(n, 0);
  TEST_ASSERT_FALSE(t.take(n, 60000));
  TEST_ASSERT_FALSE(t.take(n, 60001));  // removed all the same
}

void test_nonce_unknown_refused() {
  NonceTable t;
  uint8_t a[kNonceLen], b[kNonceLen];
  fill(a, sizeof a, 1);
  fill(b, sizeof b, 2);
  t.add(a, 0);
  TEST_ASSERT_FALSE(t.take(b, 10));
}

void test_nonce_oldest_dropped() {
  NonceTable t;
  uint8_t n[5][kNonceLen];
  for (int i = 0; i < 5; i++) {
    fill(n[i], kNonceLen, static_cast<uint8_t>(i + 1));
    t.add(n[i], static_cast<uint32_t>(i));
  }
  TEST_ASSERT_FALSE(t.take(n[0], 10));
  TEST_ASSERT_TRUE(t.take(n[4], 10));
}

void test_nonce_wraps() {
  NonceTable t;
  uint8_t n[kNonceLen];
  fill(n, sizeof n, 1);
  t.add(n, 0xFFFFFF00u);
  TEST_ASSERT_TRUE(t.take(n, 0x100u));
}

// ---- Lockout ----

void test_lockout_after_five() {
  LoginLockout l;
  const uint32_t t = 1000;
  for (int i = 0; i < 4; i++) {
    l.failure(7, t + i);
    TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(7, t + i));
  }
  l.failure(7, t + 4);
  TEST_ASSERT_EQUAL_UINT32(30000, l.retryAfterMs(7, t + 4));
  TEST_ASSERT_EQUAL_UINT32(1, l.retryAfterMs(7, t + 4 + 29999));
  TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(7, t + 4 + 30000));
}

void test_lockout_count_restarts() {
  LoginLockout l;
  for (int i = 0; i < 5; i++) l.failure(7, 1000);
  TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(7, 31000));
  l.failure(7, 31000);
  TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(7, 31000));
  // Four more reach five in all since the restart, and lock again.
  for (int i = 0; i < 4; i++) l.failure(7, 31001);
  TEST_ASSERT_TRUE(l.retryAfterMs(7, 31001) > 0);
}

void test_lockout_success_clears() {
  LoginLockout l;
  for (int i = 0; i < 4; i++) l.failure(7, 100);
  l.success(7);
  for (int i = 0; i < 4; i++) l.failure(7, 200);
  TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(7, 200));
}

void test_lockout_per_client() {
  LoginLockout l;
  for (int i = 0; i < 5; i++) l.failure(1, 500);
  TEST_ASSERT_TRUE(l.retryAfterMs(1, 500) > 0);
  TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(2, 500));
}

void test_lockout_table_full_drops_oldest() {
  LoginLockout l;
  for (uint32_t c = 1; c <= 9; c++) l.failure(c, c * 10);
  // Client 1 was dropped for client 9, so its count restarts.
  for (int i = 0; i < 4; i++) l.failure(1, 200 + i);
  TEST_ASSERT_EQUAL_UINT32(0, l.retryAfterMs(1, 204));
}

// ---- Sessions ----

void test_session_add_check() {
  SessionTable s;
  uint8_t a[kSessionTokenLen], b[kSessionTokenLen];
  fill(a, sizeof a, 1);
  fill(b, sizeof b, 2);
  s.add(a, 5);
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Valid, (int)s.check(a, 6));
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Unknown, (int)s.check(b, 6));
}

void test_session_expiry_720h() {
  SessionTable s;
  uint8_t a[kSessionTokenLen];
  fill(a, sizeof a, 1);
  s.add(a, 10);
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Valid, (int)s.check(a, 729));
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Expired, (int)s.check(a, 730));
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Unknown, (int)s.check(a, 730));
}

void test_session_future_login_expired() {
  SessionTable s;
  uint8_t a[kSessionTokenLen];
  fill(a, sizeof a, 1);
  s.add(a, 100);
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Expired, (int)s.check(a, 50));
}

void test_session_evicts_oldest() {
  SessionTable s;
  uint8_t t[5][kSessionTokenLen];
  for (int i = 0; i < 5; i++) {
    fill(t[i], kSessionTokenLen, static_cast<uint8_t>(i + 1));
    s.add(t[i], 1);
  }
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Unknown, (int)s.check(t[0], 1));
  for (int i = 1; i < 5; i++) {
    TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Valid, (int)s.check(t[i], 1));
  }
}

void test_session_remove_and_clear() {
  SessionTable s;
  uint8_t a[kSessionTokenLen], b[kSessionTokenLen];
  fill(a, sizeof a, 1);
  fill(b, sizeof b, 2);
  s.add(a, 1);
  s.add(b, 1);
  TEST_ASSERT_TRUE(s.remove(a));
  TEST_ASSERT_FALSE(s.remove(a));
  TEST_ASSERT_EQUAL_UINT32(1, s.count());
  s.clear();
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Unknown, (int)s.check(b, 1));
}

void test_session_blob_round_trip() {
  SessionTable s;
  uint8_t t[3][kSessionTokenLen];
  for (int i = 0; i < 3; i++) {
    fill(t[i], kSessionTokenLen, static_cast<uint8_t>(i + 1));
    s.add(t[i], 100 + i);
  }
  uint8_t blob[SessionTable::kBlobLen];
  s.toBlob(blob);
  SessionTable r;
  TEST_ASSERT_TRUE(r.fromBlob(blob, sizeof blob));
  TEST_ASSERT_EQUAL_UINT32(3, r.count());
  uint8_t blob2[SessionTable::kBlobLen];
  r.toBlob(blob2);
  TEST_ASSERT_EQUAL_MEMORY(blob, blob2, sizeof blob);
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Valid, (int)r.check(t[i], 110));
  }
  // Order survives: two more adds evict the first token, not the second.
  uint8_t x[kSessionTokenLen], y[kSessionTokenLen];
  fill(x, sizeof x, 40);
  fill(y, sizeof y, 41);
  r.add(x, 110);
  r.add(y, 110);
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Unknown, (int)r.check(t[0], 110));
  TEST_ASSERT_EQUAL_INT((int)SessionTable::Check::Valid, (int)r.check(t[1], 110));

  TEST_ASSERT_FALSE(r.fromBlob(blob, sizeof blob - 1));
  TEST_ASSERT_EQUAL_UINT32(0, r.count());
}

void test_session_stores_hash_not_token() {
  SessionTable s;
  uint8_t a[kSessionTokenLen];
  fill(a, sizeof a, 9);
  s.add(a, 1);
  uint8_t blob[SessionTable::kBlobLen];
  s.toBlob(blob);
  bool found = false;
  for (size_t i = 0; i + kSessionTokenLen <= sizeof blob; i++) {
    if (memcmp(blob + i, a, kSessionTokenLen) == 0) found = true;
  }
  TEST_ASSERT_FALSE(found);
}

// ---- Running hours ----

void test_running_hours_tick() {
  RunningHours h;
  h.begin(10, 0);
  TEST_ASSERT_FALSE(h.tick(3599999));
  TEST_ASSERT_TRUE(h.tick(3600000));
  TEST_ASSERT_EQUAL_UINT32(11, h.hours());
  TEST_ASSERT_FALSE(h.tick(3600001));
}

void test_running_hours_wrap() {
  RunningHours h;
  h.begin(5, 0xFFFFFFFFu - 1000);
  TEST_ASSERT_TRUE(h.tick(3600000u - 1001));
  TEST_ASSERT_EQUAL_UINT32(6, h.hours());
}

void test_running_hours_survive_restart() {
  RunningHours a;
  a.begin(10, 0);
  TEST_ASSERT_TRUE(a.tick(3600000));
  RunningHours b;
  b.begin(a.hours(), 0);
  TEST_ASSERT_EQUAL_UINT32(11, b.hours());
  TEST_ASSERT_TRUE(b.tick(3600000));
  TEST_ASSERT_EQUAL_UINT32(12, b.hours());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_iterations);
  RUN_TEST(test_proof_matches);
  RUN_TEST(test_nonce_single_use);
  RUN_TEST(test_nonce_expires);
  RUN_TEST(test_nonce_unknown_refused);
  RUN_TEST(test_nonce_oldest_dropped);
  RUN_TEST(test_nonce_wraps);
  RUN_TEST(test_lockout_after_five);
  RUN_TEST(test_lockout_count_restarts);
  RUN_TEST(test_lockout_success_clears);
  RUN_TEST(test_lockout_per_client);
  RUN_TEST(test_lockout_table_full_drops_oldest);
  RUN_TEST(test_session_add_check);
  RUN_TEST(test_session_expiry_720h);
  RUN_TEST(test_session_future_login_expired);
  RUN_TEST(test_session_evicts_oldest);
  RUN_TEST(test_session_remove_and_clear);
  RUN_TEST(test_session_blob_round_trip);
  RUN_TEST(test_session_stores_hash_not_token);
  RUN_TEST(test_running_hours_tick);
  RUN_TEST(test_running_hours_wrap);
  RUN_TEST(test_running_hours_survive_restart);
  return UNITY_END();
}
