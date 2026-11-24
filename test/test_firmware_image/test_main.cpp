#include <string.h>
#include <unity.h>

#include <vector>

#include "FirmwareImage.h"
#include "../image_fixture.h"

using namespace iheaterlink::policy;
using namespace imagefixture;

static const size_t kRec = kMarkerOffset;

static std::vector<uint8_t> goodImage(size_t size = 1024, uint32_t sha = 0x1A2B3C4D, uint16_t flags = 0) {
  std::vector<uint8_t> b = makeImage(size);
  patchImage(b, size, sha, flags);
  return b;
}

static int check(const std::vector<uint8_t> &b) { return (int)checkImage(b.data(), b.size()).result; }

void setUp() {}
void tearDown() {}

void test_crc32_matches_zlib() {
  const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(digits, 9));
  const uint8_t zeros[32] = {0};
  TEST_ASSERT_EQUAL_HEX32(0x190A55AD, crc32(zeros, 32));
  TEST_ASSERT_EQUAL_HEX32(0, crc32(digits, 0));
  uint32_t part = crc32(digits, 4);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(digits + 4, 5, part));
}

void test_marker_fixture_crc_matches_zlib() {
  std::vector<uint8_t> b = goodImage();
  uint32_t crc = b[0xD4] | (b[0xD5] << 8) | (b[0xD6] << 16) | ((uint32_t)b[0xD7] << 24);
  TEST_ASSERT_EQUAL_HEX32(0x8A56B7AC, crc);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
  TEST_ASSERT_EQUAL_UINT16(2, info.buildType);
  TEST_ASSERT_EQUAL_UINT16(1, info.protocol);
  TEST_ASSERT_EQUAL_UINT16(5, info.fwVersion);
  TEST_ASSERT_EQUAL_HEX32(0x1A2B3C4D, info.gitSha);
}

void test_summary_line_for_marker_image() {
  std::vector<uint8_t> b = goodImage();
  ImageInfo info = checkImage(b.data(), b.size());
  char out[160];
  imageSummary(info, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("App build v0.5.0-1a2b3c4, protocol 1, checksum OK", out);
}

void test_version_dirty_and_no_git() {
  char out[48];
  const uint16_t flags[] = {1, 2, 3};
  const char *want[] = {"v0.5.0-1a2b3c4-dirty", "v0.5.0", "v0.5.0"};
  for (int i = 0; i < 3; i++) {
    std::vector<uint8_t> b = goodImage(1024, 0x1A2B3C4D, flags[i]);
    ImageInfo info = checkImage(b.data(), b.size());
    TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
    imageVersion(info, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING(want[i], out);
  }
}

void test_version_uses_major_and_minor() {
  std::vector<uint8_t> b = makeImage(1024);
  put16(b, kRec + 0x0C, 0x0105);
  put16(b, kRec + 0x0E, 3);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
  char out[48];
  imageVersion(info, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("v1.5.3-1a2b3c4", out);
}

void test_max_image_bytes() {
  TEST_ASSERT_EQUAL((int)ImageCheck::TooSmall, check(makeImage(7)));
  TEST_ASSERT_EQUAL((int)ImageCheck::TooLarge, check(goodImage(28673)));
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(goodImage(28672)));
}

void test_accepts_app_build() {
  std::vector<uint8_t> b = makeImage(1024);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
  TEST_ASSERT_EQUAL_UINT16(2, info.buildType);
}

void test_rejects_rom_build_naming_type() {
  std::vector<uint8_t> b = makeImage(1024);
  put16(b, kRec + 8, 1);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::WrongBuildType, (int)info.result);
  TEST_ASSERT_EQUAL_UINT16(1, info.buildType);
  char out[160];
  imageCheckDetail(info, out, sizeof out);
  TEST_ASSERT_NOT_NULL(strstr(out, "build_type 1"));
  TEST_ASSERT_EQUAL_STRING(
      "This is a build_type 1 image (the old ROM build). The link bootloader takes build_type 2 (the app build).",
      out);
  char small[8];
  size_t n = imageCheckDetail(info, small, sizeof small);
  TEST_ASSERT_EQUAL_UINT(7, n);
  TEST_ASSERT_EQUAL_UINT(0, small[7]);
}

void test_rom_build_with_rom_vectors_names_type() {
  std::vector<uint8_t> b = makeImage(1024);
  put32(b, 4, 0x08000101);
  put16(b, kRec + 8, 1);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::WrongBuildType, (int)info.result);
  char out[160];
  imageCheckDetail(info, out, sizeof out);
  TEST_ASSERT_NOT_NULL(strstr(out, "build_type 1"));
}

void test_rejects_bootloader_build() {
  std::vector<uint8_t> b = makeImage(1024);
  put16(b, kRec + 8, 3);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::WrongBuildType, (int)info.result);
  char out[160];
  imageCheckDetail(info, out, sizeof out);
  TEST_ASSERT_NOT_NULL(strstr(out, "build_type 3"));
}

void test_no_record_refused() {
  std::vector<uint8_t> b = makeImage(1024);
  put32(b, kRec, 0);
  TEST_ASSERT_EQUAL((int)ImageCheck::NoRecord, check(b));
  // Too short to hold the magic word: still no record, not a vector fallback.
  b = makeImage(0xC3);
  TEST_ASSERT_EQUAL((int)ImageCheck::NoRecord, check(b));
}

