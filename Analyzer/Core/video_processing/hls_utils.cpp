#include "hls_utils.h"

#include <filesystem>
#include <format>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace avs_analyzer {
int GuessH264LevelIdc(int width, int height, double fps) {
  if (fps <= 0.0) {
    fps = 25.0;
  }
  const int mb_width = (width + 15) / 16;
  const int mb_height = (height + 15) / 16;
  const int mb_per_frame = mb_width * mb_height;
  const double mbps = mb_per_frame * fps;

  struct LevelLimit {
    int level_idc;
    int max_mb_per_frame;
    double max_mbps;
  };

  const LevelLimit limits[] = {
      {30, 1620, 40500.0},
      {31, 3600, 108000.0},
      {32, 5120, 216000.0},
      {40, 8192, 245760.0},
      {41, 8192, 245760.0},
      {42, 8704, 522240.0},
      {50, 22080, 589824.0},
      {51, 36864, 983040.0},
  };

  for (const auto& lim : limits) {
    if (mb_per_frame <= lim.max_mb_per_frame && mbps <= lim.max_mbps) {
      return lim.level_idc;
    }
  }
  return 51;
}
bool RemuxHlsToSuggestedMp4(const Recorder::HlsPaths& paths) {
  const std::string playlist = paths.playlist;
  const std::string out_mp4 = paths.mp4_output;

  AVFormatContext* in_fmt = nullptr;
  int ret = avformat_open_input(&in_fmt, playlist.c_str(), nullptr, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG(std::format("RemuxHlsToSuggestedMp4: could not open playlist {}", playlist), ret);
    return false;
  }
  AVFormatContextPtr in_ctx{in_fmt};

  ret = avformat_find_stream_info(in_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("RemuxHlsToSuggestedMp4: could not find stream info", ret);
    return false;
  }

  AVFormatContext* out_fmt = nullptr;
  ret = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4", out_mp4.c_str());
  if (ret < 0 || !out_fmt) {
    LOGE_FFMPEG("RemuxHlsToSuggestedMp4: could not allocate output context", ret);
    return false;
  }
  AVFormatContextPtr out_ctx{out_fmt};

  std::vector<int> stream_map(in_ctx->nb_streams, -1);
  for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
    AVStream* in_stream = in_ctx->streams[i];
    AVStream* out_stream = avformat_new_stream(out_ctx.get(), nullptr);
    if (!out_stream) {
      LOGE("RemuxHlsToSuggestedMp4: could not create output stream");
      return false;
    }
    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
      LOGE_FFMPEG("RemuxHlsToSuggestedMp4: could not copy codec parameters", ret);
      return false;
    }
    out_stream->codecpar->codec_tag = 0;
    stream_map[i] = out_stream->index;
  }

  if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&out_ctx->pb, out_mp4.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      LOGE_FFMPEG(std::format("RemuxHlsToSuggestedMp4: could not open output file {}", out_mp4), ret);
      return false;
    }
  }

  ret = avformat_write_header(out_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("RemuxHlsToSuggestedMp4: could not write header", ret);
    return false;
  }

  AVPacketPtr pkt{av_packet_alloc()};
  while ((ret = av_read_frame(in_ctx.get(), pkt.get())) >= 0) {
    if (pkt->stream_index < 0 || (unsigned)pkt->stream_index >= stream_map.size()) {
      av_packet_unref(pkt.get());
      continue;
    }
    int out_index = stream_map[pkt->stream_index];
    if (out_index < 0) {
      av_packet_unref(pkt.get());
      continue;
    }
    AVStream* in_stream = in_ctx->streams[pkt->stream_index];
    AVStream* out_stream = out_ctx->streams[out_index];

    pkt->stream_index = out_index;
    av_packet_rescale_ts(pkt.get(), in_stream->time_base, out_stream->time_base);
    pkt->pos = -1;

    ret = av_interleaved_write_frame(out_ctx.get(), pkt.get());
    if (ret < 0) {
      LOGE_FFMPEG("RemuxHlsToSuggestedMp4: error writing packet", ret);
      av_packet_unref(pkt.get());
      return false;
    }
    av_packet_unref(pkt.get());
  }

  av_write_trailer(out_ctx.get());
  return true;
}

