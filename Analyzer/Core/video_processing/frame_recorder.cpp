#include "frame_recorder.h"

#include <filesystem>
#include <format>
#include <iostream>
#include <cmath>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace avs_analyzer {

namespace {

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

  // Approximate H.264 level limits for common levels.
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
  return 51;  // fallback to a high level
}

}  // namespace

Recorder::Recorder(VideoConfig config)
    : config(config), reused_pkt_(av_packet_alloc()) {
  if (!reused_pkt_) {
    LOGE_FFMPEG("Failed to allocate packet", AVERROR(ENOMEM));
  }
}

Recorder::~Recorder() { CloseOutputSegment(); }

bool Recorder::Initialize(std::string_view file_path, OutputType type) {
  if (type == OutputType::kHls) {
    return Initialize(file_path, type, HlsConfig{});
  }

  // record last used mode
  last_output_type_ = type;
  last_hls_paths_.reset();
  last_hls_config_ = HlsConfig{};

  file_path_ = file_path;
  CodeConfig conf{};

  AVDictionaryPtr fmt_options{};
  switch (type) {
    case OutputType::kRtsp:
      fmt_options.set("rw_timeout", "30000000",
                      0);  // rtmp/http-flv connect timeout
      fmt_options.set("stimeout", "30000000", 0);  // ns
      fmt_options.set("rtsp_transport", "tcp", 0);
      conf = {1, 0, "25", "ultrafast", "rtsp", std::move(fmt_options)};
      break;
    case OutputType::kFile:
      fmt_options.set("movflags", "+faststart+use_metadata_tags", 0);
      conf.fmt_options = std::move(fmt_options);
      break;
    case OutputType::kHls:
      // handled above
      break;
  }

  return OpenOutputSegment(conf);  // Start the first segment
}

bool Recorder::Initialize(std::string_view file_path, OutputType type,
                          const HlsConfig& hls) {
  if (type != OutputType::kHls) {
    return Initialize(file_path, type);
  }

  // Derive canonical HLS paths and set them as recorder state
  const auto paths = MakeHlsPaths(file_path, hls);
  file_path_ = paths.playlist;
  last_output_type_ = OutputType::kHls;
  last_hls_config_ = hls;
  last_hls_paths_ = paths;

  CodeConfig conf{};
  conf.format_name = "hls";

  AVDictionaryPtr fmt_options{};
  const int segment_seconds = std::max(1, hls.segment_seconds);
  fmt_options.set("hls_time", std::to_string(segment_seconds).c_str(), 0);

  const int playlist_size = std::max(0, hls.playlist_size);
  fmt_options.set("hls_list_size", std::to_string(playlist_size).c_str(), 0);

  if (hls.segment_format == HlsSegmentFormat::kMpegts) {
    fmt_options.set("hls_segment_type", "mpegts", 0);
  } else {
    fmt_options.set("hls_segment_type", "fmp4", 0);
    if (!paths.init_filename.empty()) {
      fmt_options.set("hls_fmp4_init_filename", paths.init_filename.c_str(), 0);
    }
  }

  fmt_options.set("hls_segment_filename", paths.segment_pattern.c_str(), 0);

  std::string hls_flags = "independent_segments+program_date_time+temp_file+append_list+omit_endlist";
  if (hls.delete_segments && playlist_size > 0) {
    hls_flags += "+delete_segments";
  }
  fmt_options.set("hls_flags", hls_flags.c_str(), 0);

  conf.fmt_options = std::move(fmt_options);
  return OpenOutputSegment(conf);
}

Recorder::HlsPaths Recorder::MakeHlsPaths(std::string_view base) {
  return MakeHlsPaths(base, HlsConfig{});
}

Recorder::HlsPaths Recorder::MakeHlsPaths(std::string_view base,
                                          const HlsConfig& hls) {
  std::filesystem::path p{std::string(base)};
  // If user passed an explicit playlist path ending with .m3u8, use that;
  // otherwise append .m3u8
  std::filesystem::path playlist_path = p;
  if (playlist_path.extension() != ".m3u8") {
    playlist_path += ".m3u8";
  }

  const std::string stem = playlist_path.stem().string();
  const std::filesystem::path dir = playlist_path.parent_path();

  HlsPaths out;
  out.playlist = playlist_path.string();
  out.mp4_output = (dir / std::format("{}.mp4", stem)).string();

  if (hls.segment_format == HlsSegmentFormat::kMpegts) {
    out.segment_pattern = (dir / std::format("{}_%05d.ts", stem)).string();
    out.init_filename = "";  // no init file for mpegts
  } else {
    out.segment_pattern = (dir / std::format("{}_%05d.m4s", stem)).string();
    out.init_filename = std::format("{}_init.mp4", stem);
  }
  return out;
}

