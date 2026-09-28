/**
 * @file src/platform/macos/vt_output_completion.h
 * @brief Synchronous VideoToolbox output completion for FFmpeg submissions.
 */
#pragma once

#include <VideoToolbox/VideoToolbox.h>

struct AVCodecContext;
struct AVFrame;
struct AVDictionary;

namespace platf::vt {
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
