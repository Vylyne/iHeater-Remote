#include "standalone/RemoteSettings.h"

#include <Preferences.h>
#include <RemotePolicy.h>
#include <esp_system.h>

namespace iheaterlink::standalone {

namespace {

const char *kNs = "iheater-remote";
const char *kSourceKey = "source";
const char *kHostKey = "moon-host";
const char *kPortKey = "moon-port";
const char *kTlsKey = "moon-tls";
const char *kKeyKey = "moon-key";
const char *kTokenKey = "pair-token";

const char *const kLegacyNamespaces[] = {"li_common", "li_moon", "li_bambu",
                                         "li_ha"};

bool hasSourceKey() {
  Preferences prefs;
  if (!prefs.begin(kNs, true))
    return false;
  const bool present = prefs.isKey(kSourceKey);
  prefs.end();
  return present;
}

void migrateLegacy() {
  String host, apiKey;
  uint16_t port = 7125;
  bool tls = false;
  uint8_t active = 0;

  Preferences moon;
  if (moon.begin("li_moon", true)) {
    host = moon.getString("host", "");
    port = moon.getUShort("port", 7125);
    tls = moon.getBool("ssl", false);
    apiKey = moon.getString("apiKey", "");
    moon.end();
  }
  Preferences common;
  if (common.begin("li_common", true)) {
    active = common.getUChar("active", 0);
    common.end();
  }

  host.trim();
  const bool klipper = host.length() > 0 && active != 1 && active != 2;
  if (port == 0)
    port = policy::defaultMoonrakerPort(tls);

  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    Serial.println("[REMOTE] migration skipped, settings namespace unavailable");
    return;
  }
  prefs.putString(kHostKey, host);
  prefs.putUShort(kPortKey, port);
  prefs.putBool(kTlsKey, tls);
  prefs.putString(kKeyKey, apiKey);
  prefs.putUChar(kSourceKey, klipper ? 1 : 0);
  prefs.remove("auto-target");
  prefs.remove("auto-bed");
  prefs.end();

  Serial.printf("[REMOTE] migrated settings source=%s host=%s port=%u tls=%d key=%s\n",
                klipper ? "klipper" : "manual", host.c_str(),
                static_cast<unsigned>(port), tls ? 1 : 0,
                apiKey.length() ? "set" : "none");
}

void eraseLegacy() {
  for (const char *ns : kLegacyNamespaces) {
    Preferences probe;
    if (!probe.begin(ns, true))
      continue;
    probe.end();
    Preferences prefs;
    if (prefs.begin(ns, false)) {
      prefs.clear();
      prefs.end();
      Serial.printf("[REMOTE] erased legacy NVS %s\n", ns);
    }
  }
}

}  // namespace

String generatePairToken() {
  const uint64_t bits =
      (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
  char out[policy::kPairTokenLen + 1];
  policy::pairTokenFromBits(bits, out);
  return String(out);
}

void loadRemoteSettings(RemoteSettings &out) {
  if (!hasSourceKey())
    migrateLegacy();
  eraseLegacy();

  Preferences prefs;
  if (prefs.begin(kNs, true)) {
    out.source = prefs.getUChar(kSourceKey, 0) == 1 ? ControlSource::Klipper
                                                     : ControlSource::Manual;
    out.moon.host = prefs.getString(kHostKey, "");
    out.moon.tls = prefs.getBool(kTlsKey, false);
    out.moon.port = prefs.getUShort(kPortKey, 0);
    out.moon.apiKey = prefs.getString(kKeyKey, "");
    out.pairToken = prefs.getString(kTokenKey, "");
    prefs.end();
  }
  if (out.moon.port == 0)
    out.moon.port = policy::defaultMoonrakerPort(out.moon.tls);

  char norm[policy::kPairTokenLen + 1];
  if (!policy::normalizePairToken(out.pairToken.c_str(), norm)) {
    out.pairToken = generatePairToken();
    savePairToken(out.pairToken.c_str());
    char shown[policy::kPairTokenLen + 2];
    policy::formatPairToken(out.pairToken.c_str(), shown);
    Serial.printf("[REMOTE] generated pairing token %s\n", shown);
  } else {
    out.pairToken = norm;
  }
}

bool saveSourceAndMoonraker(const RemoteSettings &s) {
  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  bool ok = prefs.putUChar(kSourceKey, static_cast<uint8_t>(s.source)) == 1;
  ok = (prefs.putString(kHostKey, s.moon.host) == s.moon.host.length()) && ok;
  ok = (prefs.putUShort(kPortKey, s.moon.port) == 2) && ok;
  ok = (prefs.putBool(kTlsKey, s.moon.tls) == 1) && ok;
  ok = (prefs.putString(kKeyKey, s.moon.apiKey) == s.moon.apiKey.length()) && ok;
  prefs.end();
  return ok;
}

bool savePairToken(const char *normalized) {
  char norm[policy::kPairTokenLen + 1];
  if (!normalized || !policy::normalizePairToken(normalized, norm))
    return false;
  Preferences prefs;
  if (!prefs.begin(kNs, false))
    return false;
  const bool ok = prefs.putString(kTokenKey, norm) == policy::kPairTokenLen;
  prefs.end();
  return ok;
}

}  // namespace iheaterlink::standalone
