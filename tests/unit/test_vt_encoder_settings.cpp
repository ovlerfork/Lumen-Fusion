/**
 * @file tests/unit/test_vt_encoder_settings.cpp
 * @brief Validate VideoToolbox selection and linked FFmpeg preferences without opening hardware.
 */
#ifdef __APPLE__
#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "src/platform/macos/vt_output_completion.h"

TEST(VideoToolboxSettings, AutomaticSelectionPreservesOtherSpecificationFields) {
  const void *keys[] = {CFSTR("EnableLowLatencyRateControl"), kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, CFSTR("OtherField")};
  const void *values[] = {kCFBooleanTrue, kCFBooleanTrue, CFSTR("preserved")};
  CFDictionaryRef original = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  ASSERT_NE(original, nullptr);
  EXPECT_EQ(platf::vt::copy_encoder_specification(original, false), nullptr);
  CFDictionaryRef automatic = platf::vt::copy_encoder_specification(original, true);
  ASSERT_NE(automatic, nullptr);
  EXPECT_EQ(CFDictionaryGetCount(automatic), 2);
  EXPECT_FALSE(CFDictionaryContainsKey(automatic, keys[0]));
  EXPECT_EQ(CFDictionaryGetValue(automatic, keys[1]), values[1]);
  EXPECT_EQ(CFDictionaryGetValue(automatic, keys[2]), values[2]);
  EXPECT_EQ(CFDictionaryGetCount(original), 3);
  CFRelease(automatic);
  CFRelease(original);
  EXPECT_EQ(platf::vt::copy_encoder_specification(nullptr, true), nullptr);
}

TEST(VideoToolboxSettings, PreferencesUseLinkedEncoderCapabilities) {
  const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
  ASSERT_NE(codec, nullptr);
  AVCodecContext *context = avcodec_alloc_context3(codec);
  ASSERT_NE(context, nullptr);
  for (int preference : {-1, 0, 1}) {
    AVDictionary *options = nullptr;
    platf::vt::apply_encoder_options(context, &options, preference, preference);
    EXPECT_EQ(av_dict_get(options, "coder", nullptr, 0), nullptr);
    const auto *speed = av_dict_get(options, "prio_speed", nullptr, 0);
    if (av_opt_find(context->priv_data, "prio_speed", nullptr, 0, 0)) {
      EXPECT_NE(speed, nullptr);
      if (speed) {
        EXPECT_STREQ(speed->value, preference == 0 ? "0" : "1");
      }
    } else {
      EXPECT_EQ(speed, nullptr);
    }
    const auto *power = av_dict_get(options, "power_efficient", nullptr, 0);
    if (preference >= 0 && av_opt_find(context->priv_data, "power_efficient", nullptr, 0, 0)) {
      EXPECT_NE(power, nullptr);
      if (power) {
        EXPECT_STREQ(power->value, preference == 0 ? "0" : "1");
      }
    } else {
      EXPECT_EQ(power, nullptr);
    }
    av_dict_free(&options);
  }
  avcodec_free_context(&context);
}

TEST(VideoToolboxSettings, H264EntropyUsesLinkedEncoderNamedValues) {
  const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
  ASSERT_NE(codec, nullptr);
  AVCodecContext *context = avcodec_alloc_context3(codec);
  ASSERT_NE(context, nullptr);
  for (int coder : {-1, 0, 1, 2}) {
    AVDictionary *options = nullptr;
    EXPECT_EQ(av_dict_set(&options, "require_sw", "1", 0), 0);
    platf::vt::apply_encoder_options(context, &options, -1, -1, coder);
    const auto *entropy = av_dict_get(options, "coder", nullptr, 0);
    if (coder <= 0) {
      EXPECT_EQ(entropy, nullptr);
    } else {
      EXPECT_NE(entropy, nullptr);
      if (entropy) {
        EXPECT_STREQ(entropy->value, coder == 1 ? "cabac" : "cavlc");
        EXPECT_EQ(av_opt_set(context->priv_data, "coder", entropy->value, 0), 0);
        int64_t actual = -1;
        EXPECT_EQ(av_opt_get_int(context->priv_data, "coder", 0, &actual), 0);
        EXPECT_EQ(actual, coder == 1 ? 2 : 1);
      }
    }
    av_dict_free(&options);
  }
  avcodec_free_context(&context);
}

TEST(VideoToolboxSettings, HevcHdrKeepsSoftwareSelectionAndOmitsH264Entropy) {
  const AVCodec *codec = avcodec_find_encoder_by_name("hevc_videotoolbox");
  ASSERT_NE(codec, nullptr);
  AVCodecContext *context = avcodec_alloc_context3(codec);
  ASSERT_NE(context, nullptr);
  context->profile = AV_PROFILE_HEVC_MAIN_10;
  context->sw_pix_fmt = AV_PIX_FMT_P010;
  AVDictionary *options = nullptr;
  EXPECT_EQ(av_dict_set(&options, "require_sw", "1", 0), 0);
  platf::vt::apply_encoder_options(context, &options, 0, 1, 1);
  EXPECT_EQ(av_dict_get(options, "coder", nullptr, 0), nullptr);
  const auto *software = av_dict_get(options, "require_sw", nullptr, 0);
  EXPECT_NE(software, nullptr);
  if (software) {
    EXPECT_STREQ(software->value, "1");
  }
  EXPECT_EQ(context->profile, AV_PROFILE_HEVC_MAIN_10);
  EXPECT_EQ(context->sw_pix_fmt, AV_PIX_FMT_P010);
  av_dict_free(&options);
  avcodec_free_context(&context);
}
#endif
