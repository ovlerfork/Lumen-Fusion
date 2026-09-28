/**
 * @file src/platform/macos/encoder_benchmark.mm
 * @brief Isolated synthetic VideoToolbox encoder measurements in the native app.
 */
#include "encoder_benchmark.h"
#include "encoder_benchmark_decode.h"
#include "vt_output_completion.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/log/core.hpp>
#include <nlohmann/json.hpp>

#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/utsname.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "src/config.h"

namespace {
  using clock_type = std::chrono::steady_clock;
  using json = nlohmann::json;

  struct options {
    std::string codec = "h264";
    std::string variant = "baseline";
    std::filesystem::path output = "encoder-benchmark";
    int width = 1600;
    int height = 1112;
    int fps = 60;
    int bitrate = 20000000;
    int frames = 180;
    int warmup = 30;
    int repeat = 1;
    bool paced = true;
    bool user_initiated = true;
    bool automatic = false;
    int speed = -1;
    int power = -1;
    int coder = -1;
  };

  void require(bool condition, std::string_view message) {
    if (!condition) {
      throw std::runtime_error(std::string(message));
    }
  }

  void check(int status, const char *operation) {
    if (status < 0) {
      char error[AV_ERROR_MAX_STRING_SIZE] {};
      av_strerror(status, error, sizeof(error));
      throw std::runtime_error(std::string(operation) + ": " + error);
    }
  }

