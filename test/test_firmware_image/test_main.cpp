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

// No magic. With uartVectors false, vector 43 equals the default.
static std::vector<uint8_t> fallbackImage(size_t size, bool uartVectors = true) {
  std::vector<uint8_t> b = makeImage(size);
  put32(b, kRec, 0);
  if (!uartVectors) put32(b, 43 * 4, 0x080065F1);
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
  TEST_ASSERT_EQUAL_HEX32(0xF684F4BD, crc);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
  TEST_ASSERT_TRUE(info.hasMarker);
  TEST_ASSERT_EQUAL_UINT16(1, info.protocol);
  TEST_ASSERT_EQUAL_UINT16(5, info.fwVersion);
  TEST_ASSERT_EQUAL_HEX32(0x1A2B3C4D, info.gitSha);
}

void test_summary_line_for_marker_image() {
  std::vector<uint8_t> b = goodImage();
  ImageInfo info = checkImage(b.data(), b.size());
  char out[160];
  imageSummary(info, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("UART build v0.5.0-1a2b3c4, protocol 1, checksum OK", out);
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

void test_size_limits() {
  TEST_ASSERT_EQUAL((int)ImageCheck::TooSmall, check(makeImage(7)));
  TEST_ASSERT_EQUAL((int)ImageCheck::TooLarge, check(makeImage(30721)));
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(fallbackImage(30720)));
}

void test_stack_pointer_range() {
  const uint32_t bad[] = {0x1FFFFFFC, 0x20001804};
  const uint32_t good[] = {0x20000000, 0x20001800};
  for (uint32_t sp : bad) {
    std::vector<uint8_t> b = fallbackImage(1024);
    put32(b, 0, sp);
    TEST_ASSERT_EQUAL((int)ImageCheck::BadStackPointer, check(b));
  }
  for (uint32_t sp : good) {
    std::vector<uint8_t> b = fallbackImage(1024);
    put32(b, 0, sp);
    TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(b));
  }
}

void test_reset_vector_range() {
  const uint32_t bad[] = {0x07FFFFFF, 0x08007800};
  for (uint32_t rv : bad) {
    std::vector<uint8_t> b = fallbackImage(1024);
    put32(b, 4, rv);
    TEST_ASSERT_EQUAL((int)ImageCheck::BadResetVector, check(b));
  }
  std::vector<uint8_t> b = fallbackImage(1024);
  put32(b, 4, 0x080077FF);
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, check(b));
}

void test_record_checks_in_order() {
  std::vector<uint8_t> b = goodImage();
  put16(b, kRec + 4, 2);
  TEST_ASSERT_EQUAL((int)ImageCheck::BadRecord, check(b));

  b = goodImage();
  put16(b, kRec + 6, 33);
  TEST_ASSERT_EQUAL((int)ImageCheck::BadRecord, check(b));

  b = goodImage();
  put16(b, kRec + 8, 2);
  TEST_ASSERT_EQUAL((int)ImageCheck::NotUartBuild, check(b));

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

void test_fallback_uart_vectors() {
  std::vector<uint8_t> b = fallbackImage(1024);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
  TEST_ASSERT_FALSE(info.hasMarker);
  char out[160];
  imageSummary(info, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING(
      "This image has no checksum or version; it looks like a UART build from its vector table.", out);
}

void test_fallback_refuses_non_uart_vectors() {
  TEST_ASSERT_EQUAL((int)ImageCheck::NotUartVectors, check(fallbackImage(1024, false)));
}

void test_magic_ignored_below_0xE0() {
  std::vector<uint8_t> b = makeImage(0xDF);
  ImageInfo info = checkImage(b.data(), b.size());
  TEST_ASSERT_EQUAL((int)ImageCheck::Ok, (int)info.result);
  TEST_ASSERT_FALSE(info.hasMarker);
}

void test_fallback_needs_whole_vector_table() {
  std::vector<uint8_t> b = makeImage(0xBF);
  put32(b, kRec, 0);
  TEST_ASSERT_EQUAL((int)ImageCheck::NotUartVectors, check(b));
}

void test_messages_and_failed_summary() {
  TEST_ASSERT_EQUAL_STRING("OK", imageCheckMessage(ImageCheck::Ok));
  TEST_ASSERT_EQUAL_STRING("The file is too small to be iHeater firmware.",
                           imageCheckMessage(ImageCheck::TooSmall));
  ImageInfo bad;
  char out[100];
  imageSummary(bad, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("The file is too small to be iHeater firmware.", out);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_matches_zlib);
  RUN_TEST(test_marker_fixture_crc_matches_zlib);
  RUN_TEST(test_summary_line_for_marker_image);
  RUN_TEST(test_version_dirty_and_no_git);
  RUN_TEST(test_size_limits);
  RUN_TEST(test_stack_pointer_range);
  RUN_TEST(test_reset_vector_range);
  RUN_TEST(test_record_checks_in_order);
  RUN_TEST(test_fallback_uart_vectors);
  RUN_TEST(test_fallback_refuses_non_uart_vectors);
  RUN_TEST(test_magic_ignored_below_0xE0);
  RUN_TEST(test_fallback_needs_whole_vector_table);
  RUN_TEST(test_messages_and_failed_summary);
  return UNITY_END();
}
