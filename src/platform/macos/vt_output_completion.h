/**
 * @file src/platform/macos/vt_output_completion.h
 * @brief Synchronous VideoToolbox output completion for FFmpeg submissions.
 */
#pragma once

#include <VideoToolbox/VideoToolbox.h>

struct AVCodecContext;
struct AVFrame;

namespace platf::vt {
  // Completes real VT submissions through their numeric PTS before FFmpeg polls
  // its callback queue. EOS and other encoders retain avcodec_send_frame semantics.
  int send_frame(AVCodecContext *context, const AVFrame *frame);

  void log_encoder_properties(VTCompressionSessionRef session);
}  // namespace platf::vt
