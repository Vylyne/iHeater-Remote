#include <unity.h>

#include <cstdint>
#include <cstring>

#include "AuthPolicy.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

static const char *kHost = "192.168.1.5";
static const char *kHex = "00112233445566778899aabbccddeeff";

static bool origin(const char *o, const char *r, const char *h = kHost) {
  return originAllowed(o, r, h, false, "", "");
}

// ---- Request classes ----

void test_classify_public_routes() {
  const char *paths[] = {"/",           "/login",        "/auth.js",
                         "/api/auth/me", "/generate_204", "/nope"};
  for (const char *p : paths) {
    TEST_ASSERT_EQUAL_INT((int)RequestClass::Public,
                          (int)classifyRequest(p, false, true));
  }
}

void test_classify_login_endpoints() {
  TEST_ASSERT_EQUAL_INT(
      (int)RequestClass::LoginEndpoint,
      (int)classifyRequest("/api/auth/challenge", false, true));
  TEST_ASSERT_EQUAL_INT((int)RequestClass::LoginEndpoint,
                        (int)classifyRequest("/api/auth/login", true, true));
  TEST_ASSERT_EQUAL_INT(
      (int)RequestClass::LoginEndpoint,
      (int)classifyRequest("/api/auth/password", true, false));
}

void test_classify_password_post_once_set_is_action() {
  TEST_ASSERT_EQUAL_INT((int)RequestClass::Action,
                        (int)classifyRequest("/api/auth/password", true, true));
}

void test_classify_protected_pages() {
  TEST_ASSERT_EQUAL_INT((int)RequestClass::ProtectedPage,
                        (int)classifyRequest("/setup", false, true));
  TEST_ASSERT_EQUAL_INT((int)RequestClass::ProtectedPage,
                        (int)classifyRequest("/fw", false, false));
}

void test_classify_actions() {
  const char *paths[] = {"/api/mode", "/api/auth/logout", "/api/override/end",
                         "/api/ota", "/nope"};
  for (const char *p : paths) {
    TEST_ASSERT_EQUAL_INT((int)RequestClass::Action,
                          (int)classifyRequest(p, true, true));
  }
}

// ---- Origin check ----

void test_origin_matches_host() {
  TEST_ASSERT_TRUE(origin("http://192.168.1.5", ""));
}

void test_origin_default_port_normalised() {
  TEST_ASSERT_TRUE(origin("http://192.168.1.5:80", ""));
  TEST_ASSERT_TRUE(origin("http://192.168.1.5", "", "192.168.1.5:80"));
}

void test_origin_case_insensitive() {
  TEST_ASSERT_TRUE(origin("HTTP://Remote.LAN", "", "remote.lan"));
}

void test_origin_other_site_refused() {
  TEST_ASSERT_FALSE(origin("http://evil.example", ""));
}

void test_origin_port_mismatch() {
  TEST_ASSERT_FALSE(origin("http://192.168.1.5:8080", ""));
}

void test_origin_scheme_mismatch() {
  TEST_ASSERT_FALSE(origin("https://192.168.1.5", ""));
}

void test_referer_fallback_with_path() {
  TEST_ASSERT_TRUE(origin("", "http://192.168.1.5/setup?x=1"));
}

void test_null_origin_refused() {
  TEST_ASSERT_FALSE(origin("null", "http://192.168.1.5/"));
}

void test_both_absent_refused() { TEST_ASSERT_FALSE(origin("", "")); }

void test_origin_unparseable_refused() {
  TEST_ASSERT_FALSE(origin("ftp://192.168.1.5", ""));
  TEST_ASSERT_FALSE(origin("192.168.1.5", ""));
  TEST_ASSERT_FALSE(origin("http://", ""));
  TEST_ASSERT_FALSE(origin("http://192.168.1.5:", ""));
  TEST_ASSERT_FALSE(origin("http://192.168.1.5:99999", ""));
  TEST_ASSERT_FALSE(origin("http://evil.example@192.168.1.5", ""));
  // An Origin that is present but bad does not fall back to the Referer.
  TEST_ASSERT_FALSE(origin("http://evil.example", "http://192.168.1.5/"));
}

