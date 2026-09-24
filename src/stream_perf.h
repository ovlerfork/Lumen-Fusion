/**
 * @file src/stream_perf.h
 * @brief Fixed-memory samples for optional streaming diagnostics.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace stream {
  // Exact up to capacity; otherwise a reservoir spanning the whole window.
  // Average and maximum always include every observation.
  struct perf_samples_t {
    static constexpr std::size_t capacity = 512;

    void add(double value) {
      sum += value;
      maximum = std::max(maximum, value);
      ++total_count;
      if (sample_count < samples.size()) {
        samples[sample_count++] = value;
      } else {
        const auto index = std::uniform_int_distribution<std::uint64_t>(0, total_count - 1)(random);
        if (index < samples.size()) {
          samples[index] = value;
        }
      }
    }

    double average() const {
      return total_count ? sum / static_cast<double>(total_count) : 0.0;
    }

    double percentile(double fraction) const {
      if (!sample_count) {
        return 0.0;
      }
      auto sorted = samples;
      const auto index = static_cast<std::size_t>((sample_count - 1) * std::clamp(fraction, 0.0, 1.0));
      std::nth_element(sorted.begin(), sorted.begin() + index, sorted.begin() + sample_count);
      return sorted[index];
    }

    std::array<double, capacity> samples {};
    std::minstd_rand random {1};
    std::size_t sample_count {0};
    std::uint64_t total_count {0};
    double sum {0.0};
    double maximum {0.0};
  };
}  // namespace stream
