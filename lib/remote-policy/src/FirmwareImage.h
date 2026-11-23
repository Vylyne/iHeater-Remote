#pragma once

#include <stddef.h>
#include <stdint.h>

// Checks for an iHeater firmware image before it is flashed over the link,
// plus the CRC-32 the build record uses. Pure, no Arduino dependency.
namespace iheaterlink::policy {

constexpr uint32_t kFlashBase = 0x08000000;
constexpr uint32_t kFlashPageBytes = 1024;
constexpr uint16_t kLastFlashPage = 29;
constexpr size_t kMinImageBytes = 8, kMaxImageBytes = 30720;
constexpr uint32_t kRamBase = 0x20000000, kRamEnd = 0x20001800;  // SP, inclusive
constexpr uint32_t kResetVectorEnd = 0x08007800;                // exclusive
constexpr size_t kMarkerOffset = 0xC0, kMarkerBytes = 32;
constexpr uint32_t kMarkerMagic = 0x57464869;
constexpr uint16_t kMarkerVersion = 1, kBuildTypeUart = 1;
constexpr uint16_t kMarkerFlagDirty = 1, kMarkerFlagNoGit = 2;

enum class ImageCheck : uint8_t {
  Ok,
  TooSmall,
  TooLarge,
  BadStackPointer,
  BadResetVector,
  BadRecord,
  NotUartBuild,
  LengthMismatch,
  BadChecksum,
  UnsupportedProtocol,
  NotUartVectors
};

struct ImageInfo {
  ImageCheck result = ImageCheck::TooSmall;
  bool hasMarker = false;
  uint32_t size = 0;
  uint16_t protocol = 0, fwVersion = 0, fwPatch = 0, flags = 0;
  uint32_t gitSha = 0, crc = 0;
};

// Same as zlib.crc32; pass the previous result as `crc` to chain calls.
uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0);

ImageInfo checkImage(const uint8_t *data, size_t len);

// One sentence for the page; "OK" for Ok.
const char *imageCheckMessage(ImageCheck result);

// "v<major>.<minor>.<patch>-1a2b3c4[-dirty]", from fwVersion = major << 8 | minor.
// Writes a terminated string and returns its length.
size_t imageVersion(const ImageInfo &info, char *out, size_t cap);

// The page line: the marker summary, the fallback warning, or the message.
size_t imageSummary(const ImageInfo &info, char *out, size_t cap);

}  // namespace iheaterlink::policy
