/**
 * @file tests/integration/test_vt_output_completion.cpp
 * @brief Native VideoToolbox output delivery using the host's linked FFmpeg.
 */
#ifdef __APPLE__

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "src/platform/macos/vt_output_completion.h"

namespace {
  using clock_type = std::chrono::steady_clock;

  class VideoToolboxFixture: public testing::Test {
  protected:
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    CVPixelBufferPoolRef pixel_buffer_pool = nullptr;

    void TearDown() override {
      // Freeing the context also closes the native session on assertion failures.
      avcodec_free_context(&context);
      av_frame_free(&frame);
      av_packet_free(&packet);
      if (pixel_buffer_pool) {
        CVPixelBufferPoolRelease(pixel_buffer_pool);
      }
      pixel_buffer_pool = nullptr;
    }

    void initialize(const char *codec_name, bool completion = true) {
      std::cout << "VT measurement codec=" << codec_name << " completion=" << completion << " hardware_required=1"
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

    uint8_t background_luma(int scene, size_t x, size_t y) const {
      return static_cast<uint8_t>(16 + (scene * 37 + x / 7 + y / 5) % 220);
    }

    void paint_block(uint8_t *luma, uint8_t *chroma, size_t luma_stride, size_t chroma_stride, int scene, int block_x, int block_y) const {
      constexpr int block_width = 240;
      constexpr int block_height = 160;
      for (int y = block_y; y < block_y + block_height; ++y) {
        for (int x = block_x; x < block_x + block_width; ++x) {
          const uint8_t background = background_luma(scene, x, y);
          luma[y * luma_stride + x] = static_cast<uint8_t>(16 + (background - 16 + 92) % 220);
        }
      }
      for (int y = block_y / 2; y < (block_y + block_height) / 2; ++y) {
        for (int x = block_x; x < block_x + block_width; x += 2) {
          chroma[y * chroma_stride + x] = static_cast<uint8_t>(94 + scene * 18 + (x / 13) % 45);
          chroma[y * chroma_stride + x + 1] = static_cast<uint8_t>(125 - scene * 15 + (y / 9) % 45);
        }
      }
    }

    void fill_background(CVPixelBufferRef buffer, int scene) const {
      auto *luma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
      auto *chroma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
      const size_t luma_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
      const size_t chroma_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
      const size_t luma_width = CVPixelBufferGetWidthOfPlane(buffer, 0);
      const size_t chroma_width = 2 * CVPixelBufferGetWidthOfPlane(buffer, 1);
      const size_t luma_height = CVPixelBufferGetHeightOfPlane(buffer, 0);
      const size_t chroma_height = CVPixelBufferGetHeightOfPlane(buffer, 1);
      for (size_t y = 0; y < luma_height; ++y) {
        for (size_t x = 0; x < luma_width; ++x) {
          luma[y * luma_stride + x] = background_luma(scene, x, y);
        }
        std::memset(luma + y * luma_stride + luma_width, 16, luma_stride - luma_width);
      }
      for (size_t y = 0; y < chroma_height; ++y) {
        for (size_t x = 0; x < chroma_width; x += 2) {
          chroma[y * chroma_stride + x] = static_cast<uint8_t>(64 + scene * 18 + (x / 13) % 45);
          chroma[y * chroma_stride + x + 1] = static_cast<uint8_t>(150 - scene * 15 + (y / 9) % 45);
        }
        std::memset(chroma + y * chroma_stride + chroma_width, 128, chroma_stride - chroma_width);
      }
    }

    void initialize_pixel_buffer_pool() {
      ASSERT_EQ(pixel_buffer_pool, nullptr);
      const int width = context->width;
      const int height = context->height;
      const OSType pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
      CFNumberRef width_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
      CFNumberRef height_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
      CFNumberRef pixel_format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
      if (!width_number || !height_number || !pixel_format_number) {
        if (width_number) {
          CFRelease(width_number);
        }
        if (height_number) {
          CFRelease(height_number);
        }
        if (pixel_format_number) {
          CFRelease(pixel_format_number);
        }
        FAIL() << "Cannot create pixel buffer pool number attributes";
      }
      CFDictionaryRef surface_attributes = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      if (!surface_attributes) {
        CFRelease(width_number);
        CFRelease(height_number);
        CFRelease(pixel_format_number);
        FAIL() << "Cannot create pixel buffer pool surface attributes";
      }
      const void *keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey};
      const void *values[] = {width_number, height_number, pixel_format_number, surface_attributes};
      CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
      CFRelease(width_number);
      CFRelease(height_number);
      CFRelease(pixel_format_number);
      CFRelease(surface_attributes);
      if (!attributes) {
        FAIL() << "Cannot create pixel buffer pool attributes";
      }
      const CVReturn result = CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, attributes, &pixel_buffer_pool);
      CFRelease(attributes);
      if (result != kCVReturnSuccess || !pixel_buffer_pool) {
        if (pixel_buffer_pool) {
          CVPixelBufferPoolRelease(pixel_buffer_pool);
          pixel_buffer_pool = nullptr;
        }
        FAIL() << "Cannot create pixel buffer pool: " << result;
      }
    }

