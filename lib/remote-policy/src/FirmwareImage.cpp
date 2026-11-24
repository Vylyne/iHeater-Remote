#include "FirmwareImage.h"

#include <stdio.h>
#include <string.h>

#include "UartRegisters.h"

namespace iheaterlink::policy {

namespace {

// The magic word is readable from here; the whole 32-byte record needs 0xE0.
constexpr size_t kMagicSearchMin = kMarkerOffset + 4;

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace

uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

ImageInfo checkImage(const uint8_t *data, size_t len) {
  ImageInfo info;
  info.size = (uint32_t)len;
  if (len < kMinImageBytes) {
    info.result = ImageCheck::TooSmall;
    return info;
  }
  if (len > kMaxImageBytes) {
    info.result = ImageCheck::TooLarge;
    return info;
  }
  uint32_t sp = rd32(data);
  if (sp < kRamBase || sp > kRamEnd) {
    info.result = ImageCheck::BadStackPointer;
    return info;
  }
  uint32_t reset = rd32(data + 4);
  if (reset < kAppBase || reset >= kResetVectorEnd) {
    info.result = ImageCheck::BadResetVector;
    return info;
  }

  if (len < kMagicSearchMin || rd32(data + kMarkerOffset) != kMarkerMagic) {
    info.result = ImageCheck::NoRecord;
    return info;
  }
  // A cut-off record can't be checked, and the image is short of its own length.
  if (len < kMarkerOffset + kMarkerBytes) {
    info.result = ImageCheck::LengthMismatch;
    return info;
  }
  const uint8_t *m = data + kMarkerOffset;
  info.protocol = rd16(m + 0x0A);
  info.buildType = rd16(m + 0x08);
  info.fwVersion = rd16(m + 0x0C);
  info.fwPatch = rd16(m + 0x0E);
  info.crc = rd32(m + 0x14);
  info.gitSha = rd32(m + 0x18);
  info.flags = rd16(m + 0x1C);
  if (rd16(m + 0x04) != kMarkerVersion || rd16(m + 0x06) != kMarkerBytes) {
    info.result = ImageCheck::BadRecord;
    return info;
  }
  if (info.buildType != kBuildTypeApp) {
    info.result = ImageCheck::WrongBuildType;
    return info;
  }
  if (rd32(m + 0x10) != len) {
    info.result = ImageCheck::LengthMismatch;
    return info;
  }
  // The checksum covers the whole file with the CRC field read as zero.
  const size_t crcAt = kMarkerOffset + 0x14;
  const uint8_t zeros[4] = {0, 0, 0, 0};
  uint32_t c = crc32(data, crcAt);
  c = crc32(zeros, 4, c);
  c = crc32(data + crcAt + 4, len - crcAt - 4, c);
  if (c != info.crc) {
    info.result = ImageCheck::BadChecksum;
    return info;
  }
  if (info.protocol != kSupportedProtocol) {
    info.result = ImageCheck::UnsupportedProtocol;
    return info;
  }
  info.result = ImageCheck::Ok;
  return info;
}

const char *imageCheckMessage(ImageCheck result) {
  switch (result) {
    case ImageCheck::Ok: return "OK";
    case ImageCheck::TooSmall: return "The file is too small to be iHeater firmware.";
    case ImageCheck::TooLarge: return "The file is over 28,672 bytes, too big for the iHeater.";
    case ImageCheck::BadStackPointer:
      return "The file doesn't start with an iHeater vector table (stack pointer).";
    case ImageCheck::BadResetVector:
      return "The file doesn't start with an iHeater vector table (reset vector).";
    case ImageCheck::NoRecord: return "This file has no build record, so it can't be checked.";
    case ImageCheck::BadRecord: return "The build record has an unknown version or size.";
    case ImageCheck::WrongBuildType:
      return "This is not the app build. The link bootloader takes build_type 2 (the app build).";
    case ImageCheck::LengthMismatch:
      return "The file is truncated or has extra bytes: its length doesn't match its build record.";
    case ImageCheck::BadChecksum: return "The file is damaged: its checksum doesn't match.";
    case ImageCheck::UnsupportedProtocol:
      return "This firmware uses a link protocol this Remote doesn't support.";
  }
  return "";
}

namespace {

size_t clampLen(int n, char *out, size_t cap) {
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

}  // namespace

size_t imageCheckDetail(const ImageInfo &info, char *out, size_t cap) {
  if (cap == 0) return 0;
  int n;
  if (info.result == ImageCheck::WrongBuildType) {
    const char *what = info.buildType == 1   ? " (the old ROM build)"
                       : info.buildType == 3 ? " (the bootloader)"
                                             : "";
    n = snprintf(out, cap, "This is a build_type %u image%s. The link bootloader takes build_type %u (the app build).",
                 (unsigned)info.buildType, what, (unsigned)kBuildTypeApp);
  } else {
    n = snprintf(out, cap, "%s", imageCheckMessage(info.result));
  }
  return clampLen(n, out, cap);
}

size_t imageVersion(const ImageInfo &info, char *out, size_t cap) {
  if (cap == 0) return 0;
  const bool showSha = (info.flags & kMarkerFlagNoGit) == 0;
  int n = snprintf(out, cap, "v%u.%u.%u", (unsigned)(info.fwVersion >> 8), (unsigned)(info.fwVersion & 0xFF),
               (unsigned)info.fwPatch);
  if (n > 0 && (size_t)n < cap && showSha) {
    n += snprintf(out + n, cap - n, "-%07x", (unsigned)(info.gitSha >> 4));
    if (n > 0 && (size_t)n < cap && (info.flags & kMarkerFlagDirty)) {
      n += snprintf(out + n, cap - n, "-dirty");
    }
  }
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t imageSummary(const ImageInfo &info, char *out, size_t cap) {
  if (cap == 0) return 0;
  int n;
  if (info.result == ImageCheck::Ok) {
    char version[40];
    imageVersion(info, version, sizeof version);
    n = snprintf(out, cap, "App build %s, protocol %u, checksum OK", version, (unsigned)info.protocol);
    return clampLen(n, out, cap);
  }
  return imageCheckDetail(info, out, cap);
}

}  // namespace iheaterlink::policy
