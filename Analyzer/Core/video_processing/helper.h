//
// Created by lg on 10/1/25.
//

#ifndef ANALYZER_HELPER_H
#define ANALYZER_HELPER_H


#include <memory>
#include <string>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
};
namespace avs_analyzer {
using std::unique_ptr;
struct AVFormatContextDeleter {
  void operator()(AVFormatContext* ptr) const {
    if (!ptr) return;
    if (ptr->iformat) {
      avformat_close_input(&ptr);
      return;
    }
    if (ptr->oformat) {
      if (ptr->pb && !(ptr->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ptr->pb);
      }
      avformat_free_context(ptr);
    }
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
void log_error(const std::string& message, int error_code);

}
#endif  // ANALYZER_HELPER_H
