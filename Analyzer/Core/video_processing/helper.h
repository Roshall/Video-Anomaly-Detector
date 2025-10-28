//
// Created by lg on 10/1/25.
//

#ifndef ANALYZER_HELPER_H
#define ANALYZER_HELPER_H


#include <memory>
#include <string>
#include <source_location>
#include "Utils/log.h"
extern "C" {
#include <libavfilter/avfilter.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace avs_analyzer {

inline void log_ffmpeg_error_impl(const std::source_location& loc, const std::string& message, int error_code) {
  char err_buf[AV_ERROR_MAX_STRING_SIZE];

  av_strerror(error_code, err_buf, sizeof(err_buf));
  LOGGER->log(to_spdlog_loc(loc), spdlog::level::err, "[FFMPEG] {}: {} ({})", message, err_buf, error_code);
}
#define LOGE_FFMPEG(message, error_code) \
    log_ffmpeg_error_impl(std::source_location::current(), message, error_code)

using std::unique_ptr;
struct AVFormatContextDeleter {
  void operator()(AVFormatContext* ptr) const {
    if (!ptr) return;
    if (ptr->iformat) {
      avformat_close_input(&ptr);
      return;
    }
    if (ptr->oformat && ptr->pb && !(ptr->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&ptr->pb);
    }
    avformat_free_context(ptr);
  }

};
struct AVCodecContextDeleter {
  void operator()(AVCodecContext* ptr) const { avcodec_free_context(&ptr); }
};
struct AVFrameDeleter {
  void operator()(AVFrame* ptr) const {
    av_frame_free(&ptr);
}
};

struct AVPacketDeleter {
  void operator()(AVPacket*ptr) const {
    av_packet_free(&ptr);
  }
};

struct SwsContextDeleter {
  void  operator()(SwsContext* sws_ctx) const {
    sws_freeContext(sws_ctx);
  }
};
struct AVFilterGraphDeleter {
  void operator()(AVFilterGraph* ptr) const {
    avfilter_graph_free(&ptr);
  }
};

struct AVFilterInOutDeleter {
  void operator()(AVFilterInOut* p) const {
    avfilter_inout_free(&p);
  }
};


class AVDictionaryPtr {
 public:
  AVDictionaryPtr() noexcept = default;
  explicit AVDictionaryPtr(AVDictionary* p) noexcept : p_(p) {}

  // non-copyable
  AVDictionaryPtr(const AVDictionaryPtr&) = delete;
  AVDictionaryPtr& operator=(const AVDictionaryPtr&) = delete;

  // movable
  AVDictionaryPtr(AVDictionaryPtr&& other) noexcept;
  AVDictionaryPtr& operator=(AVDictionaryPtr&& other) noexcept;

  ~AVDictionaryPtr() { av_dict_free(&p_); }

  // Accessors
  [[nodiscard]] AVDictionary* get() const noexcept { return p_; }
  AVDictionary** addr() noexcept { return &p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }

  // Replace owned pointer (frees the old one)
  void reset(AVDictionary* np) noexcept;

  // Release ownership to caller (won't free)
  AVDictionary* release() noexcept;

  // Clear and free
  void clear() noexcept { reset(nullptr); }

  // Swap
  void swap(AVDictionaryPtr& other) noexcept { std::swap(p_, other.p_); }

  // Convenience: set a key/value and keep unique ownership in sync
  // (FFmpeg may allocate or reallocate the dict inside av_dict_set)
  int set(const char* key, const char* val, int flags = 0);

 private:
  AVDictionary* p_{nullptr};
};
struct AVBufferDeleter {
  void operator()(uint8_t * ptr) const {
    av_free(ptr);
  }
};

using AVBufferPtr = std::unique_ptr<uint8_t, AVBufferDeleter>;
using AVFilterInOutPtr = unique_ptr<AVFilterInOut, AVFilterInOutDeleter>;
using AVFilterGraphPtr = unique_ptr<AVFilterGraph, AVFilterGraphDeleter>;
using AVFramePtr = unique_ptr<AVFrame, AVFrameDeleter>;
using AVFormatContextPtr = unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr = unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using AVPacketPtr = unique_ptr<AVPacket, AVPacketDeleter>;
using SwsContextPtr = unique_ptr<SwsContext, SwsContextDeleter>;

struct AVPacketWrapper {
  AVPacketPtr pkg_{av_packet_alloc()};
  void clear() const { av_packet_unref(pkg_.get());}
};

}
#endif  // ANALYZER_HELPER_H