  int integer(std::string_view value, int low, int high, const std::string &name) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc {} && parsed.ptr == value.data() + value.size() && result >= low && result <= high,
            name + " must be an integer in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
    return result;
  }

  options parse(int argc, char **argv) {
    options o;
    for (int i = 0; i < argc; ++i) {
      const std::string name = argv[i];
      if (name == "--unpaced" || name == "--paced") {
        o.paced = name == "--paced";
        continue;
      }
      require(i + 1 < argc, "Missing value for " + name);
      const std::string value = argv[++i];
      if (name == "--codec") {
        o.codec = value;
      } else if (name == "--variant") {
        o.variant = value;
      } else if (name == "--output") {
        require(!value.empty(), "--output must be a file prefix");
        o.output = value;
      } else if (name == "--qos") {
        require(value == "inherit" || value == "user-initiated", "--qos: inherit or user-initiated");
        o.user_initiated = value == "user-initiated";
      } else if (name == "--width") {
        o.width = integer(value, 64, 4096, name);
      } else if (name == "--height") {
        o.height = integer(value, 64, 4096, name);
      } else if (name == "--fps") {
        o.fps = integer(value, 1, 240, name);
      } else if (name == "--bitrate") {
        o.bitrate = integer(value, 100000, 200000000, name);
      } else if (name == "--frames") {
        o.frames = integer(value, 2, 10000, name);
      } else if (name == "--warmup") {
        o.warmup = integer(value, 0, 1000, name);
      } else if (name == "--repeat") {
        o.repeat = integer(value, 1, 100, name);
      } else {
        throw std::runtime_error("Unknown benchmark option: " + name);
      }
    }
    require(o.codec == "h264" || o.codec == "hevc", "--codec: h264 or hevc");
    require(o.width % 2 == 0 && o.height % 2 == 0, "NV12 requires even width and height");
    require(o.variant == "baseline" || o.variant == "auto" || o.variant == "auto-poweroff" ||
              o.variant == "speedoff" || o.variant == "h264-cavlc", "Unknown --variant");
    o.automatic = o.variant == "auto" || o.variant == "auto-poweroff";
    o.power = o.variant == "auto-poweroff" ? 0 : -1;
    o.speed = o.variant == "speedoff" ? 0 : -1;
    if (o.variant == "h264-cavlc") {
      require(o.codec == "h264", "h264-cavlc requires --codec h264");
      o.coder = 2;
    }
    o.output = std::filesystem::weakly_canonical(std::filesystem::absolute(o.output));
    for (const auto &component : o.output) {
      require(component.extension() != ".app", "--output must be outside an app bundle");
    }
    require(std::filesystem::is_directory(o.output.parent_path()), "--output parent directory must exist");
    for (int run = 1; run <= o.repeat; ++run) {
      const auto prefix = o.output.string() + "-r" + std::to_string(run);
      require(!std::filesystem::exists(prefix + ".csv") && !std::filesystem::exists(prefix + ".json"),
              "Output already exists: " + prefix);
    }
    return o;
  }

  struct packet_deleter {
    void operator()(AVPacket *packet) const {
      av_packet_free(&packet);
    }
  };

  // Packet storage remains alive after timing for bitstream/decode validation.
  struct sample {
    std::unique_ptr<AVPacket, packet_deleter> packet {av_packet_alloc()};
    int64_t tick = 0;
    int64_t scheduled_tick = 0;
    int64_t skipped = 0;
    double deadline = 0;
    double sleep_enter = 0;
    double wake = 0;
    double prep_enter = 0;
    double prep_exit = 0;
    double send_enter = 0;
    double send_exit = 0;
    double receipt = 0;
    double loop_exit = 0;
    size_t backlog = 0;
    bool idr_requested = false;
    bool key_nal = false;
  };

  struct encoder {
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    AVDictionary *dictionary = nullptr;
    VTCompressionSessionRef session = nullptr;
    CVPixelBufferPoolRef pool = nullptr;

    ~encoder() {
      platf::vt::set_session_observer(nullptr, nullptr);
      av_frame_free(&frame);
      av_packet_free(&packet);
      avcodec_free_context(&context);
      av_dict_free(&dictionary);
      if (session) {
        CFRelease(session);
      }
      if (pool) {
        CVPixelBufferPoolRelease(pool);
      }
    }
  };

  json property(VTCompressionSessionRef session, CFStringRef key) {
    CFTypeRef value = nullptr;
    const OSStatus status = VTSessionCopyProperty(session, key, kCFAllocatorDefault, &value);
    json result = {{"status", status}, {"value", nullptr}};
    if (value) {
      if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        result["value"] = bool(CFBooleanGetValue(static_cast<CFBooleanRef>(value)));
      } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char text[2048] {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), text, sizeof(text), kCFStringEncodingUTF8)) {
          result["value"] = text;
        }
      } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        int64_t number = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number)) {
          result["value"] = number;
        }
      }
      CFRelease(value);
    }
    return result;
  }

  json open(encoder &e, const options &o) {
    const std::string name = o.codec + "_videotoolbox";
    const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
    require(codec, "Linked FFmpeg has no " + name);
    e.context = avcodec_alloc_context3(codec);
    e.frame = av_frame_alloc();
    e.packet = av_packet_alloc();
    require(e.context && e.frame && e.packet, "Cannot allocate encoder objects");
    auto *ctx = e.context;
    ctx->width = o.width;
    ctx->height = o.height;
    ctx->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
    ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
    ctx->profile = o.codec == "h264" ? AV_PROFILE_H264_HIGH : AV_PROFILE_HEVC_MAIN;
    ctx->time_base = {1, o.fps};
    ctx->framerate = {o.fps, 1};
    ctx->max_b_frames = 0;
    ctx->gop_size = std::numeric_limits<int>::max();
    ctx->keyint_min = std::numeric_limits<int>::max();
    ctx->flags = AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 = AV_CODEC_FLAG2_FAST;
    ctx->bit_rate = o.bitrate;
    ctx->rc_min_rate = o.bitrate;
    ctx->rc_max_rate = o.bitrate;
    ctx->rc_buffer_size = o.bitrate / o.fps;
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->colorspace = AVCOL_SPC_BT709;
    check(av_opt_set_int(ctx->priv_data, "allow_sw", 0, 0), "allow_sw");
    check(av_opt_set_int(ctx->priv_data, "require_sw", 0, 0), "require_sw");
    check(av_opt_set_int(ctx->priv_data, "realtime", 1, 0), "realtime");
    if (o.codec == "hevc") {
      check(av_opt_set_int(ctx->priv_data, "max_ref_frames", 1, 0), "max_ref_frames");
    }
    platf::vt::apply_encoder_options(ctx, &e.dictionary, o.speed, o.power, o.coder);
    platf::vt::set_session_observer([](VTCompressionSessionRef session, void *opaque) {
      auto &target = *static_cast<encoder *>(opaque);
      if (target.session) {
        CFRelease(target.session);
      }
      CFRetain(session);
      target.session = session;
    }, &e);
    const auto started = clock_type::now();
    const int status = avcodec_open2(ctx, codec, &e.dictionary);
    const double elapsed = std::chrono::duration<double, std::milli>(clock_type::now() - started).count();
    platf::vt::set_session_observer(nullptr, nullptr);
    check(status, "avcodec_open2");
    require(av_dict_count(e.dictionary) == 0, "Encoder left unconsumed options");
    require(e.session, "Native session observer did not see the encoder session");
    return {
      {"open_ms", elapsed},
      {"EncoderID", property(e.session, kVTCompressionPropertyKey_EncoderID)},
      {"UsingHardwareAcceleratedVideoEncoder", property(e.session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)},
      {"PrioritizeEncodingSpeedOverQuality", property(e.session, CFSTR("PrioritizeEncodingSpeedOverQuality"))},
      {"MaximizePowerEfficiency", property(e.session, CFSTR("MaximizePowerEfficiency"))},
      {"H264EntropyMode", property(e.session, CFSTR("H264EntropyMode"))},
      {"ProfileLevel", property(e.session, kVTCompressionPropertyKey_ProfileLevel)}
    };
  }

  using backgrounds = std::array<std::vector<uint8_t>, 4>;

  backgrounds prepare_source(encoder &e, const options &o) {
    NSDictionary *attributes = @{
      (__bridge id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
      (__bridge id) kCVPixelBufferWidthKey: @(o.width),
      (__bridge id) kCVPixelBufferHeightKey: @(o.height),
      (__bridge id) kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    require(CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef) attributes, &e.pool) == kCVReturnSuccess,
            "Cannot create NV12 pixel buffer pool");
    backgrounds scenes;
    for (size_t scene = 0; scene < scenes.size(); ++scene) {
      auto &pixels = scenes[scene];
      pixels.resize(size_t(o.width) * o.height * 3 / 2);
      for (int y = 0; y < o.height; ++y) {
        for (int x = 0; x < o.width; ++x) {
          pixels[size_t(y) * o.width + x] = 16 + (scene * 37 + x / 7 + y / 5) % 220;
        }
      }
      const size_t chroma = size_t(o.width) * o.height;
      for (int y = 0; y < o.height / 2; ++y) {
        for (int x = 0; x < o.width; x += 2) {
          pixels[chroma + size_t(y) * o.width + x] = 64 + scene * 18 + (x / 13) % 45;
          pixels[chroma + size_t(y) * o.width + x + 1] = 150 - scene * 15 + (y / 9) % 45;
        }
      }
    }
    return scenes;
  }

  void prepare_frame(encoder &e, const options &o, const backgrounds &scenes, int index, bool idr) {
    av_frame_unref(e.frame);
    auto *frame = e.frame;
    frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
    frame->width = o.width;
    frame->height = o.height;
    frame->pts = index;
    frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame->flags = idr ? AV_FRAME_FLAG_KEY : 0;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    CVPixelBufferRef buffer = nullptr;
    require(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, e.pool, &buffer) == kCVReturnSuccess,
            "Cannot allocate source pixel buffer");
    frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(buffer), 0, [](void *, uint8_t *data) {
      CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data));
    }, nullptr, 0);
    if (!frame->buf[0]) {
      CVPixelBufferRelease(buffer);
      throw std::runtime_error("Cannot retain source pixel buffer");
    }
    frame->data[3] = reinterpret_cast<uint8_t *>(buffer);
    CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    require(CVPixelBufferLockBaseAddress(buffer, 0) == kCVReturnSuccess, "Cannot lock source pixel buffer");
    // Scene and motion depend only on submission index, never on skipped ticks.
    const auto &source = scenes[(index / o.fps) % scenes.size()];
    for (size_t plane = 0; plane < 2; ++plane) {
      auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
      const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane);
      const int rows = plane == 0 ? o.height : o.height / 2;
      const size_t offset = plane == 0 ? 0 : size_t(o.width) * o.height;
      for (int row = 0; row < rows; ++row) {
        std::memcpy(base + row * stride, source.data() + offset + size_t(row) * o.width, o.width);
        std::memset(base + row * stride + o.width, plane == 0 ? 16 : 128, stride - o.width);
      }
    }
    auto *luma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    const int block_width = std::min(240, o.width / 2);
    const int block_height = std::min(160, o.height / 2);
    const int left = (index * 13) % (o.width - block_width + 1);
    const int top = (index * 7) % (o.height - block_height + 1);
    for (int y = top; y < top + block_height; ++y) {
      for (int x = left; x < left + block_width; ++x) {
        auto &value = luma[y * stride + x];
        value = value <= 143 ? value + 92 : value - 128;
      }
    }
    require(CVPixelBufferUnlockBaseAddress(buffer, 0) == kCVReturnSuccess, "Cannot unlock source pixel buffer");
  }

  bool has_key_nal(const AVPacket &packet, bool h264) {
    for (int i = 0; i + 3 < packet.size; ++i) {
      if (packet.data[i] != 0 || packet.data[i + 1] != 0) {
        continue;
      }
      int header = -1;
      if (packet.data[i + 2] == 1) {
        header = i + 3;
      } else if (packet.data[i + 2] == 0 && packet.data[i + 3] == 1 && i + 4 < packet.size) {
        header = i + 4;
      }
      if (header >= 0) {
        const int type = h264 ? packet.data[header] & 0x1f : (packet.data[header] >> 1) & 0x3f;
        if (h264 ? type == 5 : type >= 16 && type <= 21) {
          return true;
        }
      }
    }
    return false;
  }

  void measure(encoder &e, const options &o, const backgrounds &scenes, std::vector<sample> &samples) {
    const auto start = clock_type::now();
    const auto now_ms = [&]() {
      return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
    };
    const auto deadline = [&](int64_t tick) {
      return start + std::chrono::nanoseconds((tick * 1000000000 + o.fps - 1) / o.fps);
    };
    const auto tick_at = [&](clock_type::time_point time) -> int64_t {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(time - start).count() * o.fps / 1000000000;
    };
    int submitted = 0;
    int received = 0;
    size_t retained_bytes = 0;
    auto receive = [&]() {
      int result;
      while ((result = avcodec_receive_packet(e.context, e.packet)) == 0) {
        const double receipt = now_ms();
        require(e.packet->pts >= 0 && e.packet->pts < submitted, "Unexpected packet PTS");
        auto &s = samples[e.packet->pts];
        require(s.packet->size == 0 && e.packet->size > 0, "Duplicate or empty packet");
        s.receipt = receipt;
        s.backlog = submitted - ++received;
        retained_bytes += e.packet->size;
        require(retained_bytes <= 512 * 1024 * 1024, "Retained packets exceed 512 MiB; reduce frame count or bitrate");
        av_packet_move_ref(s.packet.get(), e.packet);
      }
      return result;
    };
    int64_t next_tick = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
      auto &s = samples[i];
      const auto ready = clock_type::now();
      s.scheduled_tick = next_tick;
      if (o.paced && i > 0 && deadline(next_tick) <= ready) {
        s.scheduled_tick = tick_at(ready) + 1;
      }
      const auto requested = deadline(s.scheduled_tick);
      s.deadline = std::chrono::duration<double, std::milli>(requested - start).count();
      s.sleep_enter = now_ms();
      if (o.paced) {
        std::this_thread::sleep_until(requested);
      }
      const auto woke = clock_type::now();
      s.wake = std::chrono::duration<double, std::milli>(woke - start).count();
      s.tick = o.paced ? std::max(s.scheduled_tick, tick_at(woke)) : next_tick;
      s.skipped = s.tick - next_tick;
      s.idr_requested = i % o.fps == 0;
      s.prep_enter = now_ms();
      prepare_frame(e, o, scenes, static_cast<int>(i), s.idr_requested);
      s.prep_exit = now_ms();
      s.send_enter = now_ms();
      const int sent = platf::vt::send_frame(e.context, e.frame);
      s.send_exit = now_ms();
      check(sent, "send_frame");
      ++submitted;
      const int result = receive();
      if (result != AVERROR(EAGAIN)) {
        throw std::runtime_error("Encoder did not return EAGAIN after submission: " + std::to_string(result));
      }
      require(received == submitted, "Output backlog remains after synchronous completion");
      s.loop_exit = now_ms();
      next_tick = s.tick + 1;
    }
    check(platf::vt::send_frame(e.context, nullptr), "encoder flush");
    require(receive() == AVERROR_EOF, "Encoder flush did not reach EOF");
    require(received == static_cast<int>(samples.size()), "Packet count does not match submitted frames");
    int64_t skipped = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
      auto &s = samples[i];
      skipped += s.skipped;
      require(s.packet->pts == static_cast<int64_t>(i), "Packet PTS does not match submission index");
      require(s.packet->dts == AV_NOPTS_VALUE || s.packet->dts == s.packet->pts, "Unexpected DTS with B frames disabled");
      s.key_nal = has_key_nal(*s.packet, o.codec == "h264");
      require(bool(s.packet->flags & AV_PKT_FLAG_KEY) == s.key_nal, "Key flag disagrees with Annex B key NAL");
      require(!s.idr_requested || s.key_nal, "Requested keyframe is missing a key NAL");
    }
    require(skipped == next_tick - static_cast<int64_t>(samples.size()), "Skipped tick accounting mismatch");
  }

  json statistics(std::vector<double> values) {
    if (values.empty()) {
      return {{"count", 0}, {"mean", nullptr}, {"p50", nullptr}, {"p95", nullptr}, {"p99", nullptr}, {"max", nullptr}};
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
      return values[static_cast<size_t>(std::ceil(fraction * values.size())) - 1];
    };
    return {{"count", values.size()}, {"mean", mean}, {"p50", percentile(0.50)},
            {"p95", percentile(0.95)}, {"p99", percentile(0.99)}, {"max", values.back()}};
  }

  json summary(const std::vector<sample> &samples, size_t begin, size_t end, bool paced) {
    std::vector<double> prep, send, latency, sleep, lateness, outside_wait, outside_loop, interarrival;
    int64_t skipped = 0;
    size_t bytes = 0;
    size_t keyframes = 0;
    for (size_t i = begin; i < end; ++i) {
      const auto &s = samples[i];
      prep.push_back(s.prep_exit - s.prep_enter);
      send.push_back(s.send_exit - s.send_enter);
      latency.push_back(s.receipt - s.send_enter);
      if (paced) {
        sleep.push_back(s.wake - s.sleep_enter);
        lateness.push_back(std::max(0.0, s.wake - s.deadline));
      }
      if (i > begin) {
        outside_wait.push_back(s.send_enter - samples[i - 1].send_exit);
        outside_loop.push_back(s.prep_enter - samples[i - 1].loop_exit);
        interarrival.push_back(s.receipt - samples[i - 1].receipt);
      }
      skipped += s.skipped;
      bytes += s.packet->size;
      keyframes += s.key_nal;
    }
    const size_t count = end - begin;
    const double interval_ms = count > 1 ? samples[end - 1].receipt - samples[begin].receipt : 0;
    const double active_ms = count ? samples[end - 1].receipt - samples[begin].prep_enter : 0;
    return {
      {"submitted", count}, {"matched_packets", count}, {"bytes", bytes}, {"keyframes", keyframes},
      {"skipped_ticks_before_submissions", skipped}, {"final_backlog", count ? samples[end - 1].backlog : 0},
      {"actual_fps", interval_ms > 0 ? json((count - 1) * 1000.0 / interval_ms) : json(nullptr)},
      {"active_elapsed_ms", active_ms},
      {"bitrate_bps", active_ms > 0 ? json(bytes * 8000.0 / active_ms) : json(nullptr)},
      {"prep_ms", statistics(std::move(prep))}, {"send_ms", statistics(std::move(send))},
      {"submit_to_output_ms", statistics(std::move(latency))}, {"sleep_ms", statistics(std::move(sleep))},
      {"wake_lateness_ms", statistics(std::move(lateness))}, {"outside_send_wait_ms", statistics(std::move(outside_wait))},
      {"outside_loop_wait_ms", statistics(std::move(outside_loop))}, {"packet_interarrival_ms", statistics(std::move(interarrival))}
    };
  }

  json metadata(const options &o) {
    struct utsname system {};
    const int uname_status = uname(&system);
    Dl_info library {};
    const bool have_library = dladdr(reinterpret_cast<const void *>(&avcodec_version), &library) != 0;
    int relative_priority = 0;
    const auto qos = pthread_get_qos_class_np(pthread_self(), &relative_priority);
    int policy = 0;
    struct sched_param scheduling {};
    const int schedule_status = pthread_getschedparam(pthread_self(), &policy, &scheduling);
    return {
      {"kind", "synthetic_encoder_benchmark"},
      {"scope", "NV12 preparation and encoder submission/output; excludes host, capture, network, client and end-to-end latency"},
      {"app_version", PROJECT_VERSION}, {"app_commit", PROJECT_VERSION_COMMIT},
      {"app_executable", NSProcessInfo.processInfo.arguments.firstObject.UTF8String},
      {"compiler", __clang_version__},
      {"macos", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String},
      {"machine", uname_status == 0 ? system.machine : "unknown"},
      {"kernel", uname_status == 0 ? system.release : "unknown"},
      {"ffmpeg_version", av_version_info()}, {"avcodec_version", avcodec_version()},
      {"ffmpeg_configuration", avcodec_configuration()},
      {"avcodec_image", have_library && library.dli_fname ? library.dli_fname : "unknown"},
      {"build", "optimized NDEBUG app"},
      {"scheduling", {{"requested_qos", o.user_initiated ? "user-initiated" : "inherit"},
                      {"effective_qos", qos}, {"qos_relative_priority", relative_priority},
                      {"pthread_status", schedule_status}, {"policy", policy}, {"priority", scheduling.sched_priority}}},
      {"requested", {{"codec", o.codec}, {"encoder", o.codec + "_videotoolbox"},
                     {"profile", o.codec == "h264" ? "H264High" : "HEVCMain"},
                     {"width", o.width}, {"height", o.height}, {"fps", o.fps}, {"bitrate_bps", o.bitrate},
                     {"frames", o.frames}, {"warmup", o.warmup}, {"repeat", o.repeat}, {"paced", o.paced},
                     {"variant", o.variant}, {"vt_low_latency_rate_control", o.automatic ? "auto" : "inherit"},
                     {"vt_prio_speed", o.speed}, {"vt_power_efficient", o.power}, {"vt_coder", o.coder},
                     {"max_frame_delay", -1}, {"max_b_frames", 0}, {"allow_sw", 0}, {"require_sw", 0}, {"realtime", 1},
                     {"output_completion", true}, {"pixel_format", "NV12"}, {"color", "BT709 video range"}}},
      {"content", "four synthetic backgrounds and moving luma block; scene, motion, PTS and key requests use submission index"},
      {"time_units", "milliseconds relative to the first pacing tick; PTS/DTS in 1/fps"},
      {"statistics", "nearest-rank percentiles; matched packets only; warmup excluded from measured summary"},
      {"rate_definitions", "actual_fps=(packets-1)/(last-first receipt); bitrate=bytes*8/(last receipt-first prep entry)"},
      {"wait_definitions", "outside_send_wait spans previous send exit to next send entry; outside_loop_wait spans previous loop exit to next prep entry"},
      {"skipped_tick_definition", "all omitted ticks, including ticks missed before sleep and during late wake, assigned to following submission"},
      {"decoder_validation", "not_run"}
    };
  }

  void write_results(const options &o, int run, json report, const std::vector<sample> &samples) {
    report["repeat_index"] = run;
    report["warmup"] = summary(samples, 0, o.warmup, o.paced);
    report["measured"] = summary(samples, o.warmup, samples.size(), o.paced);
    report["cold_first_frame"] = {{"included_in", o.warmup ? "warmup" : "measured"},
                                   {"prep_ms", samples[0].prep_exit - samples[0].prep_enter},
                                   {"send_ms", samples[0].send_exit - samples[0].send_enter},
                                   {"submit_to_output_ms", samples[0].receipt - samples[0].send_enter}};
    report["packet_validation"] = {{"status", "passed"}, {"pts_matches", samples.size()},
                                    {"key_flags_match_nals", true}, {"requested_keys_present", true}};
    report["columns"] = {
      "index", "phase", "cold", "tick", "scheduled_tick", "skipped_ticks", "deadline_ms", "sleep_enter_ms", "wake_ms", "lateness_ms",
      "prep_enter_ms", "prep_exit_ms", "send_enter_ms", "send_exit_ms", "packet_receipt_ms", "loop_exit_ms",
      "pts", "dts", "key_requested", "key_flag", "key_nal", "bytes", "backlog"
    };
    report["rows"] = json::array();
    for (size_t i = 0; i < samples.size(); ++i) {
      const auto &s = samples[i];
      report["rows"].push_back(json::array({
        i, i < static_cast<size_t>(o.warmup) ? "warmup" : "measured", i == 0,
        s.tick, s.scheduled_tick, s.skipped, o.paced ? json(s.deadline) : json(nullptr),
        s.sleep_enter, s.wake, o.paced ? json(std::max(0.0, s.wake - s.deadline)) : json(nullptr),
        s.prep_enter, s.prep_exit, s.send_enter, s.send_exit, s.receipt, s.loop_exit,
        s.packet->pts, s.packet->dts == AV_NOPTS_VALUE ? json(nullptr) : json(s.packet->dts),
        s.idr_requested, bool(s.packet->flags & AV_PKT_FLAG_KEY), s.key_nal, s.packet->size, s.backlog
      }));
    }
    const auto prefix = o.output.string() + "-r" + std::to_string(run);
    std::ofstream csv(prefix + ".csv");
    std::ofstream structured(prefix + ".json");
    require(csv.good() && structured.good(), "Cannot open output files: " + prefix);
    for (size_t column = 0; column < report["columns"].size(); ++column) {
      csv << (column ? "," : "") << report["columns"][column].get<std::string>();
    }
    csv << '\n';
    for (const auto &row : report["rows"]) {
      for (size_t column = 0; column < row.size(); ++column) {
        csv << (column ? "," : "");
        if (!row[column].is_null()) {
          csv << row[column].dump();
        }
      }
      csv << '\n';
    }
    structured << report.dump(2) << '\n';
    csv.close();
    structured.close();
    require(!csv.fail() && !structured.fail(), "Failed to write output files: " + prefix);
    std::cout << "Encoder benchmark: " << prefix << ".{csv,json}; matched=" << o.frames
              << " encode_ms_p50=" << report["measured"]["submit_to_output_ms"]["p50"]
              << " encode_ms_p95=" << report["measured"]["submit_to_output_ms"]["p95"]
              << " actual_fps=" << report["measured"]["actual_fps"]
              << "; decoder_validation=" << report["decoder_validation"]["status"] << '\n';
  }
}  // namespace

