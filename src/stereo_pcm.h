/**
 * @file src/stereo_pcm.h
 * @brief Preserve stereo PCM frame boundaries in a negotiated speaker layout.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

namespace audio {
  /**
   * @brief Copy interleaved stereo into FL/FR, leaving other speakers silent.
   * @details Input and output must be disjoint and describe the same number of
   * temporal frames. This is channel padding, not native surround capture.
   */
  inline bool copy_stereo_to_channels(std::span<const float> stereo, std::span<float> output, std::size_t channels) {
    if ((channels != 2 && channels != 6 && channels != 8) ||
        stereo.size() % 2 != 0 || output.size() % channels != 0 ||
        stereo.size() / 2 != output.size() / channels) {
      return false;
    }

    if (channels == 2) {
      std::copy(stereo.begin(), stereo.end(), output.begin());
      return true;
    }

    std::fill(output.begin(), output.end(), 0.0f);
    for (std::size_t frame = 0; frame < stereo.size() / 2; ++frame) {
      output[frame * channels] = stereo[frame * 2];
      output[frame * channels + 1] = stereo[frame * 2 + 1];
    }
    return true;
  }
}  // namespace audio
