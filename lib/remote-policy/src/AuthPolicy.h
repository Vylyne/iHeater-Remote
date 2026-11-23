#pragma once

#include <cstddef>
#include <cstdint>

// Request decisions for the web login: request classes, the origin check,
// trusted proxies, authentik header login, the verdict and the session
// cookie parser. Pure functions with fixed buffers and no heap use, so the
// native tests cover exactly what the firmware runs.
namespace iheaterlink::policy {

enum class RequestClass : uint8_t { Public, LoginEndpoint, ProtectedPage, Action };

// path excludes the query. Anything that isn't POST is treated as a GET.
RequestClass classifyRequest(const char *path, bool isPost, bool passwordSet);

// origin/referer/fwd* are "" when absent. host is the Host header.
bool originAllowed(const char *origin, const char *referer, const char *host,
                   bool peerTrusted, const char *fwdProto, const char *fwdHost);

constexpr size_t kMaxTrustedProxies = 4;
bool parseIpv4(const char *text, uint32_t &out);  // "a.b.c.d" -> a<<24|b<<16|c<<8|d
void formatIpv4(uint32_t ip, char out[16]);
bool parseProxyList(const char *csv, uint32_t out[kMaxTrustedProxies],
                    size_t &count);
// A peer of 0 is never trusted. An automatic of 0 means no automatic entry.
bool isTrustedProxy(uint32_t peer, const uint32_t *list, size_t count,
                    uint32_t automatic);
uint32_t clientAddress(uint32_t peer, bool peerTrusted,
                       const char *xForwardedFor);
bool viaHttpsProxy(bool peerTrusted, const char *fwdProto);

// Header login. The groups header uses authentik's separator, the pipe:
// https://docs.goauthentik.io/add-secure-apps/providers/proxy/ lists
// X-authentik-groups as "Groups the user is a member of, separated by
// pipes." (example foo|bar|baz). allowedUsers and allowedGroups are comma
// lists.
bool headerLoginOk(bool enabled, bool peerTrusted, const char *user,
                   const char *allowedUsers, const char *groups,
                   const char *allowedGroups);
bool validHeaderName(const char *name);

enum class AuthMethod : uint8_t { None, Password, Header };
AuthMethod authMethod(bool cookieValid, bool headerOk);  // the cookie wins

enum class AuthVerdict : uint8_t { Allow, NeedLogin, BadOrigin, ProxyOnly };
struct RequestFacts {
  RequestClass cls;
  bool apMode;
  bool proxyOnly;
  bool viaHttpsProxy;
  bool originOk;
  bool loggedIn;
};
AuthVerdict decideRequest(const RequestFacts &f);

// Judges a security save that would leave proxy-only on. Trust comes from the
// new MANUAL proxy list alone: the automatic Moonraker entry can vanish (a
// switch to Manual, an outage at boot, a host edit, a DNS failure), and
// proxy-only would then lock the owner out. The access point ignores
// proxy-only, so a save made there is always allowed.
enum class ProxyOnlySave : uint8_t { Allowed, NeedHttps, NeedListedProxy };
ProxyOnlySave checkProxyOnlySave(bool proxyOnly, bool apMode,
                                 bool viaHttpsProxy, uint32_t peer,
                                 const uint32_t *list, size_t count);
bool safeNextPath(const char *next);

constexpr size_t kSessionTokenLen = 16;
bool sessionTokenFromCookie(const char *cookieHeader,
                            uint8_t token[kSessionTokenLen]);

}  // namespace iheaterlink::policy