namespace platf::vt {
  int benchmark_main(int argc, char **argv) {
    @autoreleasepool {
      try {
        if (argc == 1 && std::string_view(argv[0]) == "--help") {
          std::cout << "Synthetic encoder benchmark, excludes host/capture/end-to-end latency.\n"
                       "Usage: Lumina --benchmark [options]\n"
                       "  --codec h264|hevc                default h264\n"
                       "  --width N --height N             default 1600 x 1112, even, 64..4096\n"
                       "  --fps N --bitrate N              default 60 Hz, 20000000 bits/sec\n"
                       "  --frames N --warmup N --repeat N default 180 / 30 / 1\n"
                       "  --variant baseline|auto|auto-poweroff|speedoff|h264-cavlc\n"
                       "  --paced | --unpaced              default paced\n"
                       "  --qos inherit|user-initiated      default user-initiated (matches streaming)\n"
                       "  --output PREFIX                  default ./encoder-benchmark\n"
                       "Writes PREFIX-rN.csv and PREFIX-rN.json after each pass; parent must exist.\n"
                       "Completion is always enabled. Native decode validation runs after timing.\n";
          return 0;
        }
        const options o = parse(argc, argv);
#if !defined(NDEBUG) || !defined(__OPTIMIZE__)
        throw std::runtime_error("Measurements require an optimized Release app");
#endif
        boost::log::core::get()->set_logging_enabled(false);
        av_log_set_level(AV_LOG_ERROR);
        // Only this isolated process uses these values; no configuration is loaded.
        config::video.vt.vt_low_latency_rate_control = o.automatic ? "auto" : "inherit";
        config::video.vt.vt_max_frame_delay = -1;
        if (o.user_initiated) {
          require(pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0) == 0, "Cannot set USER_INITIATED QoS");
        }
        const json common = metadata(o);
        for (int run = 1; run <= o.repeat; ++run) {
          const auto setup_started = clock_type::now();
          encoder e;
          json report = common;
          report["encoder"] = open(e, o);
          const auto scenes = prepare_source(e, o);
          std::vector<sample> samples(o.warmup + o.frames);
          for (const auto &s : samples) {
            require(bool(s.packet), "Cannot allocate saved packet");
          }
          report["setup_ms_including_open"] = std::chrono::duration<double, std::milli>(clock_type::now() - setup_started).count();
          measure(e, o, scenes, samples);
          std::vector<benchmark::packet_view> packets;
          packets.reserve(samples.size());
          for (const auto &s : samples) {
            packets.push_back({std::span<const uint8_t>(s.packet->data, s.packet->size), s.packet->pts});
          }
          const auto decoded = benchmark::validate_decode(o.codec == "hevc", o.width, o.height,
                                                         e.context->time_base.num, e.context->time_base.den, packets);
          report["decoder_validation"] = {{"status", decoded.valid ? "passed" : "failed"},
                                          {"decoded_frames", decoded.decoded_frames},
                                          {"expected_frames", samples.size()},
                                          {"osstatus", decoded.status}, {"error", decoded.error},
                                          {"scope", "post-timing bitstream, frame count, dimensions and PTS; not image quality"}};
          write_results(o, run, std::move(report), samples);
          require(decoded.valid, "Native decode validation failed: " + decoded.error);
        }
        return 0;
      } catch (const std::exception &error) {
        std::cerr << "Encoder benchmark failed: " << error.what() << '\n';
        return 1;
      }
    }
  }
}  // namespace platf::vt
