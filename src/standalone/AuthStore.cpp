#include "standalone/AuthStore.h"

#include <Preferences.h>

namespace iheaterlink::standalone {

namespace {

const char *kNs = "iheater-auth";
const char *kSaltKey = "salt";
const char *kIterKey = "iter";
const char *kVerifierKey = "verifier";
const char *kSessionsKey = "sessions";
const char *kHoursKey = "hours";
const char *kProxiesKey = "proxies";
const char *kAutoProxyKey = "autoProxy";
const char *kHdrOnKey = "hdrOn";
const char *kHdrUserKey = "hdrUser";
const char *kHdrUsersKey = "hdrUsers";
const char *kHdrGrpHdrKey = "hdrGrpHdr";
const char *kHdrGroupsKey = "hdrGroups";
const char *kProxyOnlyKey = "proxyOnly";

const char *kDefaultUserHeader = "X-authentik-username";
const char *kDefaultGroupsHeader = "X-authentik-groups";

// A blob key that is absent or the wrong length reads as not there.
bool readBlob(Preferences &prefs, const char *key, uint8_t *out, size_t len) {
  if (prefs.getBytesLength(key) != len)
    return false;
  return prefs.getBytes(key, out, len) == len;
}

// putString returns 0 for an empty value, so confirm that case with isKey().
bool putText(Preferences &prefs, const char *key, const String &value) {
  const size_t written = prefs.putString(key, value);
  return written == value.length() && (written > 0 || prefs.isKey(key));
}

// A key that is gone is as good as one that was removed.
bool dropKey(Preferences &prefs, const char *key) {
  if (!prefs.isKey(key))
    return true;
  return prefs.remove(key);
}

}  // namespace

void loadAuth(AuthSettings &out, policy::SessionTable &sessions,
              uint32_t &hours) {
  out = AuthSettings();
  sessions.clear();
  hours = 0;

  Preferences prefs;
  if (!prefs.begin(kNs, true))
    return;  // first boot: nothing stored yet

  uint8_t salt[policy::kSaltLen];
  uint8_t verifier[policy::kVerifierLen];
  const bool saltOk = readBlob(prefs, kSaltKey, salt, sizeof(salt));
  const bool verifierOk = readBlob(prefs, kVerifierKey, verifier, sizeof(verifier));
  const bool iterPresent = prefs.isKey(kIterKey);
  const uint32_t iterations = iterPresent ? prefs.getUInt(kIterKey, 0) : 0;
  if (saltOk && verifierOk && iterPresent && policy::validIterations(iterations)) {
    memcpy(out.salt, salt, sizeof(salt));
    memcpy(out.verifier, verifier, sizeof(verifier));
    out.iterations = iterations;
    out.passwordSet = true;
  }

  uint8_t blob[policy::SessionTable::kBlobLen];
  if (readBlob(prefs, kSessionsKey, blob, sizeof(blob)))
    sessions.fromBlob(blob, sizeof(blob));  // a bad blob leaves it empty
  // Sessions without a password can't be valid.
  if (!out.passwordSet)
    sessions.clear();

  hours = prefs.getUInt(kHoursKey, 0);

  // parseProxyList writes its outputs even when it fails, so parse into a
  // temporary and keep it only on success.
  const String list = prefs.getString(kProxiesKey, "");
  uint32_t proxies[policy::kMaxTrustedProxies] = {};
  size_t proxyCount = 0;
  if (policy::parseProxyList(list.c_str(), proxies, proxyCount)) {
    memcpy(out.proxies, proxies, sizeof(proxies));
    out.proxyCount = proxyCount;
  }
  out.autoProxy = prefs.getBool(kAutoProxyKey, true);

  out.headerLogin = prefs.getBool(kHdrOnKey, false);
  out.userHeader = prefs.getString(kHdrUserKey, kDefaultUserHeader);
  if (out.userHeader.length() == 0)
    out.userHeader = kDefaultUserHeader;
  out.allowedUsers = prefs.getString(kHdrUsersKey, "");
  out.groupsHeader = prefs.getString(kHdrGrpHdrKey, kDefaultGroupsHeader);
  if (out.groupsHeader.length() == 0)
    out.groupsHeader = kDefaultGroupsHeader;
  out.allowedGroups = prefs.getString(kHdrGroupsKey, "");
  out.proxyOnly = prefs.getBool(kProxyOnlyKey, false);
  prefs.end();
}

bool savePassword(const AuthSettings &s) {
  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  bool ok = prefs.putBytes(kSaltKey, s.salt, sizeof(s.salt)) == sizeof(s.salt);
  ok = (prefs.putUInt(kIterKey, s.iterations) == sizeof(uint32_t)) && ok;
  ok = (prefs.putBytes(kVerifierKey, s.verifier, sizeof(s.verifier)) ==
        sizeof(s.verifier)) && ok;
  prefs.end();
  return ok;
}

bool saveSessions(const policy::SessionTable &t) {
  uint8_t blob[policy::SessionTable::kBlobLen];
  t.toBlob(blob);
  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  const bool ok = prefs.putBytes(kSessionsKey, blob, sizeof(blob)) == sizeof(blob);
  prefs.end();
  return ok;
}

bool saveRunningHours(uint32_t hours) {
  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  const bool ok = prefs.putUInt(kHoursKey, hours) == sizeof(uint32_t);
  prefs.end();
  return ok;
}

bool saveAuthOptions(const AuthSettings &s) {
  String list;
  for (size_t i = 0; i < s.proxyCount && i < policy::kMaxTrustedProxies; ++i) {
    char text[16];
    policy::formatIpv4(s.proxies[i], text);
    if (i)
      list += ",";
    list += text;
  }
  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  bool ok = putText(prefs, kProxiesKey, list);
  ok = (prefs.putBool(kAutoProxyKey, s.autoProxy) == 1) && ok;
  ok = (prefs.putBool(kHdrOnKey, s.headerLogin) == 1) && ok;
  ok = putText(prefs, kHdrUserKey, s.userHeader) && ok;
  ok = putText(prefs, kHdrUsersKey, s.allowedUsers) && ok;
  ok = putText(prefs, kHdrGrpHdrKey, s.groupsHeader) && ok;
  ok = putText(prefs, kHdrGroupsKey, s.allowedGroups) && ok;
  ok = (prefs.putBool(kProxyOnlyKey, s.proxyOnly) == 1) && ok;
  prefs.end();
  return ok;
}

bool resetAuth(AuthSettings &s, policy::SessionTable &t) {
  // Memory first: the reset takes effect at once even if NVS fails.
  s.passwordSet = false;
  memset(s.salt, 0, sizeof(s.salt));
  s.iterations = 0;
  memset(s.verifier, 0, sizeof(s.verifier));
  s.headerLogin = false;
  s.proxyOnly = false;
  t.clear();

  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  // The password goes first, so a partial failure leaves the Remote open
  // rather than locked.
  bool ok = dropKey(prefs, kVerifierKey);
  ok = dropKey(prefs, kSaltKey) && ok;
  ok = dropKey(prefs, kIterKey) && ok;
  ok = dropKey(prefs, kSessionsKey) && ok;
  ok = (prefs.putBool(kHdrOnKey, false) == 1) && ok;
  ok = (prefs.putBool(kProxyOnlyKey, false) == 1) && ok;
  prefs.end();
  return ok;
}

}  // namespace iheaterlink::standalone
