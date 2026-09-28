/**
 * @file src/platform/macos/encoder_benchmark_decode.mm
 * @brief Validate benchmark access units using VideoToolbox decompression.
 */
#include "encoder_benchmark_decode.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <set>

#include <VideoToolbox/VideoToolbox.h>

namespace platf::vt::benchmark {
  namespace {
    template<class T>
    struct cf_ref {
      T value = nullptr;
      cf_ref() = default;
      cf_ref(const cf_ref &) = delete;
      cf_ref &operator=(const cf_ref &) = delete;
      ~cf_ref() {
        if (value) {
          CFRelease(value);
        }
      }
    };

    struct frame_input {
      std::vector<uint8_t> data;
      CMTime pts;
      bool decoded = false;
    };

    size_t prefix_size(std::span<const uint8_t> data, size_t offset) {
      const size_t remaining = data.size() - offset;
      if (remaining >= 3 && data[offset] == 0 && data[offset + 1] == 0) {
        if (data[offset + 2] == 1) {
          return 3;
        }
        if (remaining >= 4 && data[offset + 2] == 0 && data[offset + 3] == 1) {
          return 4;
        }
      }
      return 0;
    }

    const char *prepare_packet(std::span<const uint8_t> data, bool hevc, frame_input &frame, std::array<std::span<const uint8_t>, 3> &parameter_sets) {
      size_t start = 0;
      while (start < data.size() && !prefix_size(data, start)) {
        if (data[start++] != 0) {
          return "Nonzero data before Annex B start code";
        }
      }
      if (start == data.size()) {
        return "Packet has no Annex B start code";
      }

      bool has_picture = false;
      while (start < data.size()) {
        const size_t payload = start + prefix_size(data, start);
        size_t next = payload;
        while (next < data.size() && !prefix_size(data, next)) {
          ++next;
        }
        size_t end = next;
        // Annex B trailing_zero_8bits are outside the NAL unit.
        while (end > payload && data[end - 1] == 0) {
          --end;
        }
        const auto nal = data.subspan(payload, end - payload);
        if (nal.size() < (hevc ? 2u : 1u) || (nal[0] & 0x80) || (hevc && !(nal[1] & 7))) {
          return "Empty or malformed NAL unit header";
        }
        const unsigned type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
        has_picture |= hevc ? type <= 31 : type >= 1 && type <= 5;
        const int parameter_index = hevc ? (type >= 32 && type <= 34 ? static_cast<int>(type - 32) : -1) :
                                          (type == 7 ? 1 : type == 8 ? 2 : -1);
        if (parameter_index >= 0) {
          auto &stored = parameter_sets[parameter_index];
          if (!stored.empty() && !std::equal(stored.begin(), stored.end(), nal.begin(), nal.end())) {
            return "Changing parameter sets are unsupported in a fixed-format benchmark";
          }
          stored = nal;
        } else {
          if (nal.size() > std::numeric_limits<uint32_t>::max() ||
              frame.data.max_size() - frame.data.size() < 4 ||
              nal.size() > frame.data.max_size() - frame.data.size() - 4) {
            return "NAL unit or access unit is too large";
          }
          const auto length = static_cast<uint32_t>(nal.size());
          for (int shift : {24, 16, 8, 0}) {
            frame.data.push_back(static_cast<uint8_t>(length >> shift));
          }
          frame.data.insert(frame.data.end(), nal.begin(), nal.end());
        }
        start = next;
      }
      return has_picture ? nullptr : "Packet contains no coded picture";
    }

    struct callback_state {
      int width;
      int height;
      std::mutex mutex;
      size_t decoded_frames = 0;
      OSStatus status = noErr;
      const char *error = nullptr;

      callback_state(int width, int height):
          width(width), height(height) {}

      void fail(OSStatus native_status, const char *message) {
        std::lock_guard lock(mutex);
        if (!error) {
          status = native_status;
          error = message;
        }
      }

      bool failed() {
        std::lock_guard lock(mutex);
        return error != nullptr;
      }
    };

    void decoded_frame(void *opaque, void *source, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts, CMTime) {
      auto &state = *static_cast<callback_state *>(opaque);
      if (status != noErr) {
        state.fail(status, "VideoToolbox decode callback failed");
        return;
      }
      if (!image || !source || (flags & kVTDecodeInfo_FrameDropped)) {
        state.fail(noErr, "Decoder dropped a frame or returned no image/frame context");
        return;
      }
      auto &frame = *static_cast<frame_input *>(source);
      if (CVPixelBufferGetWidth(image) != static_cast<size_t>(state.width) ||
          CVPixelBufferGetHeight(image) != static_cast<size_t>(state.height)) {
        state.fail(noErr, "Decoded dimensions do not match the benchmark");
        return;
      }
      if (!CMTIME_IS_NUMERIC(pts) || CMTimeCompare(pts, frame.pts) != 0) {
        state.fail(noErr, "Decoded PTS does not match the submitted packet");
        return;
      }
      std::lock_guard lock(state.mutex);
      if (frame.decoded) {
        if (!state.error) {
          state.error = "Decoder returned multiple images for one packet";
        }
        return;
      }
      frame.decoded = true;
      ++state.decoded_frames;
    }