void test_origin_long_host_refused() {
  char longHost[200];
  memset(longHost, 'a', sizeof(longHost));
  longHost[129] = '\0';
  char o[220];
  strcpy(o, "http://");
  strcat(o, longHost);
  TEST_ASSERT_FALSE(origin(o, "", longHost));
}

void test_origin_empty_host_refused() {
  TEST_ASSERT_FALSE(origin("http://192.168.1.5", "", ""));
}

// ---- Forwarded values ----

void test_forwarded_from_trusted() {
  TEST_ASSERT_TRUE(originAllowed("https://heater.example.com", "", kHost, true,
                                 "https", "heater.example.com"));
}

void test_forwarded_from_untrusted_ignored() {
  TEST_ASSERT_FALSE(originAllowed("https://heater.example.com", "", kHost,
                                  false, "https", "heater.example.com"));
}

void test_forwarded_needs_both_headers() {
  TEST_ASSERT_FALSE(originAllowed("https://heater.example.com", "", kHost, true,
                                  "https", ""));
  TEST_ASSERT_FALSE(originAllowed("https://heater.example.com", "", kHost, true,
                                  "", "heater.example.com"));
}

void test_forwarded_default_port() {
  TEST_ASSERT_TRUE(originAllowed("https://heater.example.com:443", "", kHost,
                                 true, "https", "heater.example.com"));
}

// Behind the proxy the plain-HTTP twin of the page is not the same site, so
// the direct candidate stops counting once both forwarded headers are there.
void test_proxied_refuses_plain_http_twin() {
  TEST_ASSERT_FALSE(originAllowed("http://h", "", "h", true, "https", "h"));
  TEST_ASSERT_TRUE(originAllowed("https://h", "", "h", true, "https", "h"));
  TEST_ASSERT_FALSE(originAllowed("http://h:80", "", "h", true, "https", "h"));
  TEST_ASSERT_FALSE(
      originAllowed("http://192.168.1.5", "", kHost, true, "https", "h"));
}

void test_proxied_http_forwarded_proto_accepts_http() {
  TEST_ASSERT_TRUE(originAllowed("http://h", "", "h", true, "http", "h"));
  TEST_ASSERT_FALSE(originAllowed("https://h", "", "h", true, "http", "h"));
}

void test_untrusted_peer_still_uses_host() {
  TEST_ASSERT_TRUE(originAllowed("http://h", "", "h", false, "https", "h"));
  TEST_ASSERT_FALSE(originAllowed("https://h", "", "h", false, "https", "h"));
}

void test_trusted_peer_without_forwarded_uses_host() {
  TEST_ASSERT_TRUE(originAllowed("http://h", "", "h", true, "", ""));
  TEST_ASSERT_TRUE(originAllowed("http://h", "", "h", true, "https", ""));
  TEST_ASSERT_TRUE(originAllowed("http://h", "", "h", true, "", "h"));
}

void test_proxied_referer_fallback() {
  TEST_ASSERT_FALSE(
      originAllowed("", "http://h/setup", "h", true, "https", "h"));
  TEST_ASSERT_TRUE(
      originAllowed("", "https://h/setup?x=1", "h", true, "https", "h"));
  TEST_ASSERT_TRUE(originAllowed("", "http://h/setup", "h", false, "https", "h"));
  TEST_ASSERT_TRUE(originAllowed("", "http://h/setup", "h", true, "", ""));
}

// ---- Proxies and client address ----

