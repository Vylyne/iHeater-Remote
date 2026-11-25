#include "AuthPolicy.h"

#include <cstring>

#include "AuthCrypto.h"

namespace iheaterlink::policy {

namespace {

constexpr size_t kMaxHostLen = 128;
constexpr size_t kMaxNextLen = 64;
constexpr size_t kMaxHeaderNameLen = 40;

const char *orEmpty(const char *s) { return s ? s : ""; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// Case-insensitive equality of a (ptr, len) span against a C string.
bool equalsNoCase(const char *a, size_t aLen, const char *b) {
  if (strlen(b) != aLen) return false;
  for (size_t i = 0; i < aLen; i++) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool startsWithNoCase(const char *a, const char *prefix) {
  for (; *prefix; a++, prefix++) {
    if (lower(*a) != lower(*prefix)) return false;
  }
  return true;
}

// Trim spaces and tabs from both ends of a span.
void trim(const char *&p, size_t &len) {
  while (len > 0 && isSpace(*p)) {
    p++;
    len--;
  }
  while (len > 0 && isSpace(p[len - 1])) len--;
}

bool spanEquals(const char *a, size_t aLen, const char *b, size_t bLen) {
  return aLen == bLen && memcmp(a, b, aLen) == 0;
}

// Walk a list split on one separator; calls fn(ptr, len) for each trimmed
// entry. Stops and returns true as soon as fn returns true.
template <typename Fn>
bool anyEntry(const char *list, char sep, Fn fn) {
  const char *p = orEmpty(list);
  for (;;) {
    const char *end = p;
    while (*end && *end != sep) end++;
    const char *e = p;
    size_t len = (size_t)(end - p);
    trim(e, len);
    if (fn(e, len)) return true;
    if (!*end) return false;
    p = end + 1;
  }
}

bool blank(const char *s) {
  const char *p = orEmpty(s);
  size_t len = strlen(p);
  trim(p, len);
  return len == 0;
}

// ---- Origins ----

enum class Scheme : uint8_t { Http, Https };

struct Origin {
  Scheme scheme;
  char host[kMaxHostLen + 1];  // lower case
  uint16_t port;
};

bool parseScheme(const char *s, size_t len, Scheme &out) {
  if (equalsNoCase(s, len, "http")) {
    out = Scheme::Http;
    return true;
  }
  if (equalsNoCase(s, len, "https")) {
    out = Scheme::Https;
    return true;
  }
  return false;
}

bool hostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

// Parses "host" or "host:port" up to the end of the span. No userinfo, no
// IPv6 literals. A missing port takes the scheme's default.
bool parseAuthority(const char *s, size_t len, Scheme scheme, Origin &out) {
  out.scheme = scheme;
  size_t hostLen = 0;
  while (hostLen < len && s[hostLen] != ':') hostLen++;
  if (hostLen == 0 || hostLen > kMaxHostLen) return false;
  for (size_t i = 0; i < hostLen; i++) {
    if (!hostChar(s[i])) return false;
    out.host[i] = lower(s[i]);
  }
  out.host[hostLen] = '\0';

  if (hostLen == len) {
    out.port = scheme == Scheme::Https ? 443 : 80;
    return true;
  }
  size_t digits = len - hostLen - 1;
  if (digits == 0 || digits > 5) return false;
  uint32_t port = 0;
  for (size_t i = hostLen + 1; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    port = port * 10 + (uint32_t)(s[i] - '0');
  }
  if (port == 0 || port > 65535) return false;
  out.port = (uint16_t)port;
  return true;
}

// "scheme://authority" followed by nothing (an Origin) or by a path, query or
// fragment (a Referer).
bool parseUrlOrigin(const char *s, bool allowTail, Origin &out) {
  const char *sep = strstr(s, "://");
  if (!sep) return false;
  Scheme scheme;
  if (!parseScheme(s, (size_t)(sep - s), scheme)) return false;
  const char *auth = sep + 3;
  const char *end = auth;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;
  if (*end && !allowTail) return false;
  return parseAuthority(auth, (size_t)(end - auth), scheme, out);
}

bool sameOrigin(const Origin &a, const Origin &b) {
  return a.scheme == b.scheme && a.port == b.port &&
         strcmp(a.host, b.host) == 0;
}

// ---- Cookie ----

bool startsWith(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

}  // namespace

RequestClass classifyRequest(const char *path, bool isPost, bool passwordSet) {
  path = orEmpty(path);
  if (isPost) {
    if (strcmp(path, "/api/auth/login") == 0) return RequestClass::LoginEndpoint;
    if (strcmp(path, "/api/auth/password") == 0 && !passwordSet) {
      return RequestClass::LoginEndpoint;
    }
    return RequestClass::Action;
  }
  if (strcmp(path, "/api/auth/challenge") == 0) {
    return RequestClass::LoginEndpoint;
  }
  if (strcmp(path, "/setup") == 0 || strcmp(path, "/fw") == 0 ||
      strcmp(path, "/diag") == 0) {
    return RequestClass::ProtectedPage;
  }
  return RequestClass::Public;
}

bool originAllowed(const char *origin, const char *referer, const char *host,
                   bool peerTrusted, const char *fwdProto,
                   const char *fwdHost) {
  origin = orEmpty(origin);
  referer = orEmpty(referer);
  host = orEmpty(host);
  fwdProto = orEmpty(fwdProto);
  fwdHost = orEmpty(fwdHost);

  // Origin is used as given (no fall-back when it is present but bad); the
  // Referer only stands in when there is no Origin.
  Origin got;
  if (*origin) {
    if (!parseUrlOrigin(origin, false, got)) return false;
  } else if (*referer) {
    if (!parseUrlOrigin(referer, true, got)) return false;
  } else {
    return false;
  }

  // Through a trusted proxy that sent both forwarded headers, the browser's
  // real origin is <proto>://<host> and nothing else. Host is then only the
  // bare server name, and accepting http://<Host> would let the plain-HTTP twin
  // of the page count as same-site.
  Origin want;
  if (peerTrusted && *fwdProto && *fwdHost) {
    Scheme scheme;
    return parseScheme(fwdProto, strlen(fwdProto), scheme) &&
           parseAuthority(fwdHost, strlen(fwdHost), scheme, want) &&
           sameOrigin(got, want);
  }
  return parseAuthority(host, strlen(host), Scheme::Http, want) &&
         sameOrigin(got, want);
}

bool parseIpv4(const char *text, uint32_t &out) {
  text = orEmpty(text);
  uint32_t ip = 0;
  for (int part = 0; part < 4; part++) {
    size_t digits = 0;
    uint32_t v = 0;
    while (text[digits] >= '0' && text[digits] <= '9') {
      if (digits >= 3) return false;
      v = v * 10 + (uint32_t)(text[digits] - '0');
      digits++;
    }
    if (digits == 0 || v > 255) return false;
    ip = (ip << 8) | v;
    text += digits;
    if (part < 3) {
      if (*text != '.') return false;
      text++;
    }
  }
  if (*text) return false;
  out = ip;
  return true;
}

void formatIpv4(uint32_t ip, char out[16]) {
  char *p = out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    uint32_t v = (ip >> shift) & 0xFF;
    if (v >= 100) *p++ = (char)('0' + v / 100);
    if (v >= 10) *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    if (shift) *p++ = '.';
  }
  *p = '\0';
}

bool parseProxyList(const char *csv, uint32_t out[kMaxTrustedProxies],
                    size_t &count) {
  count = 0;
  if (blank(csv)) return true;
  size_t n = 0;
  bool ok = true;
  anyEntry(csv, ',', [&](const char *e, size_t len) {
    char buf[16];
    uint32_t ip;
    if (len == 0 || len >= sizeof(buf) || n >= kMaxTrustedProxies) {
      ok = false;
      return true;
    }
    memcpy(buf, e, len);
    buf[len] = '\0';
    if (!parseIpv4(buf, ip) || ip == 0 || ip == 0xFFFFFFFFu) {
      ok = false;
      return true;
    }
    out[n++] = ip;
    return false;
  });
  if (!ok) return false;
  count = n;
  return true;
}

bool isTrustedProxy(uint32_t peer, const uint32_t *list, size_t count,
                    uint32_t automatic) {
  if (peer == 0) return false;
  if (automatic != 0 && peer == automatic) return true;
  if (!list) return false;
  for (size_t i = 0; i < count; i++) {
    if (list[i] == peer) return true;
  }
  return false;
}

uint32_t clientAddress(uint32_t peer, bool peerTrusted,
                       const char *xForwardedFor) {
  if (!peerTrusted) return peer;
  const char *s = orEmpty(xForwardedFor);
  const char *last = s;
  for (const char *p = s; *p; p++) {
    if (*p == ',') last = p + 1;
  }
  size_t len = strlen(last);
  trim(last, len);
  char buf[16];
  if (len == 0 || len >= sizeof(buf)) return peer;
  memcpy(buf, last, len);
  buf[len] = '\0';
  uint32_t ip;
  return parseIpv4(buf, ip) ? ip : peer;
}

bool viaHttpsProxy(bool peerTrusted, const char *fwdProto) {
  return peerTrusted && equalsNoCase(orEmpty(fwdProto),
                                     strlen(orEmpty(fwdProto)), "https");
}

bool headerLoginOk(bool enabled, bool peerTrusted, const char *user,
                   const char *allowedUsers, const char *groups,
                   const char *allowedGroups) {
  if (!enabled || !peerTrusted) return false;

  const char *u = orEmpty(user);
  size_t uLen = strlen(u);
  trim(u, uLen);
  if (uLen == 0) return false;

  if (!blank(allowedUsers)) {
    bool match = anyEntry(allowedUsers, ',', [&](const char *e, size_t len) {
      return len > 0 && spanEquals(e, len, u, uLen);
    });
    if (!match) return false;
  }

  if (!blank(allowedGroups)) {
    bool shared = anyEntry(groups, '|', [&](const char *g, size_t gLen) {
      if (gLen == 0) return false;
      return anyEntry(allowedGroups, ',', [&](const char *a, size_t aLen) {
        return spanEquals(g, gLen, a, aLen);
      });
    });
    if (!shared) return false;
  }
  return true;
}

bool validHeaderName(const char *name) {
  name = orEmpty(name);
  size_t len = strlen(name);
  if (len == 0 || len > kMaxHeaderNameLen) return false;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  if (equalsNoCase(name, len, "Host") || equalsNoCase(name, len, "Cookie") ||
      equalsNoCase(name, len, "Origin") || equalsNoCase(name, len, "Referer")) {
    return false;
  }
  if (startsWithNoCase(name, "X-Forwarded-")) return false;
  return true;
}

AuthMethod authMethod(bool cookieValid, bool headerOk) {
  if (cookieValid) return AuthMethod::Password;
  if (headerOk) return AuthMethod::Header;
  return AuthMethod::None;
}

AuthVerdict decideRequest(const RequestFacts &f) {
  if (f.cls == RequestClass::Public) return AuthVerdict::Allow;
  if (f.proxyOnly && !f.apMode && !f.viaHttpsProxy) return AuthVerdict::ProxyOnly;
  switch (f.cls) {
    case RequestClass::LoginEndpoint:
      return f.originOk ? AuthVerdict::Allow : AuthVerdict::BadOrigin;
    case RequestClass::ProtectedPage:
      return f.loggedIn ? AuthVerdict::Allow : AuthVerdict::NeedLogin;
    case RequestClass::Action:
      if (!f.originOk) return AuthVerdict::BadOrigin;
      return f.loggedIn ? AuthVerdict::Allow : AuthVerdict::NeedLogin;
    case RequestClass::Public:
      break;
  }
  // Not reached today. A class added later without its own case is refused.
  return AuthVerdict::NeedLogin;
}

ProxyOnlySave checkProxyOnlySave(bool proxyOnly, bool apMode,
                                 bool viaHttpsProxy, uint32_t peer,
                                 const uint32_t *list, size_t count) {
  if (!proxyOnly || apMode) return ProxyOnlySave::Allowed;
  if (!isTrustedProxy(peer, list, count, 0)) return ProxyOnlySave::NeedListedProxy;
  if (!viaHttpsProxy) return ProxyOnlySave::NeedHttps;
  return ProxyOnlySave::Allowed;
}

bool safeNextPath(const char *next) {
  if (!next || next[0] != '/') return false;
  if (next[1] == '/' || next[1] == '\\') return false;
  size_t len = strlen(next);
  if (len > kMaxNextLen) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)next[i];
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool sessionTokenFromCookie(const char *cookieHeader,
                            uint8_t token[kSessionTokenLen]) {
  static const char kName[] = "iheater_session=";
  const char *p = orEmpty(cookieHeader);
  for (;;) {
    while (isSpace(*p)) p++;
    const char *end = p;
    while (*end && *end != ';') end++;
    if (startsWith(p, kName) && p + (sizeof(kName) - 1) <= end) {
      const char *value = p + (sizeof(kName) - 1);
      // The first cookie of this name decides; a bad value does not fall
      // through to a later one.
      if ((size_t)(end - value) != kSessionTokenLen * 2) return false;
      char hex[kSessionTokenLen * 2 + 1];
      memcpy(hex, value, kSessionTokenLen * 2);
      hex[kSessionTokenLen * 2] = '\0';
      return fromHex(hex, token, kSessionTokenLen);
    }
    if (!*end) return false;
    p = end + 1;
  }
}

}  // namespace iheaterlink::policy