bool RemoveHlsFiles(const Recorder::HlsPaths& paths) {
  bool ok = true;
  std::filesystem::path playlist{paths.playlist};
  const auto dir = playlist.parent_path();

  // Remove playlist
  std::error_code ec;
  if (std::filesystem::exists(playlist, ec)) {
    if (!std::filesystem::remove(playlist, ec) || ec) {
      LOGE("RemoveHlsFiles: could not remove playlist {}", playlist.string());
      ok = false;
    }
  }

  // Remove init file (if any)
  if (!paths.init_filename.empty()) {
    std::filesystem::path init_path = dir / paths.init_filename;
    if (std::filesystem::exists(init_path, ec)) {
      if (!std::filesystem::remove(init_path, ec) || ec) {
        LOGE("RemoveHlsFiles: could not remove init file {}", init_path.string());
        ok = false;
      }
    }
  }

  std::filesystem::path seg_pat{paths.segment_pattern};
  const std::string seg_fname = seg_pat.filename().string();
  auto pct_pos = seg_fname.find('%');
  std::string prefix;
  std::string ext;
  if (pct_pos != std::string::npos) {
    prefix = seg_fname.substr(0, pct_pos);
    ext = seg_pat.extension().string();
  } else {
    std::filesystem::path p = seg_pat;
    if (std::filesystem::exists(p, ec)) {
      if (!std::filesystem::remove(p, ec) || ec) {
        LOGE("RemoveHlsFiles: could not remove segment file {}", p.string());
        ok = false;
      }
    }
    return ok;
  }

  for (auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const auto fname = e.path().filename().string();
    if (fname.size() >= prefix.size() + ext.size() &&
        fname.rfind(ext) == fname.size() - ext.size() &&
        fname.rfind(prefix, 0) == 0) {
      std::error_code ec2;
      if (!std::filesystem::remove(e.path(), ec2) || ec2) {
        LOGE("RemoveHlsFiles: could not remove segment {}", e.path().string());
        ok = false;
      }
    }
  }
  // attempt to remove directory if empty
  std::error_code ec3;
  if (std::filesystem::is_empty(dir, ec3) && !ec3) {
    std::filesystem::remove(dir, ec3);
  }
  return ok;
}