void test_ipv4_parse_and_format() {
  uint32_t ip = 0;
  TEST_ASSERT_TRUE(parseIpv4("192.168.1.20", ip));
  TEST_ASSERT_EQUAL_UINT32(0xC0A80114u, ip);
  char out[16];
  formatIpv4(ip, out);
  TEST_ASSERT_EQUAL_STRING("192.168.1.20", out);
  formatIpv4(0xFFFFFFFFu, out);
  TEST_ASSERT_EQUAL_STRING("255.255.255.255", out);
  TEST_ASSERT_FALSE(parseIpv4("1.2.3", ip));
  TEST_ASSERT_FALSE(parseIpv4("1.2.3.4.5", ip));
  TEST_ASSERT_FALSE(parseIpv4("1.2.3.4x", ip));
  TEST_ASSERT_FALSE(parseIpv4("1..3.4", ip));
  TEST_ASSERT_FALSE(parseIpv4("", ip));
}

void test_proxy_list_parse() {
  uint32_t list[kMaxTrustedProxies];
  size_t count = 99;
  TEST_ASSERT_TRUE(parseProxyList(" 192.168.1.10, 10.0.0.2 ", list, count));
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_EQUAL_UINT32(0xC0A8010Au, list[0]);
  TEST_ASSERT_EQUAL_UINT32(0x0A000002u, list[1]);
  count = 99;
  TEST_ASSERT_TRUE(parseProxyList("", list, count));
  TEST_ASSERT_EQUAL_UINT32(0, count);
  count = 99;
  TEST_ASSERT_TRUE(parseProxyList(nullptr, list, count));
  TEST_ASSERT_EQUAL_UINT32(0, count);
  TEST_ASSERT_TRUE(
      parseProxyList("1.1.1.1,2.2.2.2,3.3.3.3,4.4.4.4", list, count));
  TEST_ASSERT_EQUAL_UINT32(4, count);
}

void test_proxy_list_rejects() {
  uint32_t list[kMaxTrustedProxies];
  size_t count = 0;
  TEST_ASSERT_FALSE(
      parseProxyList("1.1.1.1,2.2.2.2,3.3.3.3,4.4.4.4,5.5.5.5", list, count));
  TEST_ASSERT_FALSE(parseProxyList("1.2.3", list, count));
  TEST_ASSERT_FALSE(parseProxyList("256.1.1.1", list, count));
  TEST_ASSERT_FALSE(parseProxyList("0.0.0.0", list, count));
  TEST_ASSERT_FALSE(parseProxyList("255.255.255.255", list, count));
  TEST_ASSERT_FALSE(parseProxyList("1.1.1.1,,2.2.2.2", list, count));
  TEST_ASSERT_FALSE(parseProxyList("1.1.1.1,", list, count));
}

void test_trusted_proxy_match() {
  const uint32_t list[] = {0xC0A8010Au, 0x0A000002u};
  TEST_ASSERT_TRUE(isTrustedProxy(0x0A000002u, list, 2, 0));
  TEST_ASSERT_FALSE(isTrustedProxy(0x0A000003u, list, 2, 0));
  TEST_ASSERT_TRUE(isTrustedProxy(0x0A000003u, list, 2, 0x0A000003u));
  TEST_ASSERT_FALSE(isTrustedProxy(0, list, 2, 0));
  const uint32_t zero[] = {0};
  TEST_ASSERT_FALSE(isTrustedProxy(0, zero, 1, 0));
  TEST_ASSERT_FALSE(isTrustedProxy(0x0A000003u, nullptr, 0, 0));
}

void test_client_address_forwarded() {
  TEST_ASSERT_EQUAL_UINT32(
      0xC0A80114u, clientAddress(0x0A000002u, true, "10.0.0.9, 192.168.1.20"));
}

void test_client_address_untrusted_is_peer() {
  TEST_ASSERT_EQUAL_UINT32(
      0x0A000002u, clientAddress(0x0A000002u, false, "10.0.0.9, 192.168.1.20"));
}

void test_client_address_bad_forwarded_is_peer() {
  TEST_ASSERT_EQUAL_UINT32(0x0A000002u,
                           clientAddress(0x0A000002u, true, "10.0.0.9, x"));
  TEST_ASSERT_EQUAL_UINT32(0x0A000002u, clientAddress(0x0A000002u, true, ""));
}

