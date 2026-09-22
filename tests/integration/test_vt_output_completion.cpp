/**
 * @file tests/integration/test_vt_output_completion.cpp
 * @brief Native VideoToolbox output delivery using the host's linked FFmpeg.
 */
#ifdef __APPLE__

#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <thread>
#include <tuple>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "src/platform/macos/vt_output_completion.h"

namespace {
  using clock_type = std::chrono::steady_clock;

  class VideoToolboxOutputCompletion: public testing::TestWithParam<std::tuple<const char *, bool>> {
  protected:
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;

    void TearDown() override {
      // Freeing the context also closes the native session on assertion failures.
      avcodec_free_context(&context);
      av_frame_free(&frame);
      av_packet_free(&packet);
    }

    void initialize() {
      const auto [codec_name, completion] = GetParam();
      std::cout << "VT measurement codec=" << codec_name << " completion=" << completion
                << " avcodec_version=" << avcodec_version()
                << " av_version_info=" << av_version_info()
                << " configuration=" << avcodec_configuration() << std::endl;
      const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
      ASSERT_NE(codec, nullptr);
      context = avcodec_alloc_context3(codec);
      ASSERT_NE(context, nullptr);
      context->width = 1600;
      context->height = 1112;
      context->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
      context->sw_pix_fmt = AV_PIX_FMT_NV12;
      context->time_base = AVRational {1, 60};
      context->framerate = AVRational {60, 1};
      context->max_b_frames = 0;
      context->gop_size = std::numeric_limits<int>::max();
      context->keyint_min = std::numeric_limits<int>::max();
      context->flags = AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;
      context->flags2 = AV_CODEC_FLAG2_FAST;
      context->bit_rate = 20000000;
      context->rc_min_rate = context->bit_rate;
      context->rc_max_rate = context->bit_rate;
      context->rc_buffer_size = context->bit_rate / 60;
      context->color_range = AVCOL_RANGE_MPEG;
      context->color_primaries = AVCOL_PRI_BT709;
      context->color_trc = AVCOL_TRC_BT709;
      context->colorspace = AVCOL_SPC_BT709;
      ASSERT_EQ(av_opt_set_int(context->priv_data, "allow_sw", 0, 0), 0);
      ASSERT_EQ(av_opt_set_int(context->priv_data, "require_sw", 0, 0), 0);
      ASSERT_EQ(av_opt_set_int(context->priv_data, "realtime", 1, 0), 0);
      ASSERT_EQ(av_opt_set_int(context->priv_data, "prio_speed", 1, 0), 0);
      if (context->codec_id == AV_CODEC_ID_HEVC) {
        ASSERT_EQ(av_opt_set_int(context->priv_data, "max_ref_frames", 1, 0), 0);
      }
      // These tests deliberately fail if the targeted runner has no hardware encoder.
      ASSERT_EQ(avcodec_open2(context, codec, nullptr), 0);
      frame = av_frame_alloc();
      packet = av_packet_alloc();
      ASSERT_NE(frame, nullptr);
      ASSERT_NE(packet, nullptr);
    }

