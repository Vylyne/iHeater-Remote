#pragma once

#include <Arduino.h>
#include <AuthPolicy.h>
#include <AuthState.h>

#include <stddef.h>
#include <stdint.h>

namespace iheaterlink::standalone {

// The web login's settings, in their own NVS namespace "iheater-auth".
// Keys: salt, iter, verifier, sessions, hours, proxies, autoProxy, hdrOn,
// hdrUser, hdrUsers, hdrGrpHdr, hdrGroups, proxyOnly.
struct AuthSettings {
  bool passwordSet = false;
  uint8_t salt[policy::kSaltLen] = {};
  uint32_t iterations = 0;
  uint8_t verifier[policy::kVerifierLen] = {};
  uint32_t proxies[policy::kMaxTrustedProxies] = {};
  size_t proxyCount = 0;
  bool autoProxy = true;
  bool headerLogin = false;
  String userHeader = "X-authentik-username";
  String allowedUsers;
  String groupsHeader = "X-authentik-groups";
  String allowedGroups;
  bool proxyOnly = false;
};

// Missing or damaged values load as their defaults: wrong-length blobs load as
// empty, and a bad proxy list loads as no proxies.
void loadAuth(AuthSettings &out, policy::SessionTable &sessions, uint32_t &hours);
bool savePassword(const AuthSettings &s);  // salt, iter and verifier
bool saveSessions(const policy::SessionTable &t);
bool saveRunningHours(uint32_t hours);
// Proxies, autoProxy, the header fields and proxyOnly.
bool saveAuthOptions(const AuthSettings &s);
// BOOT reset: clears the password, sessions, header login and proxy-only;
// keeps the proxies and autoProxy.
bool resetAuth(AuthSettings &s, policy::SessionTable &t);

}  // namespace iheaterlink::standalone