void test_via_https_proxy() {
  TEST_ASSERT_TRUE(viaHttpsProxy(true, "https"));
  TEST_ASSERT_TRUE(viaHttpsProxy(true, "HTTPS"));
  TEST_ASSERT_FALSE(viaHttpsProxy(false, "https"));
  TEST_ASSERT_FALSE(viaHttpsProxy(true, "http"));
  TEST_ASSERT_FALSE(viaHttpsProxy(true, ""));
}

// ---- Header login ----

void test_header_login_off_or_untrusted_refused() {
  TEST_ASSERT_FALSE(headerLoginOk(false, true, "alice", "", "", ""));
  TEST_ASSERT_FALSE(headerLoginOk(true, false, "alice", "", "", ""));
}

void test_header_login_needs_user() {
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "", "", "", ""));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "  ", "", "", ""));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, nullptr, "", "", ""));
}

void test_header_login_any_user() {
  TEST_ASSERT_TRUE(headerLoginOk(true, true, "alice", "", "", ""));
  TEST_ASSERT_TRUE(headerLoginOk(true, true, " alice ", "  ", "", ""));
}

void test_header_login_allowed_users() {
  TEST_ASSERT_TRUE(headerLoginOk(true, true, "bob", "alice, bob", "", ""));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "carol", "alice, bob", "", ""));
  // Exact comparison, and a list with no names does not mean any user.
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "Bob", "alice, bob", "", ""));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "bo", "alice, bob", "", ""));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "alice", ",", "", ""));
}

void test_header_login_groups() {
  TEST_ASSERT_TRUE(headerLoginOk(true, true, "a", "", "admins|users", "admins"));
  TEST_ASSERT_TRUE(
      headerLoginOk(true, true, "a", "", "admins|users", "ops, users"));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "a", "", "admins|users", "ops"));
  TEST_ASSERT_FALSE(headerLoginOk(true, true, "a", "", "", "admins"));
  TEST_ASSERT_FALSE(
      headerLoginOk(true, true, "a", "", "administrators", "admins"));
  // No group check when the allowed list is empty.
  TEST_ASSERT_TRUE(headerLoginOk(true, true, "a", "", "", ""));
}

void test_header_login_users_and_groups_both_apply() {
  TEST_ASSERT_FALSE(
      headerLoginOk(true, true, "carol", "alice", "admins", "admins"));
  TEST_ASSERT_FALSE(
      headerLoginOk(true, true, "alice", "alice", "users", "admins"));
  TEST_ASSERT_TRUE(
      headerLoginOk(true, true, "alice", "alice", "admins", "admins"));
}

void test_valid_header_name() {
  TEST_ASSERT_TRUE(validHeaderName("X-authentik-username"));
  TEST_ASSERT_TRUE(validHeaderName("Remote-User"));
  TEST_ASSERT_FALSE(validHeaderName(""));
  TEST_ASSERT_FALSE(validHeaderName(nullptr));
  TEST_ASSERT_FALSE(
      validHeaderName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));  // 41
  TEST_ASSERT_TRUE(
      validHeaderName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));  // 40
  TEST_ASSERT_FALSE(validHeaderName("X Bad"));
  TEST_ASSERT_FALSE(validHeaderName("X_Bad"));
  TEST_ASSERT_FALSE(validHeaderName("cookie"));
  TEST_ASSERT_FALSE(validHeaderName("Host"));
  TEST_ASSERT_FALSE(validHeaderName("ORIGIN"));
  TEST_ASSERT_FALSE(validHeaderName("Referer"));
  TEST_ASSERT_FALSE(validHeaderName("x-forwarded-for"));
  TEST_ASSERT_FALSE(validHeaderName("X-Forwarded-Host"));
  TEST_ASSERT_TRUE(validHeaderName("X-Forwarded"));
}

// ---- Login method and verdict ----

