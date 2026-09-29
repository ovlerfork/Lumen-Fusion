/**
 * @file src/platform/macos/vt_output_completion.cpp
 * @brief Completes submitted VideoToolbox frames before FFmpeg collects output.
 */
#include "vt_output_completion.h"

#include <chrono>
#include <dlfcn.h>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "src/config.h"
#include "src/logging.h"

namespace {
  thread_local platf::vt::encode_observation *observation = nullptr;
  int64_t observation_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  struct completion_scope;
  thread_local completion_scope *active_completion = nullptr;
  thread_local platf::vt::session_observer session_created = nullptr;
  thread_local void *session_observer_opaque = nullptr;

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

extern "C" OSStatus VTSessionSetProperty(VTSessionRef session, CFStringRef key, CFTypeRef value) {
  static const auto set_property = reinterpret_cast<decltype(&VTSessionSetProperty)>(
    dlsym(RTLD_NEXT, "VTSessionSetProperty")
  );
  if (!set_property) {
    BOOST_LOG(error) << "Failed to resolve the system VTSessionSetProperty function";
    return kVTInvalidSessionErr;
  }
  const OSStatus status = set_property(session, key, value);
  const char *name = nullptr;
  if (key && CFEqual(key, CFSTR("PrioritizeEncodingSpeedOverQuality"))) {
    name = "PrioritizeEncodingSpeedOverQuality";
  } else if (key && CFEqual(key, CFSTR("MaximizePowerEfficiency"))) {
    name = "MaximizePowerEfficiency";
  } else if (key && CFEqual(key, CFSTR("H264EntropyMode"))) {
    name = "H264EntropyMode";
  }
  if (name) {
    if (status != noErr) {
      BOOST_LOG(warning) << "VideoToolbox " << name << ": "
                         << (status == kVTPropertyNotSupportedErr ? "unsupported" : "rejected")
                         << " (OSStatus " << status << ")";
    } else {
      CFTypeRef actual = nullptr;
      const OSStatus read_status = VTSessionCopyProperty(session, key, kCFAllocatorDefault, &actual);
      if (read_status == noErr && actual && value && CFEqual(actual, value) && CFGetTypeID(actual) == CFBooleanGetTypeID()) {
        BOOST_LOG(info) << "Applied VideoToolbox " << name << "=" << (CFBooleanGetValue(static_cast<CFBooleanRef>(actual)) ? "true" : "false");
      } else if (read_status == noErr && actual && CFGetTypeID(actual) == CFStringGetTypeID()) {
        char text[128] {};
        if (CFStringGetCString(static_cast<CFStringRef>(actual), text, sizeof(text), kCFStringEncodingUTF8)) {
          BOOST_LOG(info) << "VideoToolbox " << name << "=" << text;
          if (!value || !CFEqual(actual, value)) {
            BOOST_LOG(warning) << "VideoToolbox " << name << " readback differs from the requested value";
          }
        } else {
          BOOST_LOG(warning) << "VideoToolbox " << name << " readback string conversion failed";
        }
      } else {
        BOOST_LOG(warning) << "VideoToolbox " << name << " accepted but readback is unavailable or differs (OSStatus " << read_status << ")";
      }
      if (actual) {
        CFRelease(actual);
      }
    }
  }
  return status;
}

extern "C" OSStatus VTCompressionSessionPrepareToEncodeFrames(VTCompressionSessionRef session) {
  static const auto prepare = reinterpret_cast<decltype(&VTCompressionSessionPrepareToEncodeFrames)>(
    dlsym(RTLD_NEXT, "VTCompressionSessionPrepareToEncodeFrames")
  );
  if (!prepare) {
    return kVTInvalidSessionErr;
  }
  const int requested = config::video.vt.vt_max_frame_delay;
  auto setting = platf::vt::apply_max_frame_delay(session, requested);
  if (observation) {
    observation->setting = std::move(setting);
    observation->prepare_observed = true;
  }
  const OSStatus prepare_status = prepare(session);
  if (requested >= 0) {
    CFTypeRef value = nullptr;
    const OSStatus read_status = VTSessionCopyProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, kCFAllocatorDefault, &value);
    int64_t actual = 0;
    const bool numeric = read_status == noErr && value && CFGetTypeID(value) == CFNumberGetTypeID() &&
                         CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &actual);
    if (value) {
      CFRelease(value);
    }
    // Readback describes the prepared session; it does not establish setter acceptance.
    BOOST_LOG(info) << "VideoToolbox MaxFrameDelayCount after prepare: requested=" << requested
                    << " prepare OSStatus=" << prepare_status << " read OSStatus=" << read_status
                    << " numeric=" << numeric << " value=" << (numeric ? std::to_string(actual) : "unknown")
                    << " mismatch=" << (numeric ? (actual != requested ? "true" : "false") : "unknown");
    if (!numeric || actual != requested) {
      BOOST_LOG(warning) << "VideoToolbox MaxFrameDelayCount readback is unavailable or differs from requested=" << requested;
    }
  }
  return prepare_status;
}

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

  if (observation) {
    ++observation->submitted;
  }
  const OSStatus status = encode(session, image_buffer, presentation_timestamp, duration, frame_properties, source_frame_refcon, info_flags_out);
  if (observation) {
    const int64_t returned_ns = observation_time_ns();
    // Snapshot immediately after the native call, before forced completion.
    // A concurrent callback may finish at this observation boundary.
    size_t completed = observation->completed.load(std::memory_order_acquire);
    // With one outstanding submission, exclude a callback that finished after
    // the return timestamp. A not-yet-published completion is conservatively pending.
    if (completed == observation->submitted && observation->last_callback_ns.load(std::memory_order_acquire) > returned_ns) {
      --completed;
    }
    observation->pending_at_return = observation->submitted - completed;
    observation->callback_completed_at_return = completed == observation->submitted;
    observation->encode_status = status;
  }
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
  void set_encode_observation(encode_observation *value) {
    observation = value;
  }

  void observe_output_callback(VTCompressionOutputCallback &callback, void *&opaque) {
    if (!observation || !callback) {
      return;
    }
    observation->callback = callback;
    observation->callback_opaque = opaque;
    opaque = observation;
    callback = [](void *context, void *source, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef buffer) {
      auto &state = *static_cast<encode_observation *>(context);
      state.callback(state.callback_opaque, source, status, flags, buffer);
      state.last_callback_ns.store(observation_time_ns(), std::memory_order_release);
      state.completed.fetch_add(1, std::memory_order_release);
    };
  }

  frame_delay_setting apply_max_frame_delay(VTCompressionSessionRef session, int requested) {
    frame_delay_setting result;
    result.requested = requested;
    // Production inherit does not query or write the property.
    if (requested < 0 && !observation) {
      return result;
    }
    CFDictionaryRef supported = nullptr;
    result.supported_status = VTSessionCopySupportedPropertyDictionary(session, &supported);
    if (supported) {
      const auto description = CFDictionaryGetValue(supported, kVTCompressionPropertyKey_MaxFrameDelayCount);
      if (description) {
        CFStringRef text = CFCopyDescription(description);
        if (text) {
          const CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
          std::string utf8(capacity, '\0');
          if (CFStringGetCString(text, utf8.data(), capacity, kCFStringEncodingUTF8)) {
            utf8.resize(std::char_traits<char>::length(utf8.c_str()));
            result.supported_description = std::move(utf8);
          }
          CFRelease(text);
        }
      }
      CFRelease(supported);
    }
    if (requested >= 0) {
      CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &requested);
      if (value) {
        // Record the real setter status even if the support dictionary omits the key.
        result.setter_status = VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, value);
        CFRelease(value);
        if (*result.setter_status != noErr) {
          BOOST_LOG(warning) << "VideoToolbox rejected MaxFrameDelayCount=" << requested
                             << " with OSStatus " << *result.setter_status << " before prepare";
        } else {
          BOOST_LOG(info) << "VideoToolbox accepted MaxFrameDelayCount=" << requested << " before prepare";
        }
      } else {
        BOOST_LOG(warning) << "Cannot allocate VideoToolbox MaxFrameDelayCount value";
      }
    }
    return result;
  }

  void set_session_observer(session_observer observer, void *opaque) {
    session_created = observer;
    session_observer_opaque = opaque;
  }

  void observe_session(VTCompressionSessionRef session) {
    if (session_created) {
      session_created(session, session_observer_opaque);
    }
  }

  CFDictionaryRef copy_encoder_specification(CFDictionaryRef specification, bool automatic) {
    if (!automatic || !specification) {
      return nullptr;
    }
    auto copy = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, specification);
    if (!copy) {
      BOOST_LOG(warning) << "Cannot copy VideoToolbox encoder specification; inheriting selection";
      return nullptr;
    }
    CFDictionaryRemoveValue(copy, CFSTR("EnableLowLatencyRateControl"));
    return copy;
  }

  void apply_encoder_options(AVCodecContext *context, AVDictionary **options, int speed, int power, int coder) {
    if (!context || !context->codec || !context->codec->name ||
        !std::string_view(context->codec->name).ends_with("_videotoolbox")) {
      return;
    }
    const auto apply = [&](const char *name, const char *requested) {
      if (!context->priv_data || !av_opt_find(context->priv_data, name, nullptr, 0, 0)) {
        BOOST_LOG(warning) << "VideoToolbox FFmpeg option " << name << " unsupported by linked encoder; inheriting default";
        return;
      }
      const int status = av_dict_set(options, name, requested, 0);
      if (status < 0) {
        BOOST_LOG(warning) << "Cannot request VideoToolbox FFmpeg option " << name << "=" << requested << ": " << status;
      } else {
        BOOST_LOG(info) << "Requested VideoToolbox FFmpeg option " << name << "=" << requested;
      }
    };
    apply("prio_speed", speed == 0 ? "0" : "1");
    if (power == 0 || power == 1) {
      apply("power_efficient", power == 0 ? "0" : "1");
    }
    if (context->codec_id == AV_CODEC_ID_H264 && (coder == 1 || coder == 2)) {
      apply("coder", coder == 1 ? "cabac" : "cavlc");
    }
  }

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
