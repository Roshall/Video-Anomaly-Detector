#include "frame_recorder.h"

#include <filesystem>
#include <format>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace avs_analyzer {

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