void test_auth_method_cookie_wins() {
  TEST_ASSERT_EQUAL_INT((int)AuthMethod::Password,
                        (int)authMethod(true, false));
  TEST_ASSERT_EQUAL_INT((int)AuthMethod::Password, (int)authMethod(true, true));
  TEST_ASSERT_EQUAL_INT((int)AuthMethod::Header, (int)authMethod(false, true));
  TEST_ASSERT_EQUAL_INT((int)AuthMethod::None, (int)authMethod(false, false));
}

static RequestFacts facts(RequestClass cls) {
  RequestFacts f;
  f.cls = cls;
  f.apMode = false;
  f.proxyOnly = false;
  f.viaHttpsProxy = false;
  f.originOk = true;
  f.loggedIn = false;
  return f;
}

void test_decide_public_always_allowed() {
  RequestFacts f = facts(RequestClass::Public);
  f.proxyOnly = true;
  f.originOk = false;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::Allow, (int)decideRequest(f));
}

// A request class added later without its own case must be refused, not let
// through.
void test_decide_unknown_class_fails_closed() {
  RequestFacts f = facts(static_cast<RequestClass>(99));
  f.loggedIn = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::NeedLogin, (int)decideRequest(f));
}

// A verdict that was never computed (a value-initialized one) must not allow.
void test_default_verdict_fails_closed() {
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::NeedLogin, (int)AuthVerdict{});
}

void test_decide_login_endpoint() {
  RequestFacts f = facts(RequestClass::LoginEndpoint);
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::Allow, (int)decideRequest(f));
  f.originOk = false;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::BadOrigin, (int)decideRequest(f));
}

void test_decide_protected_page() {
  RequestFacts f = facts(RequestClass::ProtectedPage);
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::NeedLogin, (int)decideRequest(f));
  f.loggedIn = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::Allow, (int)decideRequest(f));
}

void test_decide_action() {
  RequestFacts f = facts(RequestClass::Action);
  f.loggedIn = true;
  f.originOk = false;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::BadOrigin, (int)decideRequest(f));
  f.loggedIn = false;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::BadOrigin, (int)decideRequest(f));
  f.originOk = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::NeedLogin, (int)decideRequest(f));
  f.loggedIn = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::Allow, (int)decideRequest(f));
}

void test_decide_proxy_only() {
  RequestFacts f = facts(RequestClass::Action);
  f.proxyOnly = true;
  f.loggedIn = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::ProxyOnly, (int)decideRequest(f));
  f.viaHttpsProxy = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::Allow, (int)decideRequest(f));
  f.viaHttpsProxy = false;
  f.apMode = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::Allow, (int)decideRequest(f));

  RequestFacts g = facts(RequestClass::LoginEndpoint);
  g.proxyOnly = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::ProxyOnly, (int)decideRequest(g));
  RequestFacts h = facts(RequestClass::ProtectedPage);
  h.proxyOnly = true;
  h.loggedIn = true;
  TEST_ASSERT_EQUAL_INT((int)AuthVerdict::ProxyOnly, (int)decideRequest(h));
}

// The save is judged on the new manual list only. The automatic Moonraker
// entry can vanish (a switch to Manual, an outage at boot, a host edit), and
// proxy-only would then lock the owner out.
void test_proxy_only_save_off_or_ap_always_allowed() {
  const uint32_t list[] = {0x0A000001u};
  // Proxy-only stays off: nothing to check, whoever the peer is.
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::Allowed,
      (int)checkProxyOnlySave(false, false, false, 0x01020304u, list, 1));
  // The access point ignores proxy-only, so a save there must always work.
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::Allowed,
      (int)checkProxyOnlySave(true, true, false, 0x01020304u, list, 1));
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::Allowed,
      (int)checkProxyOnlySave(true, true, false, 0, nullptr, 0));
}

void test_proxy_only_save_listed_https_peer_allowed() {
  const uint32_t list[] = {0x0A000001u, 0x0A000002u};
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::Allowed,
      (int)checkProxyOnlySave(true, false, true, 0x0A000002u, list, 2));
}