void test_reset_vector_below_app_base() {
  std::vector<uint8_t> b = goodImage();
  put32(b, 4, 0x08000101);
  TEST_ASSERT_EQUAL((int)ImageCheck::BadResetVector, check(b));
}

void test_stack_pointer_range() {
  const uint32_t bad[] = {0x1FFFFFFC, 0x20001804};
  const uint32_t good[] = {0x20000000, 0x20001800};
  for (uint32_t sp : bad) {
    std::vector<uint8_t> b = goodImage();
    put32(b, 0, sp);
    TEST_ASSERT_EQUAL((int)ImageCheck::BadStackPointer, check(b));
  }
  for (uint32_t sp : good) {
    std::vector<uint8_t> b = makeImage(1024);
    put32(b, 0, sp);
    patchImage(b, 1024, 0x1A2B3C4D, 0);
    TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(b));
  }
}

void test_reset_vector_range() {
  const uint32_t bad[] = {0x080007FF, 0x08007800};
  for (uint32_t rv : bad) {
    std::vector<uint8_t> b = goodImage();
    put32(b, 4, rv);
    TEST_ASSERT_EQUAL((int)ImageCheck::BadResetVector, check(b));
  }
  const uint32_t good[] = {0x08000800, 0x080077FF};
  for (uint32_t rv : good) {
    std::vector<uint8_t> b = makeImage(1024);
    put32(b, 4, rv);
    patchImage(b, 1024, 0x1A2B3C4D, 0);
    TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(b));
  }
}

void test_record_checks_in_order() {
  std::vector<uint8_t> b = goodImage();
  put16(b, kRec + 4, 2);
  TEST_ASSERT_EQUAL((int)ImageCheck::BadRecord, check(b));

  b = goodImage();
  put16(b, kRec + 6, 33);
  TEST_ASSERT_EQUAL((int)ImageCheck::BadRecord, check(b));

  b = goodImage();
  put16(b, kRec + 8, 1);
  TEST_ASSERT_EQUAL((int)ImageCheck::WrongBuildType, check(b));

  b = goodImage();
  put32(b, kRec + 0x10, 1023);
  TEST_ASSERT_EQUAL((int)ImageCheck::LengthMismatch, check(b));

  b = goodImage();
  b[0x300] ^= 0xFF;
  TEST_ASSERT_EQUAL((int)ImageCheck::BadChecksum, check(b));

  b = makeImage(1024);
  put16(b, kRec + 0x0A, 2);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  TEST_ASSERT_EQUAL((int)ImageCheck::UnsupportedProtocol, check(b));
}

void test_marker_image_truncated_is_refused() {
  std::vector<uint8_t> full = goodImage();
  const size_t cuts[] = {200, 0xC4, 0xDF};
  for (size_t cut : cuts) {
    std::vector<uint8_t> b(full.begin(), full.begin() + cut);
    TEST_ASSERT_EQUAL((int)ImageCheck::LengthMismatch, check(b));
  }
  std::vector<uint8_t> b(full.begin(), full.begin() + 0xC3);
  TEST_ASSERT_EQUAL((int)ImageCheck::NoRecord, check(b));
  b.assign(full.begin(), full.begin() + 1024);
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(b));
}

void test_messages_and_failed_summary() {
  TEST_ASSERT_EQUAL_STRING("OK", imageCheckMessage(ImageCheck::Ok));
  TEST_ASSERT_EQUAL_STRING("The file is too small to be iHeater firmware.",
                           imageCheckMessage(ImageCheck::TooSmall));
  ImageInfo bad;
  char out[100];
  imageSummary(bad, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("The file is too small to be iHeater firmware.", out);
  // A refusal summary carries the detail, naming the build type.
  std::vector<uint8_t> b = makeImage(1024);
  put16(b, kRec + 8, 1);
  patchImage(b, 1024, 0x1A2B3C4D, 0);
  ImageInfo rom = checkImage(b.data(), b.size());
  char line[160];
  imageSummary(rom, line, sizeof line);
  TEST_ASSERT_NOT_NULL(strstr(line, "build_type 1"));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_matches_zlib);
  RUN_TEST(test_marker_fixture_crc_matches_zlib);
  RUN_TEST(test_summary_line_for_marker_image);
  RUN_TEST(test_version_dirty_and_no_git);
  RUN_TEST(test_version_uses_major_and_minor);
  RUN_TEST(test_max_image_bytes);
  RUN_TEST(test_accepts_app_build);
  RUN_TEST(test_rejects_rom_build_naming_type);
  RUN_TEST(test_rom_build_with_rom_vectors_names_type);
  RUN_TEST(test_rejects_bootloader_build);
  RUN_TEST(test_no_record_refused);
  RUN_TEST(test_reset_vector_below_app_base);
  RUN_TEST(test_stack_pointer_range);
  RUN_TEST(test_reset_vector_range);
  RUN_TEST(test_record_checks_in_order);
  RUN_TEST(test_marker_image_truncated_is_refused);
  RUN_TEST(test_messages_and_failed_summary);
  return UNITY_END();
}
