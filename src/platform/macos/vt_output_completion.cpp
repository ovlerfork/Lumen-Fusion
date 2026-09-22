/**
 * @file src/platform/macos/vt_output_completion.cpp
 * @brief Completes submitted VideoToolbox frames before FFmpeg collects output.
 */
#include "vt_output_completion.h"

#include <dlfcn.h>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "src/logging.h"

namespace {
  struct completion_scope;
  thread_local completion_scope *active_completion = nullptr;

  struct completion_scope {
    completion_scope *previous = active_completion;
    OSStatus status = noErr;

    explicit completion_scope(bool enabled) {
      active_completion = enabled ? this : nullptr;
    }

    ~completion_scope() {
      active_completion = previous;
    }

    completion_scope(const completion_scope &) = delete;
    completion_scope &operator=(const completion_scope &) = delete;
  };
}  // namespace

extern "C" OSStatus VTCompressionSessionEncodeFrame(
  VTCompressionSessionRef session,
  CVImageBufferRef image_buffer,
  CMTime presentation_timestamp,
  CMTime duration,
  CFDictionaryRef frame_properties,
  void *source_frame_refcon,
  VTEncodeInfoFlags *info_flags_out
) {
  static const auto encode = reinterpret_cast<decltype(&VTCompressionSessionEncodeFrame)>(
    dlsym(RTLD_NEXT, "VTCompressionSessionEncodeFrame")
  );
  if (!encode) {
    return kVTInvalidSessionErr;
  }

  const OSStatus status = encode(session, image_buffer, presentation_timestamp, duration, frame_properties, source_frame_refcon, info_flags_out);
  if (status == noErr && active_completion) {
    // This blocks in the driver; there is no cancellable timeout. No callback
    // locks are held. A numeric PTS completes this submission without draining EOS.
    const OSStatus completion_status = CMTIME_IS_NUMERIC(presentation_timestamp) ?
                                         VTCompressionSessionCompleteFrames(session, presentation_timestamp) :
                                         paramErr;
    if (completion_status != noErr && active_completion->status == noErr) {
      active_completion->status = completion_status;
    }
  }

  // Once EncodeFrame succeeds, VT owns source_frame_refcon. Returning a later
  // completion failure here would make FFmpeg free it again. The enclosing send
  // reports that failure after FFmpeg has handled the successful submission.
  return status;
}

namespace platf::vt {
  int send_frame(AVCodecContext *context, const AVFrame *frame) {
    const bool enabled = frame && context && context->codec && context->codec->name &&
                         std::string_view(context->codec->name).ends_with("_videotoolbox");
    completion_scope scope(enabled);
    const int result = avcodec_send_frame(context, frame);
    if (scope.status != noErr) {
      BOOST_LOG(error) << "VideoToolbox frame completion failed: OSStatus " << scope.status;
      return AVERROR_EXTERNAL;
    }
    return result;
  }

  void log_encoder_properties(VTCompressionSessionRef session) {
    for (auto key : {kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kVTCompressionPropertyKey_EncoderID}) {
      const char *name = key == kVTCompressionPropertyKey_EncoderID ? "EncoderID" : "UsingHardwareAcceleratedVideoEncoder";
      CFTypeRef value = nullptr;
      const OSStatus status = VTSessionCopyProperty(session, key, kCFAllocatorDefault, &value);
      if (status != noErr || !value) {
        BOOST_LOG(info) << "VideoToolbox " << name << "=" << (status == kVTPropertyNotSupportedErr ? "unsupported" : "unknown") << " (OSStatus " << status << ")";
      } else if (key == kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder && CFGetTypeID(value) == CFBooleanGetTypeID()) {
        BOOST_LOG(info) << "VideoToolbox " << name << "=" << (CFBooleanGetValue(static_cast<CFBooleanRef>(value)) ? "true" : "false");
      } else if (key == kVTCompressionPropertyKey_EncoderID && CFGetTypeID(value) == CFStringGetTypeID()) {
        char text[1024] {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), text, sizeof(text), kCFStringEncodingUTF8)) {
          BOOST_LOG(info) << "VideoToolbox " << name << "=" << text;
        } else {
          BOOST_LOG(info) << "VideoToolbox " << name << "=unknown (string conversion failed)";
        }
      } else {
        BOOST_LOG(info) << "VideoToolbox " << name << "=unknown (unexpected property type)";
      }
      if (value) {
        CFRelease(value);
      }
    }
  }
}  // namespace platf::vt
