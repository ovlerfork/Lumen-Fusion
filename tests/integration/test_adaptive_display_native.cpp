/**
 * @file tests/integration/test_adaptive_display_native.cpp
 * @brief Native adaptive-display idle assertions and read-only topology inspection.
 */
#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "src/adaptive_display.h"
#include "src/utility.h"

namespace adaptive_display {
  backend macos_backend();
}

namespace {
  void expect_owned_assertion(CFStringRef expected_type) {
    CFDictionaryRef assertions = nullptr;
    auto release_assertions = util::fail_guard([&] {
      if (assertions) {
        CFRelease(assertions);
      }
    });
    ASSERT_EQ(IOPMCopyAssertionsByProcess(&assertions), kIOReturnSuccess);
    ASSERT_NE(assertions, nullptr);

    const int pid = getpid();
    auto process_key = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    ASSERT_NE(process_key, nullptr);
    auto release_process_key = util::fail_guard([&] {
      CFRelease(process_key);
    });
    const auto process_assertions = CFDictionaryGetValue(assertions, process_key);
    if (!process_assertions) {
      ASSERT_EQ(expected_type, nullptr) << "No assertions found for the test process";
      return;
    }
    ASSERT_EQ(CFGetTypeID(process_assertions), CFArrayGetTypeID());
    auto entries = static_cast<CFArrayRef>(process_assertions);
    int owned = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(entries); ++i) {
      auto entry = CFArrayGetValueAtIndex(entries, i);
      ASSERT_EQ(CFGetTypeID(entry), CFDictionaryGetTypeID());
      auto properties = static_cast<CFDictionaryRef>(entry);
      auto name = CFDictionaryGetValue(properties, kIOPMAssertionNameKey);
      if (!name || !CFEqual(name, CFSTR("Lumina paused virtual desktop"))) {
        continue;
      }
      ++owned;
      ASSERT_NE(expected_type, nullptr) << "Retained assertion remains after release";
      auto type = CFDictionaryGetValue(properties, kIOPMAssertionTypeKey);
      ASSERT_NE(type, nullptr);
      EXPECT_TRUE(CFEqual(type, expected_type)) << "Unexpected retained assertion type";
      auto level = CFDictionaryGetValue(properties, kIOPMAssertionLevelKey);
      ASSERT_NE(level, nullptr);
      ASSERT_EQ(CFGetTypeID(level), CFNumberGetTypeID());
      int value = 0;
      ASSERT_TRUE(CFNumberGetValue(static_cast<CFNumberRef>(level), kCFNumberIntType, &value));
      EXPECT_EQ(value, kIOPMAssertionLevelOn);
    }
    EXPECT_EQ(owned, expected_type ? 1 : 0);
  }

  TEST(AdaptiveDisplayNative, ReplacesAndReleasesOwnedIdleAssertion) {
    auto native = adaptive_display::macos_backend();
    auto release_power = util::fail_guard([&] {
      native.retention_power("none");
    });
    ASSERT_NO_FATAL_FAILURE(expect_owned_assertion(nullptr));

    native.retention_power("system");
    ASSERT_NO_FATAL_FAILURE(expect_owned_assertion(kIOPMAssertionTypePreventUserIdleSystemSleep));

    native.retention_power("display");
    ASSERT_NO_FATAL_FAILURE(expect_owned_assertion(kIOPMAssertionTypePreventUserIdleDisplaySleep));

    native.retention_power("none");
    ASSERT_NO_FATAL_FAILURE(expect_owned_assertion(nullptr));
  }

  TEST(AdaptiveDisplayNative, InspectsCurrentTopologyWithoutCreatingDisplay) {
    const auto current = adaptive_display::macos_backend().inspect(0);
    using adaptive_display::presence;
    EXPECT_TRUE(current.local == presence::unknown || current.local == presence::absent || current.local == presence::present);
    if (current.local == presence::present) {
      EXPECT_NE(current.local_main, 0u);
    } else {
      EXPECT_EQ(current.local_main, 0u);
    }
  }
}  // namespace

#endif
