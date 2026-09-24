/**
 * @file src/platform/macos/av_img_t.h
 * @brief Declarations for AV image types on macOS.
 */
#pragma once

// platform includes
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

// local includes
#include "src/platform/common.h"

namespace platf {
  struct av_sample_buf_t {
    CMSampleBufferRef buf;

    explicit av_sample_buf_t(CMSampleBufferRef buf):
        buf((CMSampleBufferRef) CFRetain(buf)) {
    }

    ~av_sample_buf_t() {
      if (buf != nullptr) {
        CFRelease(buf);
      }
    }
  };

  struct av_pixel_buf_t {
    CVPixelBufferRef buf;

    // Constructor
    explicit av_pixel_buf_t(CMSampleBufferRef sb):
        buf(
          CMSampleBufferGetImageBuffer(sb)
        ) {
      if (buf) {
        // These capture formats go directly to VideoToolbox as CVPixelBufferRefs.
        // CPU locking is only needed for BGRA/software access (including dummy images).
        const auto format = CVPixelBufferGetPixelFormatType(buf);
        if (format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
            format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
            format != kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange &&
            format != kCVPixelFormatType_420YpCbCr10BiPlanarFullRange) {
          locked = CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess;
        }
      }
    }

    [[nodiscard]] uint8_t *data() const {
      return locked ? static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buf)) : nullptr;
    }

    // Destructor
    ~av_pixel_buf_t() {
      if (locked) {
        CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
      }
    }

  private:
    bool locked = false;
  };

  struct av_img_t: img_t {
    std::shared_ptr<av_sample_buf_t> sample_buffer;
    std::shared_ptr<av_pixel_buf_t> pixel_buffer;
  };

  struct temp_retain_av_img_t {
    std::shared_ptr<av_sample_buf_t> sample_buffer;
    std::shared_ptr<av_pixel_buf_t> pixel_buffer;
    uint8_t *data;

    temp_retain_av_img_t(
      std::shared_ptr<av_sample_buf_t> sb,
      std::shared_ptr<av_pixel_buf_t> pb,
      uint8_t *dt
    ):
        sample_buffer(std::move(sb)),
        pixel_buffer(std::move(pb)),
        data(dt) {
    }
  };
}  // namespace platf