std::optional<std::string> Recorder::GetSuggestedMp4() const {
  // HLS: return derived mp4
  if (last_output_type_ == OutputType::kHls && last_hls_paths_) {
    return last_hls_paths_->mp4_output;
  }
  // File mode: normalize to .mp4
  if (last_output_type_ == OutputType::kFile && !file_path_.empty()) {
    std::filesystem::path p{file_path_};
    if (p.extension() == ".mp4") return p.string();
    p.replace_extension(".mp4");
    return p.string();
  }
  return std::nullopt;
}

bool Recorder::RemuxToSuggestedMp4() {
  if (last_output_type_ != OutputType::kHls || !last_hls_paths_) {
    LOGE("RemuxToSuggestedMp4: recorder is not initialized for HLS or no HLS paths available.");
    return false;
  }

  const std::string playlist = last_hls_paths_->playlist;
  auto out_mp4_opt = GetSuggestedMp4();
  if (!out_mp4_opt) {
    LOGE("RemuxToSuggestedMp4: cannot determine output mp4 path");
    return false;
  }
  const std::string out_mp4 = *out_mp4_opt;

  AVFormatContext* in_fmt = nullptr;
  int ret = avformat_open_input(&in_fmt, playlist.c_str(), nullptr, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG(std::format("RemuxToSuggestedMp4: could not open playlist {}", playlist), ret);
    return false;
  }
  AVFormatContextPtr in_ctx{in_fmt};

  ret = avformat_find_stream_info(in_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("RemuxToSuggestedMp4: could not find stream info", ret);
    return false;
  }

  AVFormatContext* out_fmt = nullptr;
  ret = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4", out_mp4.c_str());
  if (ret < 0 || !out_fmt) {
    LOGE_FFMPEG("RemuxToSuggestedMp4: could not allocate output context", ret);
    return false;
  }
  AVFormatContextPtr out_ctx{out_fmt};

  std::vector<int> stream_map(in_ctx->nb_streams, -1);
  for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
    AVStream* in_stream = in_ctx->streams[i];
    AVStream* out_stream = avformat_new_stream(out_ctx.get(), nullptr);
    if (!out_stream) {
      LOGE("RemuxToSuggestedMp4: could not create output stream");
      return false;
    }
    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
      LOGE_FFMPEG("RemuxToSuggestedMp4: could not copy codec parameters", ret);
      return false;
    }
    out_stream->codecpar->codec_tag = 0;
    stream_map[i] = out_stream->index;
  }

  if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&out_ctx->pb, out_mp4.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      LOGE_FFMPEG(std::format("RemuxToSuggestedMp4: could not open output file {}", out_mp4), ret);
      return false;
    }
  }

  ret = avformat_write_header(out_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("RemuxToSuggestedMp4: could not write header", ret);
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
      LOGE_FFMPEG("RemuxToSuggestedMp4: error writing packet", ret);
      av_packet_unref(pkt.get());
      return false;
    }
    av_packet_unref(pkt.get());
  }

  av_write_trailer(out_ctx.get());
  return true;
}

