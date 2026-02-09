#include "server_apis/api_common.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

extern "C" {
  #include <libavformat/avformat.h>
  #include <libavutil/error.h>
  #include <libavutil/avutil.h>
}

#include "scheduler.h"
#include "config.h"
#include "Utils/fast_rm_files.h"
#include "Utils/log.h"
#include "video_processing/helper.h"

using namespace avs_analyzer;

namespace {
bool path_contains_dotdot(const std::filesystem::path& p) {
  for (const auto& part : p) {
    if (part == "..") {
      return true;
    }
  }
  return false;
}

uintmax_t dir_size_bytes(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    return 0;
  }

  uintmax_t total = 0;
  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_regular_file(ec)) {
      if (!ec) {
        total += it->file_size(ec);
      }
    }
    if (ec) {
      ec.clear();
    }
  }
  return total;
}

bool clip_video_segment(const std::filesystem::path& input_path,
                        const std::filesystem::path& output_path,
                        double start_s,
                        double end_s,
                        double& out_duration_s,
                        std::string& err) {
  out_duration_s = 0.0;
  if (start_s < 0.0 || end_s <= start_s) {
    err = "invalid time range";
    return false;
  }

  auto ffmpeg_err = [](int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, buf, sizeof(buf));
    return std::string(buf);
  };

  AVFormatContext* in_fmt = nullptr;
  int ret = avformat_open_input(&in_fmt, input_path.string().c_str(), nullptr, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG(std::format("clip: failed to open input {}", input_path.string()), ret);
    err = std::format("failed to open input: {}", ffmpeg_err(ret));
    return false;
  }
  avs_analyzer::AVFormatContextPtr in_ctx{in_fmt};

  ret = avformat_find_stream_info(in_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("clip: failed to find stream info", ret);
    err = std::format("failed to read stream info: {}", ffmpeg_err(ret));
    return false;
  }

  int ref_index = av_find_best_stream(in_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ref_index < 0) {
    ref_index = 0;
  }

  AVFormatContext* out_fmt = nullptr;
  ret = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4", output_path.string().c_str());
  if (ret < 0 || !out_fmt) {
    LOGE_FFMPEG("clip: failed to allocate output context", ret);
    err = "failed to allocate output context";
    return false;
  }
  avs_analyzer::AVFormatContextPtr out_ctx{out_fmt};

  std::vector<int> stream_map(in_ctx->nb_streams, -1);
  for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
    AVStream* in_stream = in_ctx->streams[i];
    AVStream* out_stream = avformat_new_stream(out_ctx.get(), nullptr);
    if (!out_stream) {
      err = "failed to create output stream";
      return false;
    }
    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
      LOGE_FFMPEG("clip: failed to copy codec parameters", ret);
      err = std::format("failed to copy codec parameters: {}", ffmpeg_err(ret));
      return false;
    }
    out_stream->codecpar->codec_tag = 0;
    out_stream->time_base = in_stream->time_base;
    stream_map[i] = out_stream->index;
  }

  if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&out_ctx->pb, output_path.string().c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      LOGE_FFMPEG("clip: failed to open output file", ret);
      err = std::format("failed to open output file: {}", ffmpeg_err(ret));
      return false;
    }
  }

  ret = avformat_write_header(out_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("clip: failed to write header", ret);
    err = std::format("failed to write header: {}", ffmpeg_err(ret));
    return false;
  }

  const int64_t base_ts = (in_ctx->start_time != AV_NOPTS_VALUE) ? in_ctx->start_time : 0;
  const int64_t seek_ts = base_ts + static_cast<int64_t>(start_s * AV_TIME_BASE);
  ret = av_seek_frame(in_ctx.get(), -1, seek_ts, AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    LOGE_FFMPEG("clip: seek failed", ret);
    err = std::format("seek failed: {}", ffmpeg_err(ret));
    return false;
  }
  avformat_flush(in_ctx.get());

  std::vector<int64_t> first_ts(in_ctx->nb_streams, AV_NOPTS_VALUE);
  int64_t first_ref_pts = AV_NOPTS_VALUE;
  int64_t last_ref_pts = AV_NOPTS_VALUE;
  bool wrote_any = false;

  avs_analyzer::AVPacketPtr pkt{av_packet_alloc()};
  while ((ret = av_read_frame(in_ctx.get(), pkt.get())) >= 0) {
    const int idx = pkt->stream_index;
    if (idx < 0 || idx >= static_cast<int>(stream_map.size())) {
      av_packet_unref(pkt.get());
      continue;
    }
    const int out_index = stream_map[idx];
    if (out_index < 0) {
      av_packet_unref(pkt.get());
      continue;
    }

    AVStream* in_stream = in_ctx->streams[idx];
    const int64_t pkt_ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
    double pkt_time_s = -1.0;
    if (pkt_ts != AV_NOPTS_VALUE) {
      const int64_t pkt_ts_av = av_rescale_q(pkt_ts, in_stream->time_base, AV_TIME_BASE_Q);
      pkt_time_s = (pkt_ts_av - base_ts) / static_cast<double>(AV_TIME_BASE);
    }

    if (idx == ref_index && pkt_time_s >= 0.0 && pkt_time_s >= end_s) {
      av_packet_unref(pkt.get());
      break;
    }
    if (pkt_time_s >= 0.0 && pkt_time_s < start_s) {
      av_packet_unref(pkt.get());
      continue;
    }

    if (first_ts[idx] == AV_NOPTS_VALUE) {
      if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE) {
        first_ts[idx] = std::min(pkt->pts, pkt->dts);
      } else if (pkt->pts != AV_NOPTS_VALUE) {
        first_ts[idx] = pkt->pts;
      } else if (pkt->dts != AV_NOPTS_VALUE) {
        first_ts[idx] = pkt->dts;
      }
    }

    if (idx == ref_index) {
      if (pkt->pts != AV_NOPTS_VALUE) {
        if (first_ref_pts == AV_NOPTS_VALUE) {
          first_ref_pts = pkt->pts;
        }
        last_ref_pts = pkt->pts;
      } else if (pkt->dts != AV_NOPTS_VALUE) {
        if (first_ref_pts == AV_NOPTS_VALUE) {
          first_ref_pts = pkt->dts;
        }
        last_ref_pts = pkt->dts;
      }
    }

    if (pkt->pts != AV_NOPTS_VALUE && first_ts[idx] != AV_NOPTS_VALUE) {
      pkt->pts -= first_ts[idx];
      if (pkt->pts < 0) {
        pkt->pts = 0;
      }
    }
    if (pkt->dts != AV_NOPTS_VALUE && first_ts[idx] != AV_NOPTS_VALUE) {
      pkt->dts -= first_ts[idx];
      if (pkt->dts < 0) {
        pkt->dts = 0;
      }
    }

    AVStream* out_stream = out_ctx->streams[out_index];
    pkt->stream_index = out_index;
    if (pkt->pts != AV_NOPTS_VALUE) {
      pkt->pts = av_rescale_q(pkt->pts, in_stream->time_base, out_stream->time_base);
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
      pkt->dts = av_rescale_q(pkt->dts, in_stream->time_base, out_stream->time_base);
    }
    if (pkt->duration > 0) {
      pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
    }
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->dts > pkt->pts) {
      pkt->dts = pkt->pts;
    }
    pkt->pos = -1;

    ret = av_interleaved_write_frame(out_ctx.get(), pkt.get());
    if (ret < 0) {
      LOGE_FFMPEG("clip: write frame failed", ret);
      err = std::format("write frame failed: {}", ffmpeg_err(ret));
      av_packet_unref(pkt.get());
      return false;
    }
    wrote_any = true;
    av_packet_unref(pkt.get());
  }

  if (!wrote_any) {
    err = "no packets written";
    return false;
  }

  av_write_trailer(out_ctx.get());

  if (ref_index >= 0 && ref_index < static_cast<int>(in_ctx->nb_streams) &&
      first_ref_pts != AV_NOPTS_VALUE && last_ref_pts != AV_NOPTS_VALUE &&
      last_ref_pts >= first_ref_pts) {
    const AVStream* ref_stream = in_ctx->streams[ref_index];
    out_duration_s = (last_ref_pts - first_ref_pts) * av_q2d(ref_stream->time_base);
  } else {
    out_duration_s = end_s - start_s;
  }

  if (out_duration_s < 0.0) {
    out_duration_s = 0.0;
  }
  if (out_duration_s > (end_s - start_s)) {
    out_duration_s = (end_s - start_s);
  }
  return true;
}
}