    void prepare_pooled_frame(int64_t pts, bool idr, int sequence) {
      av_frame_unref(frame);
      CVPixelBufferRef buffer = nullptr;
      const CVReturn result = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pixel_buffer_pool, &buffer);
      if (result != kCVReturnSuccess || !buffer) {
        if (buffer) {
          CVPixelBufferRelease(buffer);
        }
        FAIL() << "Cannot acquire pooled pixel buffer: " << result;
      }
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
      frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(buffer), 0, [](void *, uint8_t *data) {
        CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data));
      }, nullptr, 0);
      if (!frame->buf[0]) {
        CVPixelBufferRelease(buffer);
        FAIL() << "Cannot transfer pooled pixel buffer to AVFrame";
      }
      frame->data[3] = reinterpret_cast<uint8_t *>(buffer);
      ASSERT_EQ(CVPixelBufferGetPlaneCount(buffer), 2u);
      ASSERT_EQ(CVPixelBufferGetPixelFormatType(buffer), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
      ASSERT_NE(CVPixelBufferGetIOSurface(buffer), nullptr);

      const int scene = (sequence / 45) % 4;
      const int block_x = 2 * ((sequence * 23) % ((context->width - 240) / 2));
      const int block_y = 2 * ((sequence * 11) % ((context->height - 160) / 2));
      ASSERT_EQ(CVPixelBufferLockBaseAddress(buffer, 0), kCVReturnSuccess);
      fill_background(buffer, scene);
      paint_block(
        static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0)),
        static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1)),
        CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
        CVPixelBufferGetBytesPerRowOfPlane(buffer, 1),
        scene, block_x, block_y);
      ASSERT_EQ(CVPixelBufferUnlockBaseAddress(buffer, 0), kCVReturnSuccess);
    }

  };

  class VideoToolboxOutputCompletion: public VideoToolboxFixture, public testing::WithParamInterface<std::tuple<const char *, bool>> {
  protected:
    void initialize_parameterized() {
      initialize(std::get<0>(GetParam()), std::get<1>(GetParam()));
    }
  };

  TEST_P(VideoToolboxOutputCompletion, SyntheticCadence) {
    ASSERT_NO_FATAL_FAILURE(initialize_parameterized());
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

  class VideoToolboxPacedMeasurement: public VideoToolboxFixture, public testing::WithParamInterface<const char *> {};

  bool has_annex_b_idr(const AVPacket *packet, AVCodecID codec_id) {
    for (int index = 0; index + 3 < packet->size;) {
      int start_code_size = 0;
      if (packet->data[index] == 0 && packet->data[index + 1] == 0) {
        if (packet->data[index + 2] == 1) {
          start_code_size = 3;
        } else if (index + 4 < packet->size && packet->data[index + 2] == 0 && packet->data[index + 3] == 1) {
          start_code_size = 4;
        }
      }
      if (start_code_size == 0) {
        ++index;
        continue;
      }
      const int nal_index = index + start_code_size;
      if (nal_index >= packet->size) {
        break;
      }
      if (codec_id == AV_CODEC_ID_H264 && (packet->data[nal_index] & 0x1f) == 5) {
        return true;
      }
      if (codec_id == AV_CODEC_ID_HEVC) {
        const int nal_type = (packet->data[nal_index] >> 1) & 0x3f;
        if (nal_type == 19 || nal_type == 20) {
          return true;
        }
      }
      index = nal_index + 1;
    }
    return false;
  }

  TEST_P(VideoToolboxPacedMeasurement, Native60FpsMotionTail) {
    ASSERT_NO_FATAL_FAILURE(initialize(GetParam()));
    ASSERT_NO_FATAL_FAILURE(initialize_pixel_buffer_pool());
    constexpr int warmup_frames = 30;
    constexpr int measured_frames = 180;
    constexpr int total_frames = warmup_frames + measured_frames;
    constexpr int target_fps = 60;
    constexpr int64_t first_pts = 1000;
    struct pending_frame {
      clock_type::time_point submitted;
      bool idr;
      bool measured;
      int source_tick;
    };
    struct frame_metric {
      const char *phase;
      int source_tick;
      int64_t pts;
      double submit_ms;
      double output_ms;
      double interarrival_ms;
      double submit_to_output_ms;
      size_t backlog;
      bool idr_requested;
      bool key_packet;
      bool idr_nal;
    };
    std::map<int64_t, pending_frame> pending;
    std::vector<frame_metric> frame_metrics;
    std::vector<double> warmup_submit_to_output_ms;
    std::vector<double> warmup_interarrival_ms;
    std::vector<double> measured_submit_to_output_ms;
    std::vector<double> measured_interarrival_ms;
    frame_metrics.reserve(total_frames);
    warmup_submit_to_output_ms.reserve(warmup_frames);
    warmup_interarrival_ms.reserve(warmup_frames - 1);
    measured_submit_to_output_ms.reserve(measured_frames);
    measured_interarrival_ms.reserve(measured_frames - 1);
    std::optional<clock_type::time_point> previous_warmup_output;
    std::optional<clock_type::time_point> previous_measured_output;
    std::optional<clock_type::time_point> first_measured_output;
    std::optional<clock_type::time_point> last_measured_output;
    double cold_first_submit_to_output_ms = -1.0;
    int received = 0;
    int warmup_source_skipped_ticks = 0;
    int measured_source_skipped_ticks = 0;
    size_t max_pending = 0;

    auto percentile = [](std::vector<double> values, double fraction) {
      if (values.empty()) {
        return -1.0;
      }
      std::sort(values.begin(), values.end());
      const size_t index = static_cast<size_t>(std::ceil(fraction * values.size())) - 1;
      return values[std::min(index, values.size() - 1)];
    };

    const auto started = clock_type::now();
    auto receive = [&]() {
      int result;
      while ((result = avcodec_receive_packet(context, packet)) == 0) {
        // Record output before pacing so source overruns remain represented in the sample.
        const auto output_at = clock_type::now();
        const auto found = pending.find(packet->pts);
        EXPECT_NE(found, pending.end()) << "Unexpected/duplicate PTS " << packet->pts;
        if (found == pending.end()) {
          av_packet_unref(packet);
          continue;
        }
        const pending_frame submitted = found->second;
        const double submit_to_output_ms = std::chrono::duration<double, std::milli>(output_at - submitted.submitted).count();
        const bool key_packet = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        const bool idr_nal = has_annex_b_idr(packet, context->codec_id);
        EXPECT_GT(packet->size, 0);
        EXPECT_EQ(packet->pts, first_pts + submitted.source_tick);
        if (submitted.idr) {
          EXPECT_TRUE(key_packet) << "Requested IDR PTS " << packet->pts;
          EXPECT_TRUE(idr_nal) << "Requested IDR PTS " << packet->pts << " was not an Annex B IDR NAL";
        }
        pending.erase(found);
        if (received == 0) {
          cold_first_submit_to_output_ms = submit_to_output_ms;
        }
        double interarrival_ms = -1.0;
        auto &latencies = submitted.measured ? measured_submit_to_output_ms : warmup_submit_to_output_ms;
        auto &interarrivals = submitted.measured ? measured_interarrival_ms : warmup_interarrival_ms;
        auto &previous_output = submitted.measured ? previous_measured_output : previous_warmup_output;
        latencies.push_back(submit_to_output_ms);
        if (previous_output) {
          interarrival_ms = std::chrono::duration<double, std::milli>(output_at - *previous_output).count();
          interarrivals.push_back(interarrival_ms);
        }
        previous_output = output_at;
        if (submitted.measured) {
          if (!first_measured_output) {
            first_measured_output = output_at;
          }
          last_measured_output = output_at;
        }
        frame_metrics.push_back(frame_metric {
          submitted.measured ? "measured" : "warmup",
          submitted.source_tick,
          packet->pts,
          std::chrono::duration<double, std::milli>(submitted.submitted - started).count(),
          std::chrono::duration<double, std::milli>(output_at - started).count(),
          interarrival_ms,
          submit_to_output_ms,
          pending.size(),
          submitted.idr,
          key_packet,
          idr_nal,
        });
        ++received;
        av_packet_unref(packet);
      }
      return result;
    };

    const auto deadline_for_tick = [&](int tick) {
      // Form each deadline from the 60 Hz source clock, rather than accumulating
      // a truncated frame period.
      return started + std::chrono::duration_cast<clock_type::duration>(
        std::chrono::nanoseconds {(static_cast<int64_t>(tick) * 1000000000 + target_fps - 1) / target_fps});
    };
    const auto tick_due_at = [&](clock_type::time_point time) {
      const int64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time - started).count();
      return elapsed_ns <= 0 ? 0 : static_cast<int>(elapsed_ns * target_fps / 1000000000);
    };

    int source_tick = 0;
    std::optional<bool> previous_submission_measured;
    for (int submitted_count = 0; submitted_count < total_frames; ++submitted_count) {
      const int next_tick = source_tick;
      const auto ready_at = clock_type::now();
      if (submitted_count > 0 && deadline_for_tick(source_tick) <= ready_at) {
        // After an overrun, wait for a future tick instead of sending a catch-up frame.
        source_tick = tick_due_at(ready_at) + 1;
      }
      std::this_thread::sleep_until(deadline_for_tick(source_tick));

      const bool measured = submitted_count >= warmup_frames;
      const int due_tick = std::max(source_tick, tick_due_at(clock_type::now()));
      if (due_tick > next_tick) {
        // Omit expired source ticks instead of submitting an old frame or catching up.
        // The gap belongs to the interval after the preceding submitted frame.
        int &skipped_ticks = previous_submission_measured.value_or(measured) ?
          measured_source_skipped_ticks : warmup_source_skipped_ticks;
        skipped_ticks += due_tick - next_tick;
        source_tick = due_tick;
      }

      const bool idr = submitted_count % target_fps == 0;
      const int64_t pts = first_pts + source_tick;
      ASSERT_NO_FATAL_FAILURE(prepare_pooled_frame(pts, idr, source_tick));
      // The timestamp starts after pixel generation, so submission-to-packet time
      // remains a submission metric while generation still affects source cadence.
      const auto submitted_at = clock_type::now();
      ASSERT_TRUE(pending.emplace(pts, pending_frame {submitted_at, idr, measured, source_tick}).second);
      max_pending = std::max(max_pending, pending.size());
      ASSERT_EQ(platf::vt::send_frame(context, frame), 0);
      EXPECT_EQ(receive(), AVERROR(EAGAIN));
      ASSERT_TRUE(pending.empty()) << "Output backlog remains after completed submission";
      ASSERT_EQ(received, submitted_count + 1);

      previous_submission_measured = measured;
      ++source_tick;
    }
    EXPECT_EQ(warmup_source_skipped_ticks + measured_source_skipped_ticks, source_tick - total_frames);
    ASSERT_EQ(platf::vt::send_frame(context, nullptr), 0);
    EXPECT_EQ(receive(), AVERROR_EOF);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(received, total_frames);
    EXPECT_EQ(platf::vt::send_frame(context, frame), AVERROR_EOF);
    EXPECT_EQ(platf::vt::send_frame(context, nullptr), AVERROR_EOF);

    const double measured_elapsed_seconds = first_measured_output && last_measured_output && *last_measured_output > *first_measured_output ?
      std::chrono::duration<double>(*last_measured_output - *first_measured_output).count() : 0.0;
    const double effective_fps = measured_elapsed_seconds > 0.0 ?
      static_cast<double>(measured_frames - 1) / measured_elapsed_seconds : 0.0;

    std::cout << "VT_PACED_FRAME_CSV_BEGIN\n";
    std::cout << "record,codec,phase,source_tick,pts,submit_ms,output_ms,interarrival_ms,submit_to_output_ms,backlog,idr_requested,key_packet,idr_nal\n";
    std::cout << std::fixed << std::setprecision(3);
    for (const frame_metric &metric : frame_metrics) {
      std::cout << "frame," << GetParam() << ',' << metric.phase << ',' << metric.source_tick << ',' << metric.pts << ','
                << metric.submit_ms << ',' << metric.output_ms << ',' << metric.interarrival_ms << ',' << metric.submit_to_output_ms << ','
                << metric.backlog << ',' << metric.idr_requested << ',' << metric.key_packet << ',' << metric.idr_nal << '\n';
    }
    std::cout << "VT_PACED_FRAME_CSV_END\n";
    std::cout << "VT_PACED_SUMMARY_CSV_BEGIN\n";
    std::cout << "record,codec,target_fps,bitrate,warmup_frames,warmup_received,warmup_submit_to_output_p50_ms,warmup_submit_to_output_p95_ms,warmup_submit_to_output_p99_ms,warmup_submit_to_output_max_ms,warmup_interarrival_p50_ms,warmup_interarrival_p95_ms,warmup_interarrival_p99_ms,warmup_interarrival_max_ms,warmup_source_skipped_ticks,measured_frames,measured_received,measured_submit_to_output_p50_ms,measured_submit_to_output_p95_ms,measured_submit_to_output_p99_ms,measured_submit_to_output_max_ms,measured_interarrival_p50_ms,measured_interarrival_p95_ms,measured_interarrival_p99_ms,measured_interarrival_max_ms,measured_source_skipped_ticks,cold_first_submit_to_output_ms,measured_elapsed_seconds,elapsed_effective_fps,max_pending,final_backlog\n";
    std::cout << "summary," << GetParam() << ',' << target_fps << ',' << context->bit_rate << ','
              << warmup_frames << ',' << warmup_submit_to_output_ms.size() << ','
              << percentile(warmup_submit_to_output_ms, 0.50) << ',' << percentile(warmup_submit_to_output_ms, 0.95) << ','
              << percentile(warmup_submit_to_output_ms, 0.99) << ',' << percentile(warmup_submit_to_output_ms, 1.00) << ','
              << percentile(warmup_interarrival_ms, 0.50) << ',' << percentile(warmup_interarrival_ms, 0.95) << ','
              << percentile(warmup_interarrival_ms, 0.99) << ',' << percentile(warmup_interarrival_ms, 1.00) << ','
              << warmup_source_skipped_ticks << ',' << measured_frames << ',' << measured_submit_to_output_ms.size() << ','
              << percentile(measured_submit_to_output_ms, 0.50) << ',' << percentile(measured_submit_to_output_ms, 0.95) << ','
              << percentile(measured_submit_to_output_ms, 0.99) << ',' << percentile(measured_submit_to_output_ms, 1.00) << ','
              << percentile(measured_interarrival_ms, 0.50) << ',' << percentile(measured_interarrival_ms, 0.95) << ','
              << percentile(measured_interarrival_ms, 0.99) << ',' << percentile(measured_interarrival_ms, 1.00) << ','
              << measured_source_skipped_ticks << ',' << cold_first_submit_to_output_ms << ',' << measured_elapsed_seconds << ','
              << effective_fps << ',' << max_pending << ',' << pending.size() << '\n';
    std::cout << "VT_PACED_SUMMARY_CSV_END\n";
    EXPECT_EQ(warmup_submit_to_output_ms.size(), warmup_frames);
    EXPECT_EQ(warmup_interarrival_ms.size(), warmup_frames - 1);
    EXPECT_EQ(measured_submit_to_output_ms.size(), measured_frames);
    EXPECT_EQ(measured_interarrival_ms.size(), measured_frames - 1);
    EXPECT_EQ(frame_metrics.size(), total_frames);
  }

  INSTANTIATE_TEST_SUITE_P(
    NativePaced,
    VideoToolboxPacedMeasurement,
    testing::Values("h264_videotoolbox", "hevc_videotoolbox")
  );

  TEST(VideoToolboxOutputCompletionErrors, UnopenedContext) {
    AVCodecContext *context = avcodec_alloc_context3(nullptr);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(platf::vt::send_frame(context, nullptr), AVERROR(EINVAL));
    avcodec_free_context(&context);
  }
}  // namespace
#endif