bool Recorder::ReencodeToSuggestedMp4(const ReencodeConfig& cfg) {
  if (last_output_type_ != OutputType::kHls || !last_hls_paths_) {
    LOGE("ReencodeToSuggestedMp4: recorder is not initialized for HLS or no HLS paths available.");
    return false;
  }

  const std::string playlist = last_hls_paths_->playlist;
  auto out_mp4_opt = GetSuggestedMp4();
  if (!out_mp4_opt) {
    LOGE("ReencodeToSuggestedMp4: cannot determine output mp4 path");
    return false;
  }
  const std::string out_mp4 = *out_mp4_opt;

  AVFormatContext* in_fmt = nullptr;
  int ret = avformat_open_input(&in_fmt, playlist.c_str(), nullptr, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG(std::format("ReencodeToSuggestedMp4: could not open playlist {}", playlist), ret);
    return false;
  }
  AVFormatContextPtr in_ctx{in_fmt};

  ret = avformat_find_stream_info(in_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not find stream info", ret);
    return false;
  }

  const int video_index = av_find_best_stream(in_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0) {
    LOGE("ReencodeToSuggestedMp4: no video stream found");
    return false;
  }

  AVStream* in_stream = in_ctx->streams[video_index];
  const AVCodec* decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
  if (!decoder) {
    LOGE("ReencodeToSuggestedMp4: could not find decoder");
    return false;
  }

  AVCodecContextPtr dec_ctx{avcodec_alloc_context3(decoder)};
  if (!dec_ctx) {
    LOGE("ReencodeToSuggestedMp4: could not allocate decoder context");
    return false;
  }
  ret = avcodec_parameters_to_context(dec_ctx.get(), in_stream->codecpar);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not copy decoder parameters", ret);
    return false;
  }
  ret = avcodec_open2(dec_ctx.get(), decoder, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not open decoder", ret);
    return false;
  }

  const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
  if (!encoder) {
    LOGE("ReencodeToSuggestedMp4: libx264 encoder not found");
    return false;
  }

  AVCodecContextPtr enc_ctx{avcodec_alloc_context3(encoder)};
  if (!enc_ctx) {
    LOGE("ReencodeToSuggestedMp4: could not allocate encoder context");
    return false;
  }

  // Determine FPS from input stream
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

  // Dynamic H.264 level based on input resolution/fps
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
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not open encoder", ret);
    return false;
  }

  AVFormatContext* out_fmt = nullptr;
  ret = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4", out_mp4.c_str());
  if (ret < 0 || !out_fmt) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not allocate output context", ret);
    return false;
  }
  AVFormatContextPtr out_ctx{out_fmt};

  AVStream* out_stream = avformat_new_stream(out_ctx.get(), nullptr);
  if (!out_stream) {
    LOGE("ReencodeToSuggestedMp4: could not create output stream");
    return false;
  }
  out_stream->time_base = enc_ctx->time_base;

  ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx.get());
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not copy encoder parameters", ret);
    return false;
  }

  if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&out_ctx->pb, out_mp4.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      LOGE_FFMPEG(std::format("ReencodeToSuggestedMp4: could not open output file {}", out_mp4), ret);
      return false;
    }
  }

  AVDictionaryPtr out_opts{};
  out_opts.set("movflags", "+faststart", 0);
  ret = avformat_write_header(out_ctx.get(), out_opts.addr());
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not write header", ret);
    return false;
  }

  SwsContextPtr sws_ctx{};
  AVFramePtr in_frame{av_frame_alloc()};
  AVFramePtr out_frame{av_frame_alloc()};
  if (!in_frame || !out_frame) {
    LOGE("ReencodeToSuggestedMp4: could not allocate frames");
    return false;
  }

  out_frame->format = enc_ctx->pix_fmt;
  out_frame->width = enc_ctx->width;
  out_frame->height = enc_ctx->height;
  ret = av_frame_get_buffer(out_frame.get(), 32);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: could not allocate output frame buffer", ret);
    return false;
  }

  AVPacketPtr in_pkt{av_packet_alloc()};
  AVPacketPtr out_pkt{av_packet_alloc()};
  if (!in_pkt || !out_pkt) {
    LOGE("ReencodeToSuggestedMp4: could not allocate packets");
    return false;
  }

  int64_t frame_index = 0;
  auto encode_and_write = [&](AVFrame* frame) -> bool {
    ret = avcodec_send_frame(enc_ctx.get(), frame);
    if (ret < 0) {
      LOGE_FFMPEG("ReencodeToSuggestedMp4: error sending frame to encoder", ret);
      return false;
    }
    while ((ret = avcodec_receive_packet(enc_ctx.get(), out_pkt.get())) >= 0) {
      av_packet_rescale_ts(out_pkt.get(), enc_ctx->time_base, out_stream->time_base);
      out_pkt->stream_index = out_stream->index;
      if ((ret = av_interleaved_write_frame(out_ctx.get(), out_pkt.get())) < 0) {
        LOGE_FFMPEG("ReencodeToSuggestedMp4: error writing packet", ret);
        return false;
      }
      av_packet_unref(out_pkt.get());
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      LOGE_FFMPEG("ReencodeToSuggestedMp4: encoder receive error", ret);
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
      LOGE_FFMPEG("ReencodeToSuggestedMp4: error sending packet to decoder", ret);
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
          LOGE("ReencodeToSuggestedMp4: failed to create swscale context");
          return false;
        }
        ret = av_frame_make_writable(out_frame.get());
        if (ret < 0) {
          LOGE_FFMPEG("ReencodeToSuggestedMp4: output frame not writable", ret);
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
        LOGE_FFMPEG("ReencodeToSuggestedMp4: decoder receive error", ret);
        return false;
      }
    }
  }

  // Flush decoder
  ret = avcodec_send_packet(dec_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("ReencodeToSuggestedMp4: error flushing decoder", ret);
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
        LOGE("ReencodeToSuggestedMp4: failed to create swscale context");
        return false;
      }
      ret = av_frame_make_writable(out_frame.get());
      if (ret < 0) {
        LOGE_FFMPEG("ReencodeToSuggestedMp4: output frame not writable", ret);
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

bool Recorder::OpenOutputSegment(const CodeConfig& cconfig) {
  // set up codec
  // 1. Find the H.265 encoder
  const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) {
    std::cerr << "Codec 'libx264' not found. Please ensure FFmpeg is compiled "
                 "with support for it."
              << std::endl;
    return false;
  }

  video_codec_ctx_.reset(avcodec_alloc_context3(codec));
  if (!video_codec_ctx_) {
    std::cerr << "Could not allocate video codec context" << std::endl;
    return false;
  }

  // 2. Set encoder parameters
  video_codec_ctx_->width = config.width;
  video_codec_ctx_->height = config.height;
  //  video_codec_ctx_->bit_rate = config.bitrate;
  video_codec_ctx_->time_base = {1, config.fps};
  video_codec_ctx_->framerate = {config.fps, 1};
  video_codec_ctx_->gop_size = config.fps * cconfig.gop_size_scale;
  video_codec_ctx_->max_b_frames = cconfig.max_b_frame;
  video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

  av_opt_set(video_codec_ctx_->priv_data, "crf", cconfig.crf.c_str(), 0);
  av_opt_set(video_codec_ctx_->priv_data, "preset", cconfig.preset.c_str(), 0);

  video_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  // 3. Open the codec
  int ret = avcodec_open2(video_codec_ctx_.get(), codec, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("Could not open codec", ret);
    return false;
  }
  // set up output stream
  AVFormatContext* fmt_ctx;
  ret = avformat_alloc_output_context2(&fmt_ctx, nullptr, cconfig.format_name,
                                       file_path_.c_str());
  if (ret < 0) {
    LOGE_FFMPEG("Could not create output context", ret);
    return false;
  }
  output_ctx_.reset(fmt_ctx);

  stream_ = avformat_new_stream(output_ctx_.get(), nullptr);
  if (!stream_) {
    std::cerr << "Failed to create new stream" << std::endl;
    return false;
  }
  stream_->id = output_ctx_->nb_streams - 1;
  stream_->time_base = video_codec_ctx_->time_base;

  ret = avcodec_parameters_from_context(stream_->codecpar,
                                        video_codec_ctx_.get());
  if (ret < 0) {
    LOGE_FFMPEG("Could not copy codec parameters", ret);
    return false;
  }

  if (!(output_ctx_->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&output_ctx_->pb, file_path_.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      LOGE_FFMPEG("Could not open output file", ret);
      return false;
    }
  }

  ret = avformat_write_header(output_ctx_.get(), cconfig.fmt_options.addr());

  if (ret < 0) {
    LOGE_FFMPEG("Could not write header", ret);
    return false;
  }
  initialized_ = true;
  return true;
}