    void prepare_frame(int64_t pts, bool idr) {
      av_frame_unref(frame);
      frame->format = context->pix_fmt;
      frame->width = context->width;
      frame->height = context->height;
      frame->pts = pts;
      frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
      frame->flags = idr ? AV_FRAME_FLAG_KEY : 0;
      frame->color_range = context->color_range;
      frame->color_primaries = context->color_primaries;
      frame->color_trc = context->color_trc;
      frame->colorspace = context->colorspace;

      CFDictionaryRef surface_attributes = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      ASSERT_NE(surface_attributes, nullptr);
      const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
      const void *values[] = {surface_attributes};
      CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFRelease(surface_attributes);
      ASSERT_NE(attributes, nullptr);
      CVPixelBufferRef buffer = nullptr;
      const CVReturn result = CVPixelBufferCreate(kCFAllocatorDefault, frame->width, frame->height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &buffer);
      CFRelease(attributes);
      ASSERT_EQ(result, kCVReturnSuccess);
      ASSERT_NE(buffer, nullptr);
      frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(buffer), 0, [](void *, uint8_t *data) {
        CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data));
      }, nullptr, 0);
      if (!frame->buf[0]) {
        CVPixelBufferRelease(buffer);
        FAIL() << "Cannot retain synthetic pixel buffer";
      }
      frame->data[3] = reinterpret_cast<uint8_t *>(buffer);
      ASSERT_NE(CVPixelBufferGetIOSurface(buffer), nullptr);
      ASSERT_EQ(CVPixelBufferLockBaseAddress(buffer, 0), kCVReturnSuccess);
      for (size_t plane = 0; plane < CVPixelBufferGetPlaneCount(buffer); ++plane) {
        auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
        const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane);
        const size_t height = CVPixelBufferGetHeightOfPlane(buffer, plane);
        // Initialize padding as well as visible pixels, with changing luma rows.
        for (size_t row = 0; row < height; ++row) {
          std::memset(base + row * stride, plane == 0 ? 16 + (row + pts * 3) % 220 : 128, stride);
        }
      }
      ASSERT_EQ(CVPixelBufferUnlockBaseAddress(buffer, 0), kCVReturnSuccess);
    }
  };

  TEST_P(VideoToolboxOutputCompletion, SyntheticCadence) {
    ASSERT_NO_FATAL_FAILURE(initialize());
    const bool completion = std::get<1>(GetParam());
    constexpr int frame_count = 30;
    constexpr int64_t first_pts = 100;
    constexpr int64_t idr_pts = first_pts + 15;
    std::map<int64_t, clock_type::time_point> pending;
    int received = 0;
    const auto start = clock_type::now();

    auto receive = [&]() {
      int result;
      while ((result = avcodec_receive_packet(context, packet)) == 0) {
        const auto found = pending.find(packet->pts);
        EXPECT_NE(found, pending.end()) << "Unexpected/duplicate PTS " << packet->pts;
        if (found != pending.end()) {
          const double elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - found->second).count();
          std::cout << "VT packet pts=" << packet->pts << " submission_to_packet_ms=" << elapsed << std::endl;
          pending.erase(found);
        }
        EXPECT_GT(packet->size, 0);
        if (packet->pts == first_pts || packet->pts == idr_pts) {
          EXPECT_NE(packet->flags & AV_PKT_FLAG_KEY, 0) << "Requested IDR PTS " << packet->pts;
        }
        ++received;
        av_packet_unref(packet);
      }
      return result;
    };

    for (int index = 0; index < frame_count; ++index) {
      std::this_thread::sleep_until(start + std::chrono::microseconds(index * 1000000 / 60));
      const int64_t pts = first_pts + index;
      ASSERT_NO_FATAL_FAILURE(prepare_frame(pts, pts == first_pts || pts == idr_pts));
      pending.emplace(pts, clock_type::now());
      // Raw avcodec is only an A/B measurement control, never a host fallback.
      ASSERT_EQ(completion ? platf::vt::send_frame(context, frame) : avcodec_send_frame(context, frame), 0);
      EXPECT_EQ(receive(), AVERROR(EAGAIN));
      std::cout << "VT submitted=" << index + 1 << " received=" << received << " backlog=" << pending.size() << " pending_pts=";
      for (const auto &[pending_pts, submitted] : pending) {
        std::cout << pending_pts << ',';
      }
      std::cout << std::endl;
      if (completion) {
        ASSERT_TRUE(pending.empty()) << "Output must be available without another submission";
        ASSERT_EQ(received, index + 1);
      }
    }
    ASSERT_EQ(platf::vt::send_frame(context, nullptr), 0);
    EXPECT_EQ(receive(), AVERROR_EOF);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(received, frame_count);
    EXPECT_EQ(platf::vt::send_frame(context, frame), AVERROR_EOF);
    EXPECT_EQ(platf::vt::send_frame(context, nullptr), AVERROR_EOF);
    std::cout << "VT final frames=" << received << " backlog=" << pending.size() << std::endl;
  }

  INSTANTIATE_TEST_SUITE_P(
    Native,
    VideoToolboxOutputCompletion,
    testing::Combine(testing::Values("h264_videotoolbox", "hevc_videotoolbox"), testing::Values(true, false)),
    [](const testing::TestParamInfo<VideoToolboxOutputCompletion::ParamType> &info) {
      return std::string(std::get<0>(info.param)) + (std::get<1>(info.param) ? "_Complete" : "_Control");
    }
  );

  TEST(VideoToolboxOutputCompletionErrors, UnopenedContext) {
    AVCodecContext *context = avcodec_alloc_context3(nullptr);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(platf::vt::send_frame(context, nullptr), AVERROR(EINVAL));
    avcodec_free_context(&context);
  }
}  // namespace
#endif
