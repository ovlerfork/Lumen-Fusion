/**
 * @file tests/unit/test_stereo_pcm.cpp
 * @brief Stereo capture frame boundaries and negotiated Opus channel layouts.
 */
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <opus/opus_multistream.h>

#include "src/audio.h"
#include "src/stereo_pcm.h"

namespace {
  TEST(StereoPcm, StereoUnchanged) {
    const std::array input {0.1f, -0.2f, 0.3f, -0.4f};
    std::array<float, 4> output {};
    ASSERT_TRUE(audio::copy_stereo_to_channels(input, output, 2));
    EXPECT_EQ(input, output);
  }

  TEST(StereoPcm, SurroundKeepsEachTemporalFrameAndPadsUnusedSpeakers) {
    const std::array input {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
    for (std::size_t channels : {6U, 8U}) {
      std::vector<float> output(3 * channels, 99.0f);
      ASSERT_TRUE(audio::copy_stereo_to_channels(input, output, channels));
      for (std::size_t frame = 0; frame < 3; ++frame) {
        EXPECT_EQ(output[frame * channels], input[frame * 2]);
        EXPECT_EQ(output[frame * channels + 1], input[frame * 2 + 1]);
        for (std::size_t channel = 2; channel < channels; ++channel) {
          EXPECT_EQ(output[frame * channels + channel], 0.0f);
        }
      }
    }
  }

  TEST(StereoPcm, FiveMillisecondBlocksConsumeFiveMillisecondsOfCapture) {
    // Previously 240*6 output floats consumed 720 stereo frames (15 ms),
    // but were advertised to the decoder as only 240 frames (5 ms).
    constexpr std::size_t frames = 240;
    std::vector<float> stereo(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      stereo[frame * 2] = static_cast<float>(frame);
      stereo[frame * 2 + 1] = -static_cast<float>(frame);
    }
    for (std::size_t channels : {2U, 6U, 8U}) {
      std::vector<float> output(frames * channels);
      ASSERT_TRUE(audio::copy_stereo_to_channels(stereo, output, channels));
      EXPECT_EQ(output[(frames - 1) * channels], stereo[(frames - 1) * 2]);
      EXPECT_EQ(output[(frames - 1) * channels + 1], stereo[(frames - 1) * 2 + 1]);
    }
  }

  TEST(StereoPcm, InvalidShapesDoNotModifyOutput) {
    const std::array input {0.1f, -0.2f, 0.3f, -0.4f};
    std::array<float, 12> output;
    output.fill(42.0f);
    const auto before = output;
    for (std::size_t channels : {0U, 1U, 3U, 4U, 9U}) {
      EXPECT_FALSE(audio::copy_stereo_to_channels(input, output, channels));
      EXPECT_EQ(output, before);
    }
    EXPECT_FALSE(audio::copy_stereo_to_channels(std::span(input).first(3), output, 6));
    EXPECT_FALSE(audio::copy_stereo_to_channels(input, std::span(output).first(11), 6));
    EXPECT_FALSE(audio::copy_stereo_to_channels(input, output, 2));
    EXPECT_EQ(output, before);
  }

  TEST(StereoPcm, EmptyMatchingBlocksAreValid) {
    EXPECT_TRUE(audio::copy_stereo_to_channels({}, {}, 2));
    EXPECT_TRUE(audio::copy_stereo_to_channels({}, {}, 6));
    EXPECT_TRUE(audio::copy_stereo_to_channels({}, {}, 8));
  }

  class StereoPcmOpus: public testing::TestWithParam<int> {};

  TEST_P(StereoPcmOpus, NegotiatedLayoutsRoundTripAtOriginalSampleRate) {
    const auto &config = audio::stream_configs[GetParam()];
    int error = OPUS_OK;
    std::unique_ptr<OpusMSEncoder, decltype(&opus_multistream_encoder_destroy)> encoder {
      opus_multistream_encoder_create(config.sampleRate, config.channelCount, config.streams,
                                     config.coupledStreams, config.mapping,
                                     OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error),
      opus_multistream_encoder_destroy
    };
    ASSERT_EQ(error, OPUS_OK);
    ASSERT_NE(encoder, nullptr);
    ASSERT_EQ(opus_multistream_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(config.bitrate)), OPUS_OK);
    ASSERT_EQ(opus_multistream_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)), OPUS_OK);

    std::unique_ptr<OpusMSDecoder, decltype(&opus_multistream_decoder_destroy)> decoder {
      opus_multistream_decoder_create(config.sampleRate, config.channelCount, config.streams,
                                     config.coupledStreams, config.mapping, &error),
      opus_multistream_decoder_destroy
    };
    ASSERT_EQ(error, OPUS_OK);
    ASSERT_NE(decoder, nullptr);

    const int frames = config.sampleRate * 5 / 1000;
    std::vector<float> stereo(frames * 2);
    std::vector<float> output(frames * config.channelCount);
    std::vector<float> decoded(output.size());
    std::array<unsigned char, 1400> packet {};
    double front_energy = 0.0;
    double unused_energy = 0.0;
    int total_decoded_frames = 0;
    // Continuous input across block boundaries, including enough frames to cover
    // codec lookahead. No screen/audio device or capture permission is involved.
    for (int block = 0; block < 20; ++block) {
      for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(block * frames + frame) / config.sampleRate;
        stereo[frame * 2] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
        stereo[frame * 2 + 1] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 660.0 * t));
      }
      ASSERT_TRUE(audio::copy_stereo_to_channels(stereo, output, config.channelCount));
      const int bytes = opus_multistream_encode_float(encoder.get(), output.data(), frames,
                                                      packet.data(), static_cast<opus_int32>(packet.size()));
      ASSERT_GT(bytes, 0);
      ASSERT_LE(bytes, static_cast<int>(packet.size()));
      const int decoded_frames = opus_multistream_decode_float(decoder.get(), packet.data(), bytes,
                                                               decoded.data(), frames, 0);
      ASSERT_EQ(decoded_frames, frames);
      total_decoded_frames += decoded_frames;
      for (int frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < config.channelCount; ++channel) {
          const float value = decoded[frame * config.channelCount + channel];
          ASSERT_TRUE(std::isfinite(value));
          (channel < 2 ? front_energy : unused_energy) += static_cast<double>(value) * value;
        }
      }
    }
    EXPECT_EQ(total_decoded_frames, config.sampleRate / 10);
    EXPECT_GT(front_energy, 1.0);
    EXPECT_LT(unused_energy, front_energy * 0.001);
  }

  INSTANTIATE_TEST_SUITE_P(
    AllNegotiatedLayouts,
    StereoPcmOpus,
    testing::Range(0, static_cast<int>(audio::MAX_STREAM_CONFIG))
  );
}  // namespace
