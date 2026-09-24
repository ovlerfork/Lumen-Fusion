/**
 * @file src/platform/macos/capture_frame_interval.h
 * @brief Capture cadence selected from the negotiated video configuration.
 */
#pragma once

#include "src/video.h"

namespace platf::macos {
  struct frame_interval_t {
    int value;
    int timescale;
  };

  inline frame_interval_t capture_frame_interval(int framerate, int framerate_x100) {
    AVRational fps {framerate, 1};
    if (framerate_x100 > 0) {
      fps = video::framerateX100_to_rational(framerate_x100);
    }
    return {fps.den, fps.num};
  }
}  // namespace platf::macos
