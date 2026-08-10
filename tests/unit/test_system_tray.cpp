/**
 * @file tests/unit/test_system_tray.cpp
 * @brief Tests for system tray state and native lifecycle.
 */
#include <gtest/gtest.h>

#if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
  #include <tray.h>

  #include "src/system_tray.h"

namespace {
  class SystemTrayTest: public testing::Test {
  protected:
    void SetUp() override {
      system_tray::end_tray();
      tray_loop(0);
      system_tray::reset_tray_data_for_testing();
    }

    void TearDown() override {
      system_tray::end_tray();
      tray_loop(0);
      system_tray::reset_tray_data_for_testing();
    }
  };

  TEST_F(SystemTrayTest, LuminaMenuIdentityIsPreserved) {
    const auto &tray = system_tray::tray_data_for_testing();

    ASSERT_NE(tray.menu, nullptr);
    ASSERT_NE(tray.menu[0].text, nullptr);
    EXPECT_STREQ(tray.tooltip, PROJECT_NAME);
    EXPECT_STREQ(tray.menu[0].text, "Open Lumina");
  }

  #if defined(__APPLE__) || defined(__MACH__)
  TEST_F(SystemTrayTest, MacOSUsesQtNativeContextMenuOnly) {
    const auto &tray = system_tray::tray_data_for_testing();

    ASSERT_NE(tray.cb, nullptr);
    tray.cb(const_cast<struct tray *>(&tray));
    EXPECT_FALSE(system_tray::tray_initialized_for_testing());
  }
  #endif

  TEST_F(SystemTrayTest, StateUpdatesAreIgnoredBeforeInitialization) {
    const auto &tray = system_tray::tray_data_for_testing();
    const auto *const initial_icon = tray.icon;

    system_tray::update_tray_playing("Test Game");
    system_tray::update_tray_pausing("Test Game");
    system_tray::update_tray_stopped("Test Game");

    EXPECT_FALSE(system_tray::tray_initialized_for_testing());
    EXPECT_EQ(tray.icon, initial_icon);
  }

}  // namespace
#endif
