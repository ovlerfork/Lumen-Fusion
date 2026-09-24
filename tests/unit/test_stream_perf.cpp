/**
 * @file tests/unit/test_stream_perf.cpp
 * @brief Bounded streaming telemetry includes the full observation window.
 */
#include <gtest/gtest.h>

#include "src/stream_perf.h"

TEST(StreamPerfSamples, EmptyWindow) {
  stream::perf_samples_t samples;
  EXPECT_EQ(samples.average(), 0.0);
  EXPECT_EQ(samples.percentile(0.99), 0.0);
  EXPECT_EQ(samples.total_count, 0u);
}

TEST(StreamPerfSamples, SmallWindowKeepsEveryObservation) {
  stream::perf_samples_t samples;
  for (int index = 0; index < 300; ++index) {
    samples.add(index);
  }
  EXPECT_EQ(samples.sample_count, 300u);
  EXPECT_EQ(samples.total_count, 300u);
  EXPECT_DOUBLE_EQ(samples.average(), 149.5);
  EXPECT_EQ(samples.maximum, 299.0);
  EXPECT_EQ(samples.percentile(0.50), 149.0);
  EXPECT_EQ(samples.percentile(0.95), 284.0);
  EXPECT_EQ(samples.percentile(0.99), 296.0);
}

TEST(StreamPerfSamples, HighRateWindowDoesNotKeepOnlyItsBeginning) {
  stream::perf_samples_t samples;
  for (int index = 0; index < 1200; ++index) {
    samples.add(index < 512 ? 1.0 : 100.0);
  }
  EXPECT_EQ(samples.sample_count, stream::perf_samples_t::capacity);
  EXPECT_EQ(samples.total_count, 1200u);
  EXPECT_DOUBLE_EQ(samples.average(), (512.0 + 68800.0) / 1200.0);
  EXPECT_EQ(samples.maximum, 100.0);
  EXPECT_TRUE(std::any_of(samples.samples.begin(), samples.samples.end(), [](double value) {
    return value == 100.0;
  }));
  EXPECT_EQ(samples.percentile(0.99), 100.0);
}

TEST(StreamPerfSamples, PercentileCalculationDoesNotAlterReservoir) {
  stream::perf_samples_t samples;
  for (int index = 0; index < 1800; ++index) {
    samples.add(index % 77);
  }
  const auto original = samples.samples;
  EXPECT_LE(samples.percentile(0.50), samples.percentile(0.99));
  EXPECT_EQ(samples.samples, original);
  EXPECT_EQ(samples.sample_count, stream::perf_samples_t::capacity);
  EXPECT_EQ(samples.total_count, 1800u);
}

TEST(StreamPerfSamples, ResetStartsNewWindow) {
  stream::perf_samples_t samples;
  samples.add(1000.0);
  samples = stream::perf_samples_t {};
  samples.add(2.0);
  EXPECT_EQ(samples.sample_count, 1u);
  EXPECT_EQ(samples.total_count, 1u);
  EXPECT_EQ(samples.average(), 2.0);
  EXPECT_EQ(samples.maximum, 2.0);
  EXPECT_EQ(samples.percentile(0.99), 2.0);
}
