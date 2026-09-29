/**
 * @file src/platform/macos/vt_output_completion.h
 * @brief Synchronous VideoToolbox output completion for FFmpeg submissions.
 */
#pragma once

#include <VideoToolbox/VideoToolbox.h>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVDictionary;

namespace platf::vt {
  struct frame_delay_setting {
    int requested = -1;
    OSStatus supported_status = noErr;
    std::string supported_description;
    std::optional<OSStatus> setter_status;
  };

  // -1 leaves the native property unset. Call after encoder options, before prepare.
  frame_delay_setting apply_max_frame_delay(VTCompressionSessionRef session, int requested);

  // Benchmark-only, single-session observation with one outstanding submission.
  // Keep alive through session destruction and use on the open/send thread.
  // The callback count advances only after the original FFmpeg callback returns.
  struct encode_observation {
    frame_delay_setting setting;
    bool prepare_observed = false;
    VTCompressionOutputCallback callback = nullptr;
    void *callback_opaque = nullptr;
    std::atomic<size_t> completed {0};
    std::atomic<int64_t> last_callback_ns {0};
    size_t submitted = 0;
    size_t pending_at_return = 0;
    bool callback_completed_at_return = false;
    OSStatus encode_status = noErr;
  };
  void set_encode_observation(encode_observation *observation);
  void observe_output_callback(VTCompressionOutputCallback &callback, void *&opaque);

  // Thread-local observation of session creation; callbacks may retain the session
  // to inspect properties after avcodec_open2 finishes applying encoder options.
  using session_observer = void (*)(VTCompressionSessionRef, void *);
  void set_session_observer(session_observer observer, void *opaque);
  void observe_session(VTCompressionSessionRef session);

  // Returns an owned copy only for automatic selection; nullptr leaves the specification unchanged.
  CFDictionaryRef copy_encoder_specification(CFDictionaryRef specification, bool automatic);

  // Apply before avcodec_open2. Inherit (-1) keeps speed=1 and the FFmpeg power default.
  // H.264 coder: -1/0 leave the default, 1 requests CABAC, 2 requests CAVLC.
  void apply_encoder_options(AVCodecContext *context, AVDictionary **options, int speed, int power, int coder = -1);

  // Completes real VT submissions through their numeric PTS before FFmpeg polls
  // its callback queue. EOS and other encoders retain avcodec_send_frame semantics.
  int send_frame(AVCodecContext *context, const AVFrame *frame);

  void log_encoder_properties(VTCompressionSessionRef session);
}  // namespace platf::vt
