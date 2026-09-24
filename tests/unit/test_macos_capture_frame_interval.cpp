/**
 * @file tests/unit/test_macos_capture_frame_interval.cpp
 * @brief macOS capture cadence follows the encoder's negotiated rate.
 */
#include <gtest/gtest.h>

#include "src/platform/macos/capture_frame_interval.h"

TEST(MacOSCaptureFrameInterval, MatchesEncoderRateSelection) {
  const auto integer = platf::macos::capture_frame_interval(60, 0);
  EXPECT_EQ(integer.value, 1);
  EXPECT_EQ(integer.timescale, 60);

  const auto invalid_refresh = platf::macos::capture_frame_interval(60, -1);
  EXPECT_EQ(invalid_refresh.value, 1);
  EXPECT_EQ(invalid_refresh.timescale, 60);

  const auto ntsc = platf::macos::capture_frame_interval(60, 5994);
  EXPECT_EQ(ntsc.value, 1001);
  EXPECT_EQ(ntsc.timescale, 60000);

  const auto fractional = platf::macos::capture_frame_interval(60, 9498);
  EXPECT_EQ(fractional.value, 50);
  EXPECT_EQ(fractional.timescale, 4749);
}
