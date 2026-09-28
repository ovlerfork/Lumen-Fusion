/**
 * @file src/platform/macos/encoder_benchmark_decode.h
 * @brief Native decode validation for encoded benchmark packets.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace platf::vt::benchmark {
  struct packet_view {
    std::span<const uint8_t> data;
    int64_t pts;
  };

  struct decode_result {
    bool valid = false;
    size_t decoded_frames = 0;
    // Original OSStatus for a reported native failure; zero for input/output validation errors.
    int32_t status = 0;
    std::string error;
  };

  // Packets are in decode order, each containing one complete Annex B access unit
  // with a unique PTS in timebase_num/timebase_den units. Parameter sets must be
  // included; one fixed SPS/PPS (and HEVC VPS) is supported, with identical repeats.
  // Input storage must remain alive for this call. Returns after decoder teardown.
  // Valid means exactly one decoded image per packet with matching dimensions/PTS.
  decode_result validate_decode(bool hevc, int width, int height, int timebase_num, int timebase_den, const std::vector<packet_view> &packets);
}  // namespace platf::vt::benchmark
