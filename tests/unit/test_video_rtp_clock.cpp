/**
 * @file tests/unit/test_video_rtp_clock.cpp
 * @brief RTP presentation-time selection across idle and fresh frames.
 */
#include <gtest/gtest.h>

#include "src/video_rtp_clock.h"

namespace {
  using namespace std::chrono_literals;
  using clock_t = video::rtp_clock_t;
  const clock_t::time_point epoch {10s};

  TEST(VideoRtpClock, FreshFramesUseCaptureTimeNotSendTime) {
    clock_t clock;
    EXPECT_EQ(clock.next(epoch + 10ms, epoch + 30ms, epoch), 900u);
    EXPECT_EQ(clock.next(epoch + 20ms, epoch + 40ms, epoch), 1800u);
  }

  TEST(VideoRtpClock, IdleRepeatUsesCurrentFrameStart) {
    clock_t clock;
    EXPECT_EQ(clock.next(epoch + 10ms, epoch + 15ms, epoch), 900u);
    EXPECT_EQ(clock.next(std::nullopt, epoch + 1s, epoch), 90000u);
    EXPECT_EQ(clock.next(std::nullopt, epoch + 2s, epoch), 180000u);
  }

  TEST(VideoRtpClock, FreshFrameAfterIdleCannotMoveTransportClockBackward) {
    clock_t clock;
    EXPECT_EQ(clock.next(std::nullopt, epoch + 40ms, epoch), 3600u);
    const std::optional capture {epoch + 35ms};
    EXPECT_EQ(clock.next(capture, epoch + 50ms, epoch), 3601u);
    EXPECT_EQ(capture, epoch + 35ms);
    EXPECT_EQ(clock.next(epoch + 60ms, epoch + 80ms, epoch), 5400u);
  }

  TEST(VideoRtpClock, EqualCaptureTimesStillIdentifyDifferentFrames) {
    clock_t clock;
    EXPECT_EQ(clock.next(epoch + 20ms, epoch + 30ms, epoch), 1800u);
    EXPECT_EQ(clock.next(epoch + 20ms, epoch + 40ms, epoch), 1801u);
  }

  TEST(VideoRtpClock, WireWrapDoesNotResetExtendedClock) {
    clock_t clock;
    // 2^32 ticks is just beyond 47721.8588 seconds at 90 kHz.
    const auto before = epoch + 47721858811111ns;
    const auto after = epoch + 47721858855556ns;
    EXPECT_EQ(clock.next(before, before, epoch), 0xfffffffdu);
    EXPECT_EQ(clock.next(after, after, epoch), 1u);
    EXPECT_EQ(clock.next(before, after, epoch), 2u);
  }

  TEST(VideoRtpClock, SessionsDoNotShareTimelineState) {
    clock_t first;
    clock_t second;
    EXPECT_EQ(first.next(std::nullopt, epoch + 10s, epoch), 900000u);
    EXPECT_EQ(second.next(epoch + 10ms, epoch + 20ms, epoch), 900u);
  }

  TEST(VideoRtpClock, PreEpochCaptureCannotUnderflowWireTime) {
    clock_t clock;
    EXPECT_EQ(clock.next(epoch - 1s, epoch + 1s, epoch), 0u);
    EXPECT_EQ(clock.next(epoch, epoch + 1s, epoch), 1u);
  }
}  // namespace