void test_proxy_only_save_unlisted_peer_refused() {
  const uint32_t list[] = {0x0A000001u};
  // The peer may be trusted today through the automatic entry, but that
  // doesn't count: it is not in the manual list.
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::NeedListedProxy,
      (int)checkProxyOnlySave(true, false, true, 0x0A000009u, list, 1));
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::NeedListedProxy,
      (int)checkProxyOnlySave(true, false, true, 0x0A000009u, nullptr, 0));
  // An unknown peer (0) is never listed.
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::NeedListedProxy,
      (int)checkProxyOnlySave(true, false, true, 0, list, 1));
}

void test_proxy_only_save_listed_peer_without_https_refused() {
  const uint32_t list[] = {0x0A000001u};
  TEST_ASSERT_EQUAL_INT(
      (int)ProxyOnlySave::NeedHttps,
      (int)checkProxyOnlySave(true, false, false, 0x0A000001u, list, 1));
}

// ---- Redirect path and cookie ----

void test_safe_next_path() {
  TEST_ASSERT_TRUE(safeNextPath("/setup"));
  TEST_ASSERT_TRUE(safeNextPath("/"));
  TEST_ASSERT_FALSE(safeNextPath(""));
  TEST_ASSERT_FALSE(safeNextPath(nullptr));
  TEST_ASSERT_FALSE(safeNextPath("//evil"));
  TEST_ASSERT_FALSE(safeNextPath("/\\evil"));
  TEST_ASSERT_FALSE(safeNextPath("http://x"));
  TEST_ASSERT_FALSE(safeNextPath("/a\r\nSet-Cookie: x=1"));
  TEST_ASSERT_FALSE(safeNextPath("/a\tb"));
  char p[80];
  memset(p, 'a', sizeof(p));
  p[0] = '/';
  p[64] = '\0';
  TEST_ASSERT_TRUE(safeNextPath(p));  // 64 characters
  p[64] = 'a';
  p[65] = '\0';
  TEST_ASSERT_FALSE(safeNextPath(p));  // 65 characters
}

static void expectToken(const uint8_t *t) {
  for (size_t i = 0; i < kSessionTokenLen; i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(i * 0x11), t[i]);
  }
}

void test_cookie_single() {
  char c[80];
  strcpy(c, "iheater_session=");
  strcat(c, kHex);
  uint8_t t[kSessionTokenLen] = {0};
  TEST_ASSERT_TRUE(sessionTokenFromCookie(c, t));
  expectToken(t);
}

void test_cookie_among_others() {
  char c[120];
  strcpy(c, "a=1; iheater_session=");
  strcat(c, kHex);
  strcat(c, "; b=2");
  uint8_t t[kSessionTokenLen] = {0};
  TEST_ASSERT_TRUE(sessionTokenFromCookie(c, t));
  expectToken(t);
  strcpy(c, "a=1;iheater_session=");
  strcat(c, kHex);
  memset(t, 0, sizeof(t));
  TEST_ASSERT_TRUE(sessionTokenFromCookie(c, t));
  expectToken(t);
}

void test_cookie_lookalike_name_refused() {
  char c[80];
  uint8_t t[kSessionTokenLen];
  strcpy(c, "xiheater_session=");
  strcat(c, kHex);
  TEST_ASSERT_FALSE(sessionTokenFromCookie(c, t));
  strcpy(c, "a=1; iheater_session2=");
  strcat(c, kHex);
  TEST_ASSERT_FALSE(sessionTokenFromCookie(c, t));
  strcpy(c, "a=iheater_session=");
  strcat(c, kHex);
  TEST_ASSERT_FALSE(sessionTokenFromCookie(c, t));
}

