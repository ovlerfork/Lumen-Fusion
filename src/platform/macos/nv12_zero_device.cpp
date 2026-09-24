/**
 * @file src/platform/macos/nv12_zero_device.cpp
 * @brief Definitions for NV12 zero copy device on macOS.
 */
// standard includes
#include <utility>

// local includes
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/nv12_zero_device.h"
#include "src/video.h"

extern "C" {
#include "libavutil/imgutils.h"
}

namespace platf {

  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  void free_buffer(void *opaque, uint8_t *data) {
    CVPixelBufferRelease((CVPixelBufferRef) data);
  }

  util::safe_ptr<AVFrame, free_frame> av_frame;

  int nv12_zero_device::convert(platf::img_t &img) {
    auto *av_img = (av_img_t *) &img;

    if (!av_img->pixel_buffer || !av_img->pixel_buffer->buf) {
      return -1;  // No valid pixel buffer — caller should skip this frame
    }

    // Attach an AVBufferRef to this frame which will retain ownership of the CVPixelBuffer
    // until it is replaced or the frame is freed with av_frame_free().
    //
    // The presence of the AVBufferRef allows FFmpeg to simply add a reference to the buffer
    // rather than having to perform a deep copy of the data buffers in avcodec_send_frame().
    auto pixel_buffer = CVPixelBufferRetain(av_img->pixel_buffer->buf);
    auto buffer = av_buffer_create((uint8_t *) pixel_buffer, 0, free_buffer, nullptr, 0);
    if (!buffer) {
      CVPixelBufferRelease(pixel_buffer);
      return -1;
    }

    // Replace the previous buffer only after ownership of the new one is secured.
    av_buffer_unref(&av_frame->buf[0]);
    av_frame->buf[0] = buffer;

    // Place a CVPixelBufferRef at data[3] as required by AV_PIX_FMT_VIDEOTOOLBOX
    av_frame->data[3] = (uint8_t *) av_img->pixel_buffer->buf;

    return 0;
  }

  int nv12_zero_device::set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
    this->frame = frame;

    av_frame.reset(frame);

    resolution_fn(this->display, frame->width, frame->height);

    return 0;
  }

  int nv12_zero_device::init(void *display, pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn) {
    pixel_format_fn(display, pix_fmt == pix_fmt_e::nv12 ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);

    this->display = display;
    this->resolution_fn = std::move(resolution_fn);

    // we never use this pointer, but its existence is checked/used
    // by the platform independent code
    data = this;

    return 0;
  }

}  // namespace platf
