/**
 * @file tests/integration/test_macos_pixel_buffer.cpp
 * @brief Native capture-buffer CPU access and VideoToolbox frame ownership.
 */
#ifdef __APPLE__

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/nv12_zero_device.h"

namespace {
  class MacosPixelBuffer: public testing::TestWithParam<OSType> {
  protected:
    static constexpr int width = 64;
    static constexpr int height = 32;
    CVPixelBufferRef source_pixel = nullptr;
    CMSampleBufferRef source_sample = nullptr;
    std::shared_ptr<platf::av_img_t> img;

    [[nodiscard]] bool is_p010_format() const {
      return GetParam() == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
             GetParam() == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    }

    void fill_buffer(CVPixelBufferRef buffer, uint16_t p010_value, uint8_t other_value) const {
      ASSERT_EQ(CVPixelBufferLockBaseAddress(buffer, 0), kCVReturnSuccess);
      auto unlock = util::fail_guard([&] {
        CVPixelBufferUnlockBaseAddress(buffer, 0);
      });
      const bool planar = CVPixelBufferIsPlanar(buffer);
      const size_t planes = planar ? CVPixelBufferGetPlaneCount(buffer) : 1;
      for (size_t plane = 0; plane < planes; ++plane) {
        auto data = planar ? CVPixelBufferGetBaseAddressOfPlane(buffer, plane) : CVPixelBufferGetBaseAddress(buffer);
        const auto stride = planar ? CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) : CVPixelBufferGetBytesPerRow(buffer);
        const auto rows = planar ? CVPixelBufferGetHeightOfPlane(buffer, plane) : CVPixelBufferGetHeight(buffer);
        ASSERT_NE(data, nullptr);
        if (is_p010_format()) {
          std::fill_n(static_cast<uint16_t *>(data), stride * rows / sizeof(uint16_t), p010_value);
        } else {
          std::memset(data, other_value, stride * rows);
        }
      }
    }