bool ReencodeHlsToSuggestedMp4(const Recorder::HlsPaths& paths, const ReencodeConfig& cfg) {
  const std::string playlist = paths.playlist;
  const std::string out_mp4 = paths.mp4_output;

  AVFormatContext* in_fmt = nullptr;
  int ret = avformat_open_input(&in_fmt, playlist.c_str(), nullptr, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG(std::format("ReencodeHlsToSuggestedMp4: could not open playlist {}", playlist), ret);
    return false;
  }
  AVFormatContextPtr in_ctx{in_fmt};

  ret = avformat_find_stream_info(in_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not find stream info", ret);
    return false;
  }

  const int video_index = av_find_best_stream(in_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0) {
    LOGE("ReencodeHlsToSuggestedMp4: no video stream found");
    return false;
  }

  AVStream* in_stream = in_ctx->streams[video_index];
  const AVCodec* decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
  if (!decoder) {
    LOGE("ReencodeHlsToSuggestedMp4: could not find decoder");
    return false;
  }

  AVCodecContextPtr dec_ctx{avcodec_alloc_context3(decoder)};
  if (!dec_ctx) {
    LOGE("ReencodeHlsToSuggestedMp4: could not allocate decoder context");
    return false;
  }
  ret = avcodec_parameters_to_context(dec_ctx.get(), in_stream->codecpar);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not copy decoder parameters", ret);
    return false;
  }
  ret = avcodec_open2(dec_ctx.get(), decoder, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not open decoder", ret);
    return false;
  }

  const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
  if (!encoder) {
    LOGE("ReencodeHlsToSuggestedMp4: libx264 encoder not found");
    return false;
  }

  AVCodecContextPtr enc_ctx{avcodec_alloc_context3(encoder)};
  if (!enc_ctx) {
    LOGE("ReencodeHlsToSuggestedMp4: could not allocate encoder context");
    return false;
  }

  AVRational fps_q = in_stream->avg_frame_rate.num ? in_stream->avg_frame_rate : in_stream->r_frame_rate;
  if (fps_q.num == 0 || fps_q.den == 0) {
    fps_q = {25, 1};
  }
  const double fps = av_q2d(fps_q);

  enc_ctx->width = in_stream->codecpar->width;
  enc_ctx->height = in_stream->codecpar->height;
  enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  enc_ctx->time_base = av_inv_q(fps_q);
  enc_ctx->framerate = fps_q;
  enc_ctx->gop_size = cfg.gop_size;
  enc_ctx->max_b_frames = cfg.max_b_frames;

  enc_ctx->level = GuessH264LevelIdc(enc_ctx->width, enc_ctx->height, fps);

  av_opt_set(enc_ctx->priv_data, "preset", cfg.preset.c_str(), 0);
  av_opt_set_int(enc_ctx->priv_data, "crf", cfg.crf, 0);
  av_opt_set(enc_ctx->priv_data, "profile", cfg.profile.c_str(), 0);
  av_opt_set_int(enc_ctx->priv_data, "refs", cfg.refs, 0);
  av_opt_set_int(enc_ctx->priv_data, "keyint_min", cfg.keyint_min, 0);
  av_opt_set_int(enc_ctx->priv_data, "sc_threshold", cfg.sc_threshold, 0);

  if (!cfg.x264_params.empty()) {
    av_opt_set(enc_ctx->priv_data, "x264-params", cfg.x264_params.c_str(), 0);
  }

  enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  ret = avcodec_open2(enc_ctx.get(), encoder, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not open encoder", ret);
    return false;
  }

  AVFormatContext* out_fmt = nullptr;
  ret = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4", out_mp4.c_str());
  if (ret < 0 || !out_fmt) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not allocate output context", ret);
    return false;
  }
  AVFormatContextPtr out_ctx{out_fmt};

  AVStream* out_stream = avformat_new_stream(out_ctx.get(), nullptr);
  if (!out_stream) {
    LOGE("ReencodeHlsToSuggestedMp4: could not create output stream");
    return false;
  }
  out_stream->time_base = enc_ctx->time_base;

  ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx.get());
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not copy encoder parameters", ret);
    return false;
  }

  if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&out_ctx->pb, out_mp4.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      LOGE_FFMPEG(std::format("ReencodeHlsToSuggestedMp4: could not open output file {}", out_mp4), ret);
      return false;
    }
  }

  AVDictionaryPtr out_opts{};
  out_opts.set("movflags", "+faststart", 0);
  ret = avformat_write_header(out_ctx.get(), out_opts.addr());
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not write header", ret);
    return false;
  }

  SwsContextPtr sws_ctx{};
  AVFramePtr in_frame{av_frame_alloc()};
  AVFramePtr out_frame{av_frame_alloc()};
  if (!in_frame || !out_frame) {
    LOGE("ReencodeHlsToSuggestedMp4: could not allocate frames");
    return false;
  }

  out_frame->format = enc_ctx->pix_fmt;
  out_frame->width = enc_ctx->width;
  out_frame->height = enc_ctx->height;
  ret = av_frame_get_buffer(out_frame.get(), 32);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: could not allocate output frame buffer", ret);
    return false;
  }

  AVPacketPtr in_pkt{av_packet_alloc()};
  AVPacketPtr out_pkt{av_packet_alloc()};
  if (!in_pkt || !out_pkt) {
    LOGE("ReencodeHlsToSuggestedMp4: could not allocate packets");
    return false;
  }

  int64_t frame_index = 0;
  auto encode_and_write = [&](AVFrame* frame) -> bool {
    ret = avcodec_send_frame(enc_ctx.get(), frame);
    if (ret < 0) {
      LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: error sending frame to encoder", ret);
      return false;
    }
    while ((ret = avcodec_receive_packet(enc_ctx.get(), out_pkt.get())) >= 0) {
      av_packet_rescale_ts(out_pkt.get(), enc_ctx->time_base, out_stream->time_base);
      out_pkt->stream_index = out_stream->index;
      if ((ret = av_interleaved_write_frame(out_ctx.get(), out_pkt.get())) < 0) {
        LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: error writing packet", ret);
        return false;
      }
      av_packet_unref(out_pkt.get());
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: encoder receive error", ret);
      return false;
    }
    return true;
  };

  while ((ret = av_read_frame(in_ctx.get(), in_pkt.get())) >= 0) {
    if (in_pkt->stream_index != video_index) {
      av_packet_unref(in_pkt.get());
      continue;
    }

    ret = avcodec_send_packet(dec_ctx.get(), in_pkt.get());
    av_packet_unref(in_pkt.get());
    if (ret < 0) {
      LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: error sending packet to decoder", ret);
      return false;
    }

    while ((ret = avcodec_receive_frame(dec_ctx.get(), in_frame.get())) >= 0) {
      AVFrame* frame_to_encode = in_frame.get();

      if (in_frame->format != enc_ctx->pix_fmt ||
          in_frame->width != enc_ctx->width ||
          in_frame->height != enc_ctx->height) {
        sws_ctx.reset(sws_getCachedContext(
            sws_ctx.release(),
            in_frame->width, in_frame->height, static_cast<AVPixelFormat>(in_frame->format),
            enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws_ctx) {
          LOGE("ReencodeHlsToSuggestedMp4: failed to create swscale context");
          return false;
        }
        ret = av_frame_make_writable(out_frame.get());
        if (ret < 0) {
          LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: output frame not writable", ret);
          return false;
        }
        sws_scale(sws_ctx.get(), in_frame->data, in_frame->linesize, 0, in_frame->height,
                  out_frame->data, out_frame->linesize);
        frame_to_encode = out_frame.get();
      }

      int64_t pts = in_frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE) {
        pts = frame_index++;
      }
      frame_to_encode->pts = av_rescale_q(pts, in_stream->time_base, enc_ctx->time_base);

      if (!encode_and_write(frame_to_encode)) {
        return false;
      }
      av_frame_unref(in_frame.get());
    }

    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      if (ret < 0) {
        LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: decoder receive error", ret);
        return false;
      }
    }
  }

  // Flush decoder
  ret = avcodec_send_packet(dec_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: error flushing decoder", ret);
    return false;
  }
  while ((ret = avcodec_receive_frame(dec_ctx.get(), in_frame.get())) >= 0) {
    AVFrame* frame_to_encode = in_frame.get();
    if (in_frame->format != enc_ctx->pix_fmt ||
        in_frame->width != enc_ctx->width ||
        in_frame->height != enc_ctx->height) {
      sws_ctx.reset(sws_getCachedContext(
          sws_ctx.release(),
          in_frame->width, in_frame->height, static_cast<AVPixelFormat>(in_frame->format),
          enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
          SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (!sws_ctx) {
        LOGE("ReencodeHlsToSuggestedMp4: failed to create swscale context");
        return false;
      }
      ret = av_frame_make_writable(out_frame.get());
      if (ret < 0) {
        LOGE_FFMPEG("ReencodeHlsToSuggestedMp4: output frame not writable", ret);
        return false;
      }
      sws_scale(sws_ctx.get(), in_frame->data, in_frame->linesize, 0, in_frame->height,
                out_frame->data, out_frame->linesize);
      frame_to_encode = out_frame.get();
    }
    int64_t pts = in_frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
      pts = frame_index++;
    }
    frame_to_encode->pts = av_rescale_q(pts, in_stream->time_base, enc_ctx->time_base);
    if (!encode_and_write(frame_to_encode)) {
      return false;
    }
    av_frame_unref(in_frame.get());
  }

  // Flush encoder
  if (!encode_and_write(nullptr)) {
    return false;
  }

  av_write_trailer(out_ctx.get());
  return true;
}

}  // namespace avs_analyzer
