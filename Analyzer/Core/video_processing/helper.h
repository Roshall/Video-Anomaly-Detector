//
// Created by lg on 10/1/25.
//

#ifndef ANALYZER_HELPER_H
#define ANALYZER_HELPER_H
#include <memory>
#include <string>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
};
namespace avs_analyzer {
using std::unique_ptr;
struct AVFormatContextDeleter {
  void operator()(AVFormatContext* ptr) const {
    if (ptr) {
      // If it's an output format, pb might need separate closing.
      if (ptr->pb && !(ptr->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ptr->pb);
      }
      avformat_close_input(&ptr);
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

using AVFramePtr = unique_ptr<AVFrame, AVFrameDeleter>;
using AVFormatContextPtr = unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr = unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using AVPacketPtr = unique_ptr<AVPacket, AVPacketDeleter>;

struct AVPacketWrapper {
  AVPacketPtr pkg_{av_packet_alloc()};
  void clear() const { av_packet_unref(pkg_.get());}
};
void log_error(const std::string& message, int error_code);

}
#endif  // ANALYZER_HELPER_H