    std::shared_ptr<platf::av_img_t> create_released_image(uint16_t p010_value, uint8_t other_value) const {
      CVPixelBufferRef pixel = nullptr;
      auto surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      EXPECT_NE(surface, nullptr);
      if (!surface) {
        return {};
      }
      auto release_surface = util::fail_guard([&] {
        CFRelease(surface);
      });
      const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
      const void *values[] = {surface};
      auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      EXPECT_NE(attributes, nullptr);
      if (!attributes) {
        return {};
      }
      auto release_attributes = util::fail_guard([&] {
        CFRelease(attributes);
      });
      if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, GetParam(), attributes, &pixel) != kCVReturnSuccess || !pixel) {
        ADD_FAILURE() << "Could not create replacement pixel buffer";
        return {};
      }
      auto release_pixel = util::fail_guard([&] {
        CVPixelBufferRelease(pixel);
      });
      fill_buffer(pixel, p010_value, other_value);

      CMVideoFormatDescriptionRef description = nullptr;
      if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel, &description) != noErr || !description) {
        ADD_FAILURE() << "Could not describe replacement pixel buffer";
        return {};
      }
      auto release_description = util::fail_guard([&] {
        CFRelease(description);
      });
      CMSampleBufferRef sample = nullptr;
      CMSampleTimingInfo timing = {CMTimeMake(1, 60), kCMTimeZero, kCMTimeInvalid};
      if (CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixel, true, nullptr, nullptr, description, &timing, &sample) != noErr || !sample) {
        ADD_FAILURE() << "Could not create replacement sample buffer";
        return {};
      }
      auto release_sample = util::fail_guard([&] {
        CFRelease(sample);
      });

      auto replacement = std::make_shared<platf::av_img_t>();
      replacement->sample_buffer = std::make_shared<platf::av_sample_buf_t>(sample);
      replacement->pixel_buffer = std::make_shared<platf::av_pixel_buf_t>(replacement->sample_buffer->buf);
      replacement->data = replacement->pixel_buffer->data();
      return replacement;
    }

    void SetUp() override {
      auto surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      ASSERT_NE(surface, nullptr);
      auto release_surface = util::fail_guard([&] {
        CFRelease(surface);
      });
      const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
      const void *values[] = {surface};
      auto attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      ASSERT_NE(attributes, nullptr);
      auto release_attributes = util::fail_guard([&] {
        CFRelease(attributes);
      });
      ASSERT_EQ(CVPixelBufferCreate(kCFAllocatorDefault, width, height, GetParam(), attributes, &source_pixel), kCVReturnSuccess);
      ASSERT_NE(source_pixel, nullptr);

      fill_buffer(source_pixel, 0x8000, 0x80);

      CMVideoFormatDescriptionRef description = nullptr;
      auto release_description = util::fail_guard([&] {
        if (description) {
          CFRelease(description);
        }
      });
      ASSERT_EQ(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, source_pixel, &description), noErr);
      CMSampleTimingInfo timing = {CMTimeMake(1, 60), kCMTimeZero, kCMTimeInvalid};
      ASSERT_EQ(CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, source_pixel, true, nullptr, nullptr, description, &timing, &source_sample), noErr);
      ASSERT_NE(source_sample, nullptr);
      img = std::make_shared<platf::av_img_t>();
      img->sample_buffer = std::make_shared<platf::av_sample_buf_t>(source_sample);
      img->pixel_buffer = std::make_shared<platf::av_pixel_buf_t>(img->sample_buffer->buf);
      img->data = img->pixel_buffer->data();
    }

    void release_sources() {
      if (source_sample) {
        CFRelease(source_sample);
        source_sample = nullptr;
      }
      if (source_pixel) {
        CVPixelBufferRelease(source_pixel);
        source_pixel = nullptr;
      }
    }

    void TearDown() override {
      img.reset();
      release_sources();
    }

    void expect_contents(CVPixelBufferRef buffer, uint16_t p010_value = 0x8000, uint8_t other_value = 0x80) {
      ASSERT_NE(buffer, nullptr);
      EXPECT_EQ(CVPixelBufferGetPixelFormatType(buffer), GetParam());
      EXPECT_EQ(CVPixelBufferGetWidth(buffer), width);
      EXPECT_EQ(CVPixelBufferGetHeight(buffer), height);
      // Independent CPU read after ownership transfers, outside the capture wrapper.
      ASSERT_EQ(CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
      auto unlock = util::fail_guard([&] {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
      });
      const bool planar = CVPixelBufferIsPlanar(buffer);
      const size_t planes = planar ? CVPixelBufferGetPlaneCount(buffer) : 1;
      for (size_t plane = 0; plane < planes; ++plane) {
        auto data = planar ? CVPixelBufferGetBaseAddressOfPlane(buffer, plane) : CVPixelBufferGetBaseAddress(buffer);
        const auto stride = planar ? CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) : CVPixelBufferGetBytesPerRow(buffer);
        const auto rows = planar ? CVPixelBufferGetHeightOfPlane(buffer, plane) : CVPixelBufferGetHeight(buffer);
        ASSERT_NE(data, nullptr);
        if (is_p010_format()) {
          auto begin = static_cast<const uint16_t *>(data);
          EXPECT_TRUE(std::all_of(begin, begin + stride * rows / sizeof(uint16_t), [p010_value](uint16_t value) {
            return value == p010_value;
          }));
        } else {
          auto begin = static_cast<const uint8_t *>(data);
          EXPECT_TRUE(std::all_of(begin, begin + stride * rows, [other_value](uint8_t value) {
            return value == other_value;
          }));
        }
      }
    }
  };

  TEST_P(MacosPixelBuffer, WrapperPreservesContentsAfterSourceOwnersRelease) {
    const auto buffer = source_pixel;
    release_sources();
    ASSERT_EQ(img->pixel_buffer->buf, buffer);
    if (GetParam() == kCVPixelFormatType_32BGRA) {
      ASSERT_NE(img->pixel_buffer->data(), nullptr);
      EXPECT_EQ(img->data, img->pixel_buffer->data());
      const auto size = CVPixelBufferGetBytesPerRow(buffer) * height;
      EXPECT_TRUE(std::all_of(img->data, img->data + size, [](uint8_t value) {
        return value == 0x80;
      }));
    } else {
      EXPECT_EQ(img->pixel_buffer->data(), nullptr);
      EXPECT_EQ(img->data, nullptr);
    }
    expect_contents(buffer);
  }

  TEST_P(MacosPixelBuffer, ConversionRetainsBufferAcrossReplacementAndDeviceDestruction) {
    const auto buffer = source_pixel;
    release_sources();
    util::safe_ptr<AVFrame, platf::free_frame> retained {av_frame_alloc()};
    ASSERT_NE(retained, nullptr);
    {
      platf::nv12_zero_device device;
      const auto format = is_p010_format() ? platf::pix_fmt_e::p010 : platf::pix_fmt_e::nv12;
      ASSERT_EQ(device.init(nullptr, format, [](void *, int, int) {}, [](void *, int) {}), 0);
      auto frame = av_frame_alloc();
      ASSERT_NE(frame, nullptr);
      frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
      frame->width = width;
      frame->height = height;
      ASSERT_EQ(device.set_frame(frame, nullptr), 0);  // device now owns frame

      platf::av_img_t empty;
      EXPECT_NE(device.convert(empty), 0);
      ASSERT_EQ(device.convert(*img), 0);
      ASSERT_NE(frame->buf[0], nullptr);
      EXPECT_EQ(frame->format, AV_PIX_FMT_VIDEOTOOLBOX);
      EXPECT_EQ(frame->data[3], reinterpret_cast<uint8_t *>(buffer));
      EXPECT_EQ(frame->buf[0]->data, frame->data[3]);
      ASSERT_EQ(av_frame_ref(retained.get(), frame), 0);

      // Replacing the device's frame must not change the buffer retained by FFmpeg.
      auto replacement = create_released_image(0x4000, 0x40);
      ASSERT_NE(replacement, nullptr);
      const auto replacement_buffer = replacement->pixel_buffer->buf;
      ASSERT_NE(replacement_buffer, buffer);
      ASSERT_EQ(device.convert(*replacement), 0);
      ASSERT_NE(frame->buf[0], nullptr);
      EXPECT_EQ(frame->buf[0]->data, reinterpret_cast<uint8_t *>(replacement_buffer));
      EXPECT_EQ(frame->data[3], reinterpret_cast<uint8_t *>(replacement_buffer));
      EXPECT_EQ(retained->buf[0]->data, reinterpret_cast<uint8_t *>(buffer));
      EXPECT_EQ(retained->data[3], reinterpret_cast<uint8_t *>(buffer));
      replacement.reset();
      img.reset();
      expect_contents(reinterpret_cast<CVPixelBufferRef>(retained->data[3]));
      expect_contents(reinterpret_cast<CVPixelBufferRef>(frame->data[3]), 0x4000, 0x40);
      EXPECT_NE(device.convert(empty), 0);
    }
    // Only the cloned AVFrame owns the pixel buffer after device destruction.
    ASSERT_NE(retained->buf[0], nullptr);
    EXPECT_EQ(retained->data[3], reinterpret_cast<uint8_t *>(buffer));
    EXPECT_EQ(retained->buf[0]->data, retained->data[3]);
    expect_contents(reinterpret_cast<CVPixelBufferRef>(retained->data[3]));
    av_frame_unref(retained.get());
    av_frame_unref(retained.get());
  }

  INSTANTIATE_TEST_SUITE_P(
    NativeFormats,
    MacosPixelBuffer,
    testing::Values(
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
      kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
      kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
      kCVPixelFormatType_420YpCbCr10BiPlanarFullRange,
      kCVPixelFormatType_32BGRA
    )
  );
}  // namespace

#endif