void ApiCleanupFiles(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
  if (!scheduler) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"scheduler not available"})");
    return;
  }

  const std::string upload_dir = scheduler->GetConfig().upload_dir;
  if (upload_dir.empty()) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"upload_dir is empty"})");
    return;
  }

  nlohmann::json root;
  if (!get_request_json(req, root)) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  if (!root.is_array()) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"request body must be an array"})");
    return;
  }

  std::vector<std::string> full_paths;
  full_paths.reserve(root.size());
  nlohmann::json validation_errors = nlohmann::json::array();

  const std::filesystem::path upload_root(upload_dir);
  for (const auto& item : root) {
    if (!item.is_string()) {
      validation_errors.push_back({{"path", ""}, {"message", "path must be string"}});
      continue;
    }

    const std::string rel = item.get<std::string>();
    if (rel.empty()) {
      validation_errors.push_back({{"path", rel}, {"message", "path is empty"}});
      continue;
    }

    std::filesystem::path rel_path(rel);
    if (rel_path.is_absolute()) {
      validation_errors.push_back({{"path", rel}, {"message", "absolute path rejected"}});
      continue;
    }

    rel_path = rel_path.lexically_normal();
    if (path_contains_dotdot(rel_path)) {
      validation_errors.push_back({{"path", rel}, {"message", "path contains .."}});
      continue;
    }

    const std::filesystem::path full = upload_root / rel_path;
    full_paths.push_back(full.string());
  }

  fast_remove_files::Options opt;
  opt.stop_at = upload_dir;
  opt.allow_dotdot = false;
  opt.collect_errors = true;

  const auto rep = fast_remove_files::remove_files_and_empty_parents_pmr(full_paths, opt);

  nlohmann::json result{
      {"filesRequested", rep.files_requested},
      {"filesRemoved", rep.files_removed},
      {"filesMissingOk", rep.files_missing_ok},
      {"dirsRemoved", rep.dirs_removed},
      {"unlinkFailed", rep.unlink_failed},
      {"rmdirFailed", rep.rmdir_failed},
      {"skippedPaths", rep.skipped_paths}};

  nlohmann::json errors = nlohmann::json::array();
  for (const auto& e : rep.errors) {
    if (!e.message.empty()) {
      errors.push_back({{"path", e.path}, {"message", e.message}});
    }
  }
  for (const auto& e : validation_errors) {
    errors.push_back(e);
  }
  result["errors"] = errors;

  send_json(req, HTTP_OK, result.dump());
}