void test_cookie_bad_value_refused() {
  uint8_t t[kSessionTokenLen];
  TEST_ASSERT_FALSE(sessionTokenFromCookie(
      "iheater_session=00112233445566778899aabbccddeef", t));  // 31
  TEST_ASSERT_FALSE(sessionTokenFromCookie(
      "iheater_session=00112233445566778899aabbccddeeff0", t));  // 33
  TEST_ASSERT_FALSE(sessionTokenFromCookie(
      "iheater_session=00112233445566778899aabbccddeefg", t));
  TEST_ASSERT_FALSE(sessionTokenFromCookie("a=1; b=2", t));
  TEST_ASSERT_FALSE(sessionTokenFromCookie("", t));
  TEST_ASSERT_FALSE(sessionTokenFromCookie(nullptr, t));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_classify_public_routes);
  RUN_TEST(test_classify_login_endpoints);
  RUN_TEST(test_classify_password_post_once_set_is_action);
  RUN_TEST(test_classify_protected_pages);
  RUN_TEST(test_classify_actions);
  RUN_TEST(test_origin_matches_host);
  RUN_TEST(test_origin_default_port_normalised);
  RUN_TEST(test_origin_case_insensitive);
  RUN_TEST(test_origin_other_site_refused);
  RUN_TEST(test_origin_port_mismatch);
  RUN_TEST(test_origin_scheme_mismatch);
  RUN_TEST(test_referer_fallback_with_path);
  RUN_TEST(test_null_origin_refused);
  RUN_TEST(test_both_absent_refused);
  RUN_TEST(test_origin_unparseable_refused);
  RUN_TEST(test_origin_long_host_refused);
  RUN_TEST(test_origin_empty_host_refused);
  RUN_TEST(test_forwarded_from_trusted);
  RUN_TEST(test_forwarded_from_untrusted_ignored);
  RUN_TEST(test_forwarded_needs_both_headers);
  RUN_TEST(test_forwarded_default_port);
  RUN_TEST(test_proxied_refuses_plain_http_twin);
  RUN_TEST(test_proxied_http_forwarded_proto_accepts_http);
  RUN_TEST(test_untrusted_peer_still_uses_host);
  RUN_TEST(test_trusted_peer_without_forwarded_uses_host);
  RUN_TEST(test_proxied_referer_fallback);
  RUN_TEST(test_ipv4_parse_and_format);
  RUN_TEST(test_proxy_list_parse);
  RUN_TEST(test_proxy_list_rejects);
  RUN_TEST(test_trusted_proxy_match);
  RUN_TEST(test_client_address_forwarded);
  RUN_TEST(test_client_address_untrusted_is_peer);
  RUN_TEST(test_client_address_bad_forwarded_is_peer);
  RUN_TEST(test_via_https_proxy);
  RUN_TEST(test_header_login_off_or_untrusted_refused);
  RUN_TEST(test_header_login_needs_user);
  RUN_TEST(test_header_login_any_user);
  RUN_TEST(test_header_login_allowed_users);
  RUN_TEST(test_header_login_groups);
  RUN_TEST(test_header_login_users_and_groups_both_apply);
  RUN_TEST(test_valid_header_name);
  RUN_TEST(test_auth_method_cookie_wins);
  RUN_TEST(test_decide_public_always_allowed);
  RUN_TEST(test_decide_login_endpoint);
  RUN_TEST(test_decide_protected_page);
  RUN_TEST(test_decide_action);
  RUN_TEST(test_decide_proxy_only);
  RUN_TEST(test_decide_unknown_class_fails_closed);
  RUN_TEST(test_default_verdict_fails_closed);
  RUN_TEST(test_proxy_only_save_off_or_ap_always_allowed);
  RUN_TEST(test_proxy_only_save_listed_https_peer_allowed);
  RUN_TEST(test_proxy_only_save_unlisted_peer_refused);
  RUN_TEST(test_proxy_only_save_listed_peer_without_https_refused);
  RUN_TEST(test_safe_next_path);
  RUN_TEST(test_cookie_single);
  RUN_TEST(test_cookie_among_others);
  RUN_TEST(test_cookie_lookalike_name_refused);
  RUN_TEST(test_cookie_bad_value_refused);
  return UNITY_END();
}
