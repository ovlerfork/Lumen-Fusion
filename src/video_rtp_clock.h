/**
 * @file src/video_rtp_clock.h
 * @brief RTP presentation timestamps without modifying capture-time evidence.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace video {
  class rtp_clock_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    std::uint32_t next(std::optional<time_point> capture, time_point frame_start, time_point epoch) {
      using ticks_t = std::chrono::duration<std::int64_t, std::ratio<1, 90000>>;
      const auto relative = std::chrono::round<ticks_t>(capture.value_or(frame_start) - epoch).count();
      auto ticks = static_cast<std::uint64_t>(std::max<std::int64_t>(0, relative));
      // A new capture may predate the transmission of a preceding idle repeat.
      // Keep transport order monotonic; the original capture timestamp is untouched.
      if (last && ticks <= *last) {
        ticks = *last + 1;
      }
      last = ticks;
      // Truncate only at the wire boundary so normal 32-bit RTP wrap is preserved.
      return static_cast<std::uint32_t>(ticks);
    }

  private:
    std::optional<std::uint64_t> last;
  };
}  // namespace video
