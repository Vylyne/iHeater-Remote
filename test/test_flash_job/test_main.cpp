#include <string.h>
#include <unity.h>

#include <initializer_list>

#include "RomProtocol.h"

using namespace iheaterlink::policy;

void setUp() {}
void tearDown() {}

void test_command_frame() {
  uint8_t out[2];
  buildRomCommand(kRomWrite, out);
  TEST_ASSERT_EQUAL_HEX8(0x31, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xCE, out[1]);
  buildRomCommand(kRomGet, out);
  TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[1]);
}

void test_address_frame() {
  uint8_t out[5];
  buildRomAddress(0x08000100, out);
  const uint8_t want[5] = {0x08, 0x00, 0x01, 0x00, 0x09};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

void test_extended_erase_body() {
  uint8_t out[64];
  TEST_ASSERT_EQUAL(9, buildEraseBody(3, true, out, sizeof out));
  const uint8_t want[9] = {0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x01};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 9);
}

void test_legacy_erase_body() {
  uint8_t out[64];
  TEST_ASSERT_EQUAL(5, buildEraseBody(3, false, out, sizeof out));
  const uint8_t want[5] = {0x02, 0x00, 0x01, 0x02, 0x01};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

void test_erase_refuses_page_30() {
  uint8_t out[128];
  TEST_ASSERT_EQUAL(63, buildEraseBody(30, true, out, sizeof out));
  TEST_ASSERT_EQUAL(32, buildEraseBody(30, false, out, sizeof out));
  for (bool ext : {true, false}) {
    TEST_ASSERT_EQUAL(0, buildEraseBody(31, ext, out, sizeof out));
    TEST_ASSERT_EQUAL(0, buildEraseBody(0, ext, out, sizeof out));
  }
  TEST_ASSERT_EQUAL(0, buildEraseBody(3, true, out, 8));
  TEST_ASSERT_EQUAL(0, buildEraseBody(3, false, out, 4));
}

void test_write_body() {
  uint8_t out[300];
  const uint8_t data[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL(6, buildWriteBody(data, 4, out, sizeof out));
  const uint8_t want[6] = {0x03, 0x01, 0x02, 0x03, 0x04, 0x07};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 6);

  uint8_t big[260] = {0};
  TEST_ASSERT_EQUAL(0, buildWriteBody(big, 5, out, sizeof out));
  TEST_ASSERT_EQUAL(0, buildWriteBody(big, 0, out, sizeof out));
  TEST_ASSERT_EQUAL(0, buildWriteBody(big, 260, out, sizeof out));
  TEST_ASSERT_EQUAL(258, buildWriteBody(big, 256, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
  TEST_ASSERT_EQUAL(0, buildWriteBody(data, 4, out, 5));
}

void test_read_count() {
  uint8_t out[2];
  buildReadCount(256, out);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
  buildReadCount(4, out);
  TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFC, out[1]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_command_frame);
  RUN_TEST(test_address_frame);
  RUN_TEST(test_extended_erase_body);
  RUN_TEST(test_legacy_erase_body);
  RUN_TEST(test_erase_refuses_page_30);
  RUN_TEST(test_write_body);
  RUN_TEST(test_read_count);
  return UNITY_END();
}