void ApiStorageUsage(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
  if (!scheduler) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"scheduler not available"})");
    return;
  }

  const std::string upload_dir = scheduler->GetConfig().upload_dir;
  if (upload_dir.empty()) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"upload_dir is empty"})");
    return;
  }

  const std::filesystem::path upload_path(upload_dir);
  std::error_code ec;
  const auto space_info = std::filesystem::space(upload_path, ec);
  if (ec) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"failed to read disk space"})");
    return;
  }

  const uintmax_t total_bytes = space_info.capacity;
  const uintmax_t available_bytes = space_info.available;
  const uintmax_t alarm_bytes = dir_size_bytes(upload_path / "alarms");
  const uintmax_t clips_bytes = dir_size_bytes(upload_path / "clips");

  nlohmann::json result{
      {"total_bytes", total_bytes},
      {"available_bytes", available_bytes},
      {"alarm_bytes", alarm_bytes},
      {"clips_bytes", clips_bytes}};
  send_json(req, HTTP_OK, result.dump());
}

void ApiClip(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
  if (!scheduler) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"error":"scheduler not available"})");
    return;
  }

  const std::string upload_dir = scheduler->GetConfig().upload_dir;
  if (upload_dir.empty()) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"error":"upload_dir is empty"})");
    return;
  }

  nlohmann::json root;
  if (!get_request_json(req, root)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"invalid request parameter"})");
    return;
  }

  if (!root.is_object()) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"request body must be an object"})");
    return;
  }

  const std::string rel = root.value("video_path", "");
  if (rel.empty()) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"missing video_path"})");
    return;
  }

  auto parse_double = [](const nlohmann::json& v, double& out) -> bool {
    if (v.is_number()) {
      out = v.get<double>();
      return true;
    }
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      try {
        size_t idx = 0;
        out = std::stod(s, &idx);
        return idx == s.size();
      } catch (...) {
        return false;
      }
    }
    return false;
  };

  double start_offset = 0.0;
  double end_offset = 0.0;
  if (!root.contains("start_offset") || !parse_double(root["start_offset"], start_offset)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"invalid start_offset"})");
    return;
  }
  if (!root.contains("end_offset") || !parse_double(root["end_offset"], end_offset)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"invalid end_offset"})");
    return;
  }

  if (root.contains("max_duration")) {
    double max_duration = 0.0;
    if (!parse_double(root["max_duration"], max_duration) || max_duration <= 0.0) {
      send_json(req, HTTP_BADREQUEST, R"({"error":"invalid max_duration"})");
      return;
    }
    if ((end_offset - start_offset) > max_duration) {
      send_json(req, HTTP_BADREQUEST, R"({"error":"duration exceeds max_duration"})");
      return;
    }
  }

  std::filesystem::path rel_path(rel);
  if (rel_path.is_absolute()) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"absolute path rejected"})");
    return;
  }

  rel_path = rel_path.lexically_normal();
  if (path_contains_dotdot(rel_path)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"path contains .."})");
    return;
  }

  const std::filesystem::path upload_root(upload_dir);
  const std::filesystem::path input_path = upload_root / rel_path;
  std::error_code ec;
  if (!std::filesystem::exists(input_path, ec) || ec) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"video path not found"})");
    return;
  }

  const std::filesystem::path clips_dir = upload_root / "clips";
  std::filesystem::create_directories(clips_dir, ec);
  if (ec) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"error":"failed to create clips directory"})");
    return;
  }

  const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  const int64_t start_ms = static_cast<int64_t>(start_offset * 1000.0);
  const int64_t end_ms = static_cast<int64_t>(end_offset * 1000.0);
  const std::string stem = input_path.stem().string().empty() ? "clip" : input_path.stem().string();
  const std::string name = std::format("{}_{}_{}_{}.mp4", stem, start_ms, end_ms, ts);
  const std::filesystem::path output_path = clips_dir / name;

  std::string err;
  double actual_duration = 0.0;
  LOGD("Clipping video segment: input='{}' output='{}' start={} end={}", 
       input_path.string(), output_path.string(), start_offset, end_offset);
  if (!clip_video_segment(input_path, output_path, start_offset, end_offset, actual_duration, err)) {
    send_json(req, HTTP_BADREQUEST, std::format(R"({{"error":"{}"}})", escape_json(err)));
    return;
  }

  nlohmann::json result;
  result["clip_path"] = (std::filesystem::path("clips") / name).string();
  result["duration"] = actual_duration;
  send_json(req, HTTP_OK, result.dump());
}
