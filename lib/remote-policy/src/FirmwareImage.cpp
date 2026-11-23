#include "FirmwareImage.h"

#include <stdio.h>
#include <string.h>

#include "UartRegisters.h"

namespace iheaterlink::policy {

namespace {

constexpr size_t kVectorCount = 48;
constexpr size_t kVectorTableBytes = kVectorCount * 4;
// The magic word is readable from here; the whole 32-byte record needs 0xE0.
constexpr size_t kMagicSearchMin = kMarkerOffset + 4;
constexpr size_t kUartVectorFirst = 16;
constexpr size_t kUartVectorIndex = 43;

constexpr const char *kFallbackWarning =
    "This image has no checksum or version; it looks like a UART build from its vector table.";

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The vector value that appears most often in vectors 16..47. On a tie, the
// first value to reach the top count wins.
uint32_t commonVector(const uint8_t *data) {
  uint32_t best = 0;
  int bestCount = 0;
  for (size_t i = kUartVectorFirst; i < kVectorCount; i++) {
    uint32_t v = rd32(data + i * 4);
    int n = 0;
    for (size_t j = kUartVectorFirst; j <= i; j++) {
      if (rd32(data + j * 4) == v) n++;
    }
    if (n > bestCount) {
      bestCount = n;
      best = v;
    }
  }
  return best;
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
  if (reset < kFlashBase || reset >= kResetVectorEnd) {
    info.result = ImageCheck::BadResetVector;
    return info;
  }

  if (len >= kMagicSearchMin && rd32(data + kMarkerOffset) == kMarkerMagic) {
    // A cut-off marker image would otherwise pass the fallback checks and
    // flash a valid page 0 whose reset vector points at pages never sent.
    if (len < kMarkerOffset + kMarkerBytes) {
      info.hasMarker = true;
      info.result = ImageCheck::LengthMismatch;
      return info;
    }
    const uint8_t *m = data + kMarkerOffset;
    info.hasMarker = true;
    info.protocol = rd16(m + 0x0A);
    info.fwVersion = rd16(m + 0x0C);
    info.fwPatch = rd16(m + 0x0E);
    info.crc = rd32(m + 0x14);
    info.gitSha = rd32(m + 0x18);
    info.flags = rd16(m + 0x1C);
    if (rd16(m + 0x04) != kMarkerVersion || rd16(m + 0x06) != kMarkerBytes) {
      info.result = ImageCheck::BadRecord;
      return info;
    }
    if (rd16(m + 0x08) != kBuildTypeUart) {
      info.result = ImageCheck::NotUartBuild;
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

  // No build record: accept only a file whose vector table looks like a UART
  // build (vector 43 differs from the common handler).
  if (len < kVectorTableBytes || rd32(data + kUartVectorIndex * 4) == commonVector(data)) {
    info.result = ImageCheck::NotUartVectors;
    return info;
  }
  // The reset handler must lie inside the image; the Thumb bit is not part of
  // the address.
  if ((reset & ~1u) >= kFlashBase + (uint32_t)len) {
    info.result = ImageCheck::BadResetVector;
    return info;
  }
  info.result = ImageCheck::Ok;
  return info;
}

const char *imageCheckMessage(ImageCheck result) {
  switch (result) {
    case ImageCheck::Ok: return "OK";
    case ImageCheck::TooSmall: return "The file is too small to be iHeater firmware.";
    case ImageCheck::TooLarge: return "The file is over 30,720 bytes, too big for the iHeater.";
    case ImageCheck::BadStackPointer:
      return "The file doesn't start with an iHeater vector table (stack pointer).";
    case ImageCheck::BadResetVector:
      return "The file doesn't start with an iHeater vector table (reset vector).";
    case ImageCheck::BadRecord: return "The build record has an unknown version or size.";
    case ImageCheck::NotUartBuild: return "This is not a UART build of the iHeater firmware.";
    case ImageCheck::LengthMismatch:
      return "The file is truncated or has extra bytes: its length doesn't match its build record.";
    case ImageCheck::BadChecksum: return "The file is damaged: its checksum doesn't match.";
    case ImageCheck::UnsupportedProtocol:
      return "This firmware uses a link protocol this Remote doesn't support.";
    case ImageCheck::NotUartVectors:
      return "This file has no build record and doesn't look like a UART build.";
  }
  return "";
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
  if (info.result == ImageCheck::Ok && info.hasMarker) {
    char version[40];
    imageVersion(info, version, sizeof version);
    n = snprintf(out, cap, "UART build %s, protocol %u, checksum OK", version, (unsigned)info.protocol);
  } else if (info.result == ImageCheck::Ok) {
    n = snprintf(out, cap, "%s", kFallbackWarning);
  } else {
    n = snprintf(out, cap, "%s", imageCheckMessage(info.result));
  }
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

}  // namespace iheaterlink::policy
