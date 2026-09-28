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
#endif
