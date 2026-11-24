#include <unity.h>

#include "FoxHuntTarget.h"

void setUp() {}
void tearDown() {}

void test_mac_match_requires_equal_non_null_addresses() {
  const uint8_t target[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  const uint8_t equal[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  const uint8_t different[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x56};

  TEST_ASSERT_TRUE(pocketnode::foxHuntMacMatches(target, equal));
  TEST_ASSERT_FALSE(pocketnode::foxHuntMacMatches(target, different));
  TEST_ASSERT_FALSE(pocketnode::foxHuntMacMatches(nullptr, equal));
  TEST_ASSERT_FALSE(pocketnode::foxHuntMacMatches(target, nullptr));
}

void test_only_wifi_targets_adopt_valid_channels() {
  TEST_ASSERT_TRUE(pocketnode::foxHuntShouldUpdateChannel(false, 1));
  TEST_ASSERT_TRUE(pocketnode::foxHuntShouldUpdateChannel(false, 177));
  TEST_ASSERT_FALSE(pocketnode::foxHuntShouldUpdateChannel(false, 0));
  TEST_ASSERT_FALSE(pocketnode::foxHuntShouldUpdateChannel(true, 1));
}

void test_stale_detection_handles_timer_rollover() {
  TEST_ASSERT_FALSE(pocketnode::foxHuntTargetIsStale(1500, 0, 1500));
  TEST_ASSERT_TRUE(pocketnode::foxHuntTargetIsStale(1501, 0, 1500));
  TEST_ASSERT_FALSE(pocketnode::foxHuntTargetIsStale(5, UINT32_MAX - 4, 10));
  TEST_ASSERT_TRUE(pocketnode::foxHuntTargetIsStale(6, UINT32_MAX - 5, 10));
}

void test_next_channel_wraps_and_rejects_empty_ranges() {
  TEST_ASSERT_EQUAL_UINT8(0, pocketnode::foxHuntNextChannel(0, 0));
  TEST_ASSERT_EQUAL_UINT8(1, pocketnode::foxHuntNextChannel(0, 14));
  TEST_ASSERT_EQUAL_UINT8(7, pocketnode::foxHuntNextChannel(6, 14));
  TEST_ASSERT_EQUAL_UINT8(1, pocketnode::foxHuntNextChannel(14, 14));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_mac_match_requires_equal_non_null_addresses);
  RUN_TEST(test_only_wifi_targets_adopt_valid_channels);
  RUN_TEST(test_stale_detection_handles_timer_rollover);
  RUN_TEST(test_next_channel_wraps_and_rejects_empty_ranges);
  return UNITY_END();
}