void Recorder::CloseOutputSegment() {
  if (initialized_ && output_ctx_) {
    // Flush any remaining packets from the encoder
    EncodeAndWrite(nullptr);

    std::cout << "Closing file: " << file_path_ << std::endl;
    av_write_trailer(output_ctx_.get());
    output_ctx_.reset();
  }
}

bool Recorder::EncodeAndWrite(AVFrame* frame) {
  int ret = avcodec_send_frame(video_codec_ctx_.get(), frame);
  if (ret < 0) {
    LOGE_FFMPEG("Error sending frame to encoder", ret);
    return false;
  }

  bool get_frame{true};
  while (true) {
    ret = avcodec_receive_packet(video_codec_ctx_.get(), reused_pkt_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      get_frame = false;
      LOGE_FFMPEG("Error receiving packet from encoder", ret);
      break;
    }
    reused_pkt_->stream_index = stream_->index;
    av_packet_rescale_ts(reused_pkt_.get(), video_codec_ctx_->time_base,
                         stream_->time_base);
    if (ret = av_interleaved_write_frame(output_ctx_.get(), reused_pkt_.get());
        ret < 0) {
      get_frame = false;
      LOGE_FFMPEG("Error writing frame to output", ret);
      break;
    }
    av_packet_unref(reused_pkt_.get());
  }
  return get_frame;
}

bool Recorder::AddVideoFrame(AVFrame* frame) {
  // frame.time_base must set to input stream's time base
  auto pts = av_rescale_q(frame->best_effort_timestamp, frame->time_base,
                          video_codec_ctx_->time_base);
  if (pts <= last_pts_) {
    LOGD("recorder reconstruct frame's pts because of pts: {} <= {}", pts, last_pts_);
    pts = last_pts_ + 1;
  }
  last_pts_ = frame->pts = pts;
  return EncodeAndWrite(frame);
}
}  // namespace avs_analyzer