    struct decoder_session {
      callback_state &state;
      VTDecompressionSessionRef value = nullptr;

      explicit decoder_session(callback_state &state):
          state(state) {}
      decoder_session(const decoder_session &) = delete;
      decoder_session &operator=(const decoder_session &) = delete;

      void close() {
        if (!value) {
          return;
        }
        const OSStatus finish = VTDecompressionSessionFinishDelayedFrames(value);
        if (finish != noErr) {
          state.fail(finish, "VTDecompressionSessionFinishDelayedFrames failed");
        }
        const OSStatus wait = VTDecompressionSessionWaitForAsynchronousFrames(value);
        if (wait != noErr) {
          state.fail(wait, "VTDecompressionSessionWaitForAsynchronousFrames failed");
        }
        VTDecompressionSessionInvalidate(value);
        CFRelease(value);
        value = nullptr;
      }

      ~decoder_session() {
        close();
      }
    };
  }  // namespace

  decode_result validate_decode(bool hevc, int width, int height, int timebase_num, int timebase_den, const std::vector<packet_view> &packets) {
    if (width <= 0 || height <= 0 || timebase_num <= 0 || timebase_den <= 0 || packets.empty()) {
      return {false, 0, 0, "Positive dimensions/timebase and at least one packet are required"};
    }
    std::vector<frame_input> frames;
    frames.reserve(packets.size());
    std::set<int64_t> timestamps;
    std::array<std::span<const uint8_t>, 3> parameter_sets;
    for (const auto &packet : packets) {
      if (packet.pts > std::numeric_limits<int64_t>::max() / timebase_num ||
          packet.pts < std::numeric_limits<int64_t>::min() / timebase_num ||
          !timestamps.insert(packet.pts).second) {
        return {false, 0, 0, "Packet PTS is duplicated or overflows CMTime"};
      }
      frames.push_back({{}, CMTimeMake(packet.pts * timebase_num, timebase_den)});
      if (const char *error = prepare_packet(packet.data, hevc, frames.back(), parameter_sets)) {
        return {false, 0, 0, error};
      }
    }
    if (parameter_sets[1].empty() || parameter_sets[2].empty() || (hevc && parameter_sets[0].empty())) {
      return {false, 0, 0, "Missing SPS/PPS or HEVC VPS parameter sets"};
    }
    const uint8_t *pointers[] = {parameter_sets[0].data(), parameter_sets[1].data(), parameter_sets[2].data()};
    const size_t sizes[] = {parameter_sets[0].size(), parameter_sets[1].size(), parameter_sets[2].size()};
    cf_ref<CMVideoFormatDescriptionRef> format;
    const OSStatus format_status = hevc ?
                                     CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, pointers, sizes, 4, nullptr, &format.value) :
                                     CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, pointers + 1, sizes + 1, 4, &format.value);
    if (format_status != noErr || !format.value) {
      return {false, 0, format_status, "Cannot create video format description from parameter sets"};
    }
    callback_state state {width, height};
    decoder_session session(state);
    VTDecompressionOutputCallbackRecord callback {decoded_frame, &state};
    const OSStatus create = VTDecompressionSessionCreate(kCFAllocatorDefault, format.value, nullptr, nullptr, &callback, &session.value);
    if (create != noErr || !session.value) {
      state.fail(create, "VTDecompressionSessionCreate failed");
    } else {
      // Frame storage and callback contexts remain alive until session.close().
      for (auto &frame : frames) {
        cf_ref<CMBlockBufferRef> block;
        OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, frame.data.data(), frame.data.size(), kCFAllocatorNull, nullptr, 0, frame.data.size(), 0, &block.value);
        if (status != noErr || !block.value) {
          state.fail(status, "CMBlockBufferCreateWithMemoryBlock failed");
          break;
        }
        cf_ref<CMSampleBufferRef> sample;
        const CMSampleTimingInfo timing {kCMTimeInvalid, frame.pts, kCMTimeInvalid};
        const size_t size = frame.data.size();
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, block.value, format.value, 1, 1, &timing, 1, &size, &sample.value);
        if (status != noErr || !sample.value) {
          state.fail(status, "CMSampleBufferCreateReady failed");
          break;
        }
        VTDecodeInfoFlags flags = 0;
        status = VTDecompressionSessionDecodeFrame(session.value, sample.value, 0, &frame, &flags);
        if (status != noErr) {
          state.fail(status, "VTDecompressionSessionDecodeFrame failed");
        } else if (flags & kVTDecodeInfo_FrameDropped) {
          state.fail(noErr, "VTDecompressionSessionDecodeFrame dropped a frame");
        }
        if (state.failed()) {
          break;
        }
      }
    }
    session.close();
    std::lock_guard lock(state.mutex);
    if (!state.error && state.decoded_frames != packets.size()) {
      state.error = "Decoder output count does not match packet count";
    }
    return {!state.error, state.decoded_frames, state.status, state.error ? state.error : ""};
  }
}  // namespace platf::vt::benchmark
