#pragma once

#include <Arduino.h>
#include "standalone/MoonrakerLink.h"

namespace iheaterlink::standalone {

enum class ControlSource : uint8_t { Manual = 0, Klipper = 1 };
enum class OutputKind : uint8_t { Pulse = 0, Uart = 1 };

struct RemoteSettings {
  ControlSource source = ControlSource::Manual;
  MoonrakerSettings moon;
  String pairToken;  // normalized, 8 characters
};

// Loads the Remote's own settings. On first boot after the idryer-core era it
// migrates the old li_common / li_moon values, erases the legacy namespaces
// (every boot, so a power cut mid-migration is harmless) and makes sure a
// valid pairing token exists.
void loadRemoteSettings(RemoteSettings &out);
bool saveSourceAndMoonraker(const RemoteSettings &s);
// The iHeater output choice. NVS "iheater-remote" key "output"; defaults to
// Pulse. Opens Preferences on its own, so it works before Wi-Fi and before
// loadRemoteSettings.
OutputKind loadOutputKind();
bool saveOutputKind(OutputKind kind);
bool savePairToken(const char *normalized);
String generatePairToken();  // normalized; built from esp_random() bits

}  // namespace iheaterlink::standalone
