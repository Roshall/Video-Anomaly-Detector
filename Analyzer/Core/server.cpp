#include "server.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>

#include <format>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <numeric>
#include <sstream>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/avutil.h>
}

#include "Utils/base64.h"
#include "Utils/common.h"
#include "Utils/file_server_cb.h"
#include "Utils/fast_rm_files.h"
#include "Utils/log.h"
#include "video_processing/helper.h"
#include "alarm_sound.h"
#include "config.h"
#include "monitor.h"
#include "scheduler.h"
#include "task.h"

static int extract_port(const std::string& ip_address) {
  std::string port_str = ip_address.substr(ip_address.rfind(':') + 1);
  int port = 0;
  std::istringstream(port_str) >> port;
  return port;
}

using namespace avs_analyzer;

constexpr int kRecvBufMaxSize = 1024 * 8;

void ApiCleanupFiles(evhttp_request* req, void* arg);
void ApiStorageUsage(evhttp_request* req, void* arg);
void ApiClip(evhttp_request* req, void* arg);

std::thread HTTPServerStart(Scheduler& scheduler, event_base* event_base_p,
                            FileRouteOptions& file_route_opt) {
  int port = extract_port(scheduler.GetConfig().analyzer_host);
  return std::thread([&, event_base_p, port]() {
    LOGI("启动分析器服务：http://0.0.0.0:{}", port);
    evhttp* http = evhttp_new(event_base_p);
    evhttp_set_default_content_type(http, "text/html; charset=utf-8");

    evhttp_set_timeout(http, 0);
    // 设置路由
    evhttp_set_cb(http, "/", ApiIndex, nullptr);
    evhttp_set_cb(http, "/api/health", ApiHealth, nullptr);
    evhttp_set_cb(http, "/api/controls", ApiAllMonitors, &scheduler);
    evhttp_set_cb(http, "/api/control", ApiMonitor, &scheduler);
    evhttp_set_cb(http, "/api/control/add", ApiAddMonitor, &scheduler);
    evhttp_set_cb(http, "/api/control/cancel", ApiRemoveMonitor, &scheduler);
    evhttp_set_cb(http, "/api/probe_rtsp", ApiProbeRtsp, &scheduler);
    evhttp_set_cb(http, "/api/snapshot", ApiSnapShot, nullptr);
    evhttp_set_cb(http, "/api/cleanup_files", ApiCleanupFiles, &scheduler);
    evhttp_set_cb(http, "/api/storage_usage", ApiStorageUsage, &scheduler);
    evhttp_set_cb(http, "/api/clip", ApiClip, &scheduler);
    evhttp_set_cb(http, "/file", file_route_cb, &file_route_opt);
    evhttp_set_cb(http, "/alarm-sound", alarm_sound_cv, scheduler.GetAlarmController());

    evhttp_bind_socket(http, "0.0.0.0", port);
    event_base_dispatch(event_base_p);

    evhttp_free(http);
  });
}

static bool to_int(const std::string& s, int& out) {
  auto* begin = s.data(); auto* end = s.data() + s.size();
  auto rc = std::from_chars(begin, end, out);
  return rc.ec == std::errc() && rc.ptr == end;
}

static int get_int_any(const nlohmann::json& v, int def = 0) {
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_number_unsigned()) return static_cast<int>(v.get<unsigned int>());
  if (v.is_string()) {
    int x{};
    if (to_int(v.get<std::string>(), x)) return x;
  }
  if (v.is_number_float()) return static_cast<int>(v.get<double>());
  return def;
}

static std::string get_string(const nlohmann::json& v, const std::string& def = "") {
  return v.is_string() ? v.get<std::string>() : def;
}

static bool path_contains_dotdot(const std::filesystem::path& p) {
  for (const auto& part : p) {
    if (part == "..") return true;
  }
  return false;
}

static uintmax_t dir_size_bytes(const std::filesystem::path& root) {
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

// Clip segment and return the actual duration written (seconds) via out_duration_s.
// This uses FFmpeg stream copy (no re-encode) for efficiency.
static bool clip_video_segment(const std::filesystem::path& input_path,
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
  AVFormatContextPtr in_ctx{in_fmt};

  ret = avformat_find_stream_info(in_ctx.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("clip: failed to find stream info", ret);
    err = std::format("failed to read stream info: {}", ffmpeg_err(ret));
    return false;
  }

  int ref_index = av_find_best_stream(in_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ref_index < 0) ref_index = 0;

  AVFormatContext* out_fmt = nullptr;
  ret = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4", output_path.string().c_str());
  if (ret < 0 || !out_fmt) {
    LOGE_FFMPEG("clip: failed to allocate output context", ret);
    err = "failed to allocate output context";
    return false;
  }
  AVFormatContextPtr out_ctx{out_fmt};

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

  const int64_t seek_ts = static_cast<int64_t>(start_s * AV_TIME_BASE);
  ret = av_seek_frame(in_ctx.get(), -1, seek_ts, AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    LOGE_FFMPEG("clip: seek failed", ret);
    err = std::format("seek failed: {}", ffmpeg_err(ret));
    return false;
  }
  avformat_flush(in_ctx.get());

  std::vector<int64_t> first_pts(in_ctx->nb_streams, AV_NOPTS_VALUE);
  std::vector<int64_t> first_dts(in_ctx->nb_streams, AV_NOPTS_VALUE);
  int64_t first_ref_pts = AV_NOPTS_VALUE;
  int64_t last_ref_pts = AV_NOPTS_VALUE;
  bool wrote_any = false;

  AVPacketPtr pkt{av_packet_alloc()};
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
      pkt_time_s = pkt_ts * av_q2d(in_stream->time_base);
    }

    if (idx == ref_index && pkt_time_s >= 0.0 && pkt_time_s >= end_s) {
      av_packet_unref(pkt.get());
      break;
    }
    if (pkt_time_s >= 0.0 && pkt_time_s < start_s) {
      av_packet_unref(pkt.get());
      continue;
    }

    if (pkt->pts != AV_NOPTS_VALUE && first_pts[idx] == AV_NOPTS_VALUE) {
      first_pts[idx] = pkt->pts;
    }
    if (pkt->dts != AV_NOPTS_VALUE && first_dts[idx] == AV_NOPTS_VALUE) {
      first_dts[idx] = pkt->dts;
    }

    if (idx == ref_index) {
      if (pkt->pts != AV_NOPTS_VALUE) {
        if (first_ref_pts == AV_NOPTS_VALUE) first_ref_pts = pkt->pts;
        last_ref_pts = pkt->pts;
      } else if (pkt->dts != AV_NOPTS_VALUE) {
        if (first_ref_pts == AV_NOPTS_VALUE) first_ref_pts = pkt->dts;
        last_ref_pts = pkt->dts;
      }
    }

    if (pkt->pts != AV_NOPTS_VALUE && first_pts[idx] != AV_NOPTS_VALUE) {
      pkt->pts -= first_pts[idx];
      if (pkt->pts < 0) pkt->pts = 0;
    }
    if (pkt->dts != AV_NOPTS_VALUE && first_dts[idx] != AV_NOPTS_VALUE) {
      pkt->dts -= first_dts[idx];
      if (pkt->dts < 0) pkt->dts = 0;
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

  if (out_duration_s < 0.0) out_duration_s = 0.0;
  if (out_duration_s > (end_s - start_s)) out_duration_s = (end_s - start_s);
  return true;
}

static int probe_rtsp_url(const std::string& url, int timeout_ms) {
  if (url.empty()) {
    LOGD("rtsp probe skipped: empty url");
    return 1000;
  }

  AVFormatContext* fmt_ctx = nullptr;
  AVDictionary* opts = nullptr;

  const std::string timeout_us = std::to_string(static_cast<int64_t>(timeout_ms) * 1000);
  av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  av_dict_set(&opts, "stimeout", timeout_us.c_str(), 0);
  av_dict_set(&opts, "rw_timeout", timeout_us.c_str(), 0);

  const int ret = avformat_open_input(&fmt_ctx, url.c_str(), nullptr, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err_buf, sizeof(err_buf));
    LOGD("rtsp probe failed url={} err={} ({})", url, err_buf, ret);
    if (fmt_ctx) {
      avformat_close_input(&fmt_ctx);
    }
    return 1000;
  }

  avformat_close_input(&fmt_ctx);
  LOGD("rtsp probe success url={}", url);
  return 0;
}

static std::pair<std::vector<int>, std::vector<std::string>> parse_levels(const nlohmann::json& levelsNode, LevelMode& mode) {
  std::vector<int> thresholds;
  std::vector<std::string> levels;

  if (levelsNode.is_null() || !levelsNode.is_object()) return { thresholds, levels};

  // v2 format: { "mode": "seconds"|"people", "values": [3, 5, ...] }
  if (levelsNode.contains("values") && levelsNode["values"].is_array()) {
    const auto mode_str = get_string(levelsNode.value("mode", nlohmann::json{}), "");
    mode = (mode_str == "seconds" || mode_str == "duration")
               ? LevelMode::kDuration
               : LevelMode::kNumPeople;

    const auto& values = levelsNode["values"];
    thresholds.reserve(values.size());
    levels.reserve(values.size());

    for (const auto& v : values) {
      int threshold = get_int_any(v, 0);
      if (mode == LevelMode::kDuration) {
        threshold *= 1000;
        thresholds.push_back(threshold);
        levels.push_back(std::to_string(get_int_any(v, 0)) + "s");
      } else {
        thresholds.push_back(threshold);
        levels.push_back(std::to_string(threshold));
      }
    }

    std::vector<size_t> idx(thresholds.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
      return thresholds[a] < thresholds[b];
    });
    std::vector<int> t_sorted;
    std::vector<std::string> l_sorted;
    t_sorted.reserve(thresholds.size());
    l_sorted.reserve(levels.size());
    for (auto i : idx) {
      t_sorted.push_back(thresholds[i]);
      l_sorted.push_back(levels[i]);
    }
    return {t_sorted, l_sorted};
  }

  // Iterate through all levels
  std::vector<std::pair<int, std::string>> combined;
  for (auto it = levelsNode.begin(); it != levelsNode.end(); ++it) {
    const auto& name = it.key();
    const auto& lv = it.value();
    if (!lv.is_object()) continue;

    int threshold = get_int_any(lv.value("threshold", nlohmann::json{}), 0);
    if (get_string(lv.value("unit", nlohmann::json{}), "") == "seconds") {
      mode = LevelMode::kDuration;
      threshold *= 1000;
    } else {
      mode = LevelMode::kNumPeople;
    }
    combined.emplace_back(threshold, name);
  }


  // Sort by threshold
  std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;  // Ascending order by threshold
  });
  for (const auto& [threshold, unit] : combined) {
    thresholds.push_back(threshold);
    levels.push_back(unit);
  }

  return { thresholds, levels};
}
void send_json(evhttp_request* req, int code, const std::string& json) {
  evbuffer* out = evbuffer_new();
  if (!out) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, "No buffer");
    return;
  }
  auto parsed = nlohmann::json::parse(json, nullptr, false);
  if (parsed.is_object() && parsed.contains("mime")) {
    LOGI("sending http code: {}, json: {}", code, json);
  } else {
    LOGI("sending http code: {}, json with mime data", code);
  }
  evbuffer_add(out, json.data(), json.size());
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json; charset=utf-8");
  // (Optional) CORS
  evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
  evhttp_send_reply(req, code, nullptr, out);
  evbuffer_free(out);
}

std::string escape_json(const std::string& s) {
  std::ostringstream os;
  for (unsigned char c : s) {
    switch (c) {
      case '\"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\b': os << "\\b"; break;
      case '\f': os << "\\f"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (c < 0x20) { os << "\\u" << std::hex << std::uppercase << (int)c; }
        else { os << c; }
    }
  }
  return os.str();
}

bool get_request_json(const evhttp_request* req, nlohmann::json& root) {
  char buf[kRecvBufMaxSize];
  ParsePostRequest(req, buf);

  try {
    root = nlohmann::json::parse(buf);
    return true;
  } catch (...) {
    return false;
  }
}


void ApiSnapShot(evhttp_request* req, void*) {
  if (nlohmann::json json_req; get_request_json(req, json_req)) {
    const std::string path = json_req.value("videoPath", "");
    if (path.empty()) {
      send_json(req, HTTP_BADREQUEST, R"({"error":"missing videoPath"})");
      return;
    }
    // Open video
    cv::VideoCapture cap;
    if (!cap.open(path)) {
      auto msg = std::format(R"({{"error":"failed to open video","path":"{}"}})", escape_json(path));
      send_json(req, HTTP_BADREQUEST, msg);
      return;
    }

    // Read first frame
    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
      auto msg = std::format(R"({{"error":"failed to read first frame","path":"{}" }})", escape_json(path));
      send_json(req, HTTP_BADREQUEST, msg);
      return;
    }

    // Encode to JPEG bytes
    std::vector<unsigned char> jpeg;
    if (std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        !cv::imencode(".jpg", frame, jpeg, params)) {
      send_json(req, HTTP_SERVUNAVAIL, R"({"error":"JPEG encode failed"})");
      return;
    }
    // Get codec (FOURCC) and resolution
    int fourcc = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
    char codec_c[5] = {0};
    codec_c[0] = static_cast<char>(fourcc & 0xFF);
    codec_c[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    codec_c[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    codec_c[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    std::string codec = codec_c;
    if (codec.empty() || codec.find_first_not_of("\0 ") == std::string::npos) {
      codec = "unknown";
    }

    // Get resolution
    std::string resolution = std::format("{}x{}", frame.cols, frame.rows);

    // Base64 encode (cpp-base64: base64_encode(unsigned char const*, unsigned int, bool url=false))
    const std::string b64 = base64_encode(jpeg.data(),
                                          static_cast<unsigned int>(jpeg.size()),
                                          false /*url*/);

    // JSON response
    nlohmann::json json;
    json["mime"] = "image/jpeg";
    json["data"] = b64;
    json["codec"] = codec;
    json["resolution"] = resolution;

    send_json(req, HTTP_OK, json.dump());
    LOGI("receive request for snapshot: {}. Successfully sent.", path);
  } else {
    LOGE("failed to parse json");
  }
}
void ApiIndex(evhttp_request* req, void*) {
  nlohmann::json result_urls;
  result_urls["/api"] = "this api version 1.0";
  result_urls["/api/health"] = "check health";
  result_urls["/api/controls"] = "get all control being analyzed";
  result_urls["/api/control"] = "get control being analyzed";
  result_urls["/api/control/add"] = "add control";
  result_urls["/api/control/cancel"] = "cancel control";
  result_urls["/api/largeModelCalcu"] = "largeModelCalcu";
  result_urls["/api/cleanup_files"] = "remove files under upload_dir";
  result_urls["/api/storage_usage"] = "storage usage for upload_dir";
  result_urls["/api/clip"] = "clip a video under upload_dir";

  nlohmann::json result;
  result["urls"] = result_urls;
  send_json(req, HTTP_OK, result.dump());
}
void ApiHealth(evhttp_request* req, void*) {
  int result_code = 0;
  std::string result_msg = "error";

  // 健康检测
  result_code = 1000;
  result_msg = "current service health";

  nlohmann::json result;
  result["msg"] = result_msg;
  result["code"] = result_code;
  send_json(req, HTTP_OK, result.dump());
}

void ApiProbeRtsp(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<Scheduler*>(arg);
  const int timeout_ms = scheduler ? scheduler->GetConfig().rtsp_probe_timeout_ms : 5000;

  nlohmann::json root;
  if (!get_request_json(req, root)) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  if (!root.is_array()) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"request body must be an array"})");
    return;
  }

  const size_t num_urls = root.size();
  if (num_urls == 0) {
    send_json(req, HTTP_OK, "[]");
    return;
  }

  // Check if thread pool has capacity for concurrent probing
  // Use concurrent mode only if:
  // 1. Thread pool has available threads
  // 2. Queue is not heavily loaded (tasks_queued < thread_count)
  const size_t thread_count = g_th_pool.get_thread_count();
  const size_t tasks_running = g_th_pool.get_tasks_running();
  const size_t tasks_queued = g_th_pool.get_tasks_queued();
  const bool use_concurrent = (tasks_running < thread_count) && (tasks_queued < thread_count);

  nlohmann::json result = nlohmann::json::array();

  if (use_concurrent) {
    LOGD("rtsp probe: using concurrent mode for {} urls (pool: {}/{} threads, {} queued)", 
         num_urls, tasks_running, thread_count, tasks_queued);

    // Launch all probes concurrently using thread pool
    std::vector<std::future<std::pair<std::string, int>>> futures;
    futures.reserve(num_urls);

    size_t idx = 0;
    for (const auto& item : root) {
      std::string name;
      std::string url;

      if (item.is_object()) {
        name = item.value("name", "");
        url = item.value("url", item.value("rtsp", item.value("rtsp_url", "")));
      }

      if (name.empty()) {
        name = std::format("stream-{}", idx);
      }

      // Submit probe task to thread pool
      futures.push_back(g_th_pool.submit_task([name, url, timeout_ms]() {
        const int code = probe_rtsp_url(url, timeout_ms);
        return std::make_pair(name, code);
      }));

      ++idx;
    }

    // Collect results in order
    for (auto& fut : futures) {
      auto [name, code] = fut.get();
      nlohmann::json entry;
      entry["name"] = name;
      entry["code"] = code;
      result.push_back(std::move(entry));
    }
  } else {
    LOGD("rtsp probe: using sequential mode for {} urls (pool busy: {}/{} threads, {} queued)", 
         num_urls, tasks_running, thread_count, tasks_queued);

    // Sequential probing when thread pool is busy
    size_t idx = 0;
    for (const auto& item : root) {
      std::string name;
      std::string url;

      if (item.is_object()) {
        name = item.value("name", "");
        url = item.value("url", item.value("rtsp", item.value("rtsp_url", "")));
      }

      if (name.empty()) {
        name = std::format("stream-{}", idx);
      }

      const int code = probe_rtsp_url(url, timeout_ms);

      nlohmann::json entry;
      entry["name"] = name;
      entry["code"] = code;
      result.push_back(std::move(entry));
      ++idx;
    }
  }

  send_json(req, HTTP_OK, result.dump());
}

void ApiCleanupFiles(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<Scheduler*>(arg);
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

  nlohmann::json result {
    {"filesRequested", rep.files_requested},
    {"filesRemoved", rep.files_removed},
    {"filesMissingOk", rep.files_missing_ok},
    {"dirsRemoved", rep.dirs_removed},
    {"unlinkFailed", rep.unlink_failed},
    {"rmdirFailed", rep.rmdir_failed},
    {"skippedPaths", rep.skipped_paths}

  };

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
  auto* scheduler = static_cast<Scheduler*>(arg);
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
    {"clips_bytes", clips_bytes}
  };
  send_json(req, HTTP_OK, result.dump());
}

void ApiClip(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<Scheduler*>(arg);
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

  double start_s = 0.0;
  double end_s = 0.0;
  if (!root.contains("start_s") || !parse_double(root["start_s"], start_s)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"invalid start_s"})");
    return;
  }
  if (!root.contains("end_s") || !parse_double(root["end_s"], end_s)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"invalid end_s"})");
    return;
  }

  if (root.contains("max_duration")) {
    double max_duration = 0.0;
    if (!parse_double(root["max_duration"], max_duration) || max_duration <= 0.0) {
      send_json(req, HTTP_BADREQUEST, R"({"error":"invalid max_duration"})");
      return;
    }
    if ((end_s - start_s) > max_duration) {
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
  const int64_t start_ms = static_cast<int64_t>(start_s * 1000.0);
  const int64_t end_ms = static_cast<int64_t>(end_s * 1000.0);
  const std::string stem = input_path.stem().string().empty() ? "clip" : input_path.stem().string();
  const std::string name = std::format("{}_{}_{}_{}.mp4", stem, start_ms, end_ms, ts);
  const std::filesystem::path output_path = clips_dir / name;

  std::string err;
  double actual_duration = 0.0;
  if (!clip_video_segment(input_path, output_path, start_s, end_s, actual_duration, err)) {
    send_json(req, HTTP_BADREQUEST, std::format(R"({{"error":"{}"}})", escape_json(err)));
    return;
  }

  nlohmann::json result;
  result["clip_path"] = (std::filesystem::path("clips") / name).string();
  result["duration"] = actual_duration;
  send_json(req, HTTP_OK, result.dump());
}
void ApiAllMonitors(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);

  int result_code = 0;
  std::string result_msg = "error";

  nlohmann::json result;
  if (auto monitors = scheduler->GetAllMonitors(); !monitors.empty()) {
    nlohmann::json result_data = nlohmann::json::array();
    int64_t curTimestamp = getCurTimestamp();
    for (auto& monitor : monitors) {
      const int64_t startTimestamp = monitor.start_timestamp;
      result_data.push_back({
          {"code", monitor.code},
          {"cameraId", monitor.video_code},
          {"streamUrl", monitor.stream_url},
          {"pushStream", monitor.push_stream},
          {"pushStreamUrl", monitor.push_stream_url},
          {"algorithmCode", toString(monitor.task)},
          {"recognitionRegion", monitor.roi_str},
          {"sourceType", monitor.source_type},
          {"startTimestamp", startTimestamp},
          {"liveMilliseconds", curTimestamp - startTimestamp}
      });
    }
    result["data"] = result_data;
    result_code = 1000;
    result_msg = "success";
  } else {
    result_msg = "the number of control exector is empty";
  }
  result["msg"] = result_msg;  result["code"] = result_code;

  send_json(req, HTTP_OK, result.dump());
}
void ApiMonitor(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);

  nlohmann::json result_monitor;
  int result_code = 0;
  std::string result_msg = "error";

  nlohmann::json root;
  if (get_request_json(req, root)) {
    if (!root.contains("code") || !root["code"].is_string() ||
        !root.contains("cameraId") || !root["cameraId"].is_string()) {
      result_msg = "invalid request parameter";
    } else {
      std::string code = root["code"].get<std::string>();
      std::string video_code = root["cameraId"].get<std::string>();
      if (scheduler->ContainsMonitor(video_code, code)) {
        result_monitor["code"] = code;
        result_code = 1000;
        result_msg = "success";

      } else {
        result_msg = "the monitor does not exist";
      }
    }

  } else {
    result_msg = "invalid request parameter";
  }

  nlohmann::json result;
  result["control"] = result_monitor;
  result["msg"] = result_msg;
  result["code"] = result_code;

  LOGI("\n \t request:{} \n \t response:{}", root.dump(), result.dump());
  send_json(req, HTTP_OK, result.dump());
}
void ApiAddMonitor(evhttp_request* req, void* arg) {
  const auto scheduler = static_cast<Scheduler*>(arg);

  int result_code = 0;
  std::string result_msg;

  nlohmann::json result;
  nlohmann::json root;
  if (get_request_json(req, root)) {

    // Required format:
    // { cameraId, sourceType, streamUrl, controls: [ {...}, {...} ] }
    if (!root.is_object() || !root.contains("controls") || !root["controls"].is_array()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
      return;
    }

    const std::string camera_id = root.value("cameraId", "");
    const std::string source_type = root.value("sourceType", "");
    const std::string stream_url = root.value("streamUrl", "");
    const auto& controls = root["controls"];

    std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>> monitors;
    std::vector<std::string> control_codes;
    monitors.reserve(controls.size());
    control_codes.reserve(controls.size());

    for (const auto& ctrl : controls) {
      try {
        int task = 0;
        if (ctrl.contains("algorithmCode")) {
          const auto& ac = ctrl["algorithmCode"];
          if (ac.is_string()) {
            task = std::stoi(ac.get<std::string>());
          } else if (ac.is_number_integer()) {
            task = ac.get<int>();
          } else {
            task = 0;
          }
        }
        if (task < 0 || task >= 6) {
          LOGE("wrong algorithm code: {}", task);
          continue;
        }

        const std::string ctrl_code = ctrl.value("code", "");
        control_codes.push_back(ctrl_code);

        LevelMode mode{};
        Monitor monitor{
            .code = ctrl_code,
            .video_code = camera_id,
            .source_type = source_type,
            .stream_url = stream_url,
          .push_stream = ctrl.value("pushStream", false),
          .push_stream_url = ctrl.value("pushStreamUrl", ""),
            .task = static_cast<Task>(task),
          .conf_threshold = ctrl.value("modelConfidenceThreshold", 0.5f),
          .roi_str = ctrl.value("recognitionRegion", ""),
          .levels = parse_levels(ctrl.contains("levels") ? ctrl["levels"] : nlohmann::json{}, mode),
            .level_mode = mode,
        };

        const int ws = ctrl.value("alertWindowSize", 1);
        const int first_threshold = monitor.levels.first.empty() ? 0 : monitor.levels.first.front();
        const int64_t duration = (mode == LevelMode::kDuration) ? first_threshold : 0;

        EventDetectorConfig event_cfg{
          .major_stride_ms = static_cast<int64_t>(ctrl.value("minInterval", 0.0f) * 1000),
          .minor_stride_ms = static_cast<int64_t>(ctrl.value("alertInterval", 0.0f) * 1000),
            .window_size = ws,
          .min_vote = std::max(static_cast<int>(ws * ctrl.value("alertFrameRatio", 100.0f) / 100), 1),
          .max_event_duration_ms = static_cast<int64_t>(ctrl.value("maxAlarmDuration", 0.0f) * 1000),
            .latest_duration_ms = duration,
            .minimum_num = (mode == LevelMode::kNumPeople) ? std::max(first_threshold, 1) : 1,
        };

        std::string validate_msg;
        if (!monitor.ValidateAdd(validate_msg)) {
          LOGE("invalid monitor in controls: {}", validate_msg);
          continue;
        }

        monitors.emplace_back(std::make_unique<Monitor>(std::move(monitor)), std::move(event_cfg));
      } catch (const std::exception& e) {
        LOGE("runtime error in controls add: {}", e.what());
      }
    }

    std::vector<std::pair<int, std::string>> add_results;
    scheduler->MonitorBatchAdd(std::move(monitors), add_results);

    if (source_type == "stream") {
      nlohmann::json arr = nlohmann::json::array();
      const size_t n = std::min(control_codes.size(), add_results.size());
      for (size_t i = 0; i < n; ++i) {
        arr.push_back({
            {"controlCode", control_codes[i]},
            {"code", add_results[i].first},
            {"msg", add_results[i].second}
        });
      }
      send_json(req, HTTP_OK, arr.dump());
      return;
    }

    // source_type == "file" : must be all successful or failed
    bool all_ok = !add_results.empty();
    std::string fail_msg;
    for (const auto& [rc, msg] : add_results) {
      if (rc != 1000) {
        all_ok = false;
        fail_msg = msg;
        break;
      }
    }
    nlohmann::json obj {
      {"cameraId", camera_id},
      {"code", all_ok ? 1000 : 0},
      {"msg", all_ok ? "added successfully" : (fail_msg.empty() ? "batch add failed" : fail_msg)}
    };
    send_json(req, HTTP_OK, obj.dump());
    return;
  } else {
    result_msg = "invalid request parameter";
    result["msg"] = result_msg;
    result["code"] = result_code;
  }

  LOGI("\n \t request:{} \n \t response:{}", root.dump(), result.dump());
  send_json(req, HTTP_OK, result.dump());
}

void ApiRemoveMonitor(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);

  nlohmann::json root;
  if (!get_request_json(req, root)) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  // Required format:
  // { "cameraId": "...", "sourceType": "stream", "codes": ["..."] }
  if (!root.is_object() || !root.contains("codes") || !root["codes"].is_array()) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  const std::string camera_id = root.value("cameraId", "");
  const std::string source_type = root.value("sourceType", "");
  const auto& codes = root["codes"];

  if (source_type == "file") {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"remove monitors for sourceType=file is not supported"})");
    return;
  }

  std::vector<std::pair<std::string, std::string>> monitor_ids;
  std::vector<std::string> control_codes;
  monitor_ids.reserve(codes.size());
  control_codes.reserve(codes.size());

  for (const auto& c : codes) {
    if (!c.is_string()) {
      continue;
    }
    const std::string code = c.get<std::string>();
    if (code.empty()) {
      continue;
    }
    control_codes.push_back(code);
    monitor_ids.emplace_back(camera_id, code);
  }

  std::vector<std::pair<int, std::string>> rm_results;
  scheduler->MonitorBatchRemove(monitor_ids, rm_results);

  nlohmann::json arr = nlohmann::json::array();
  const size_t n = std::min(control_codes.size(), rm_results.size());
  for (size_t i = 0; i < n; ++i) {
    arr.push_back({
        {"controlCode", control_codes[i]},
        {"code", rm_results[i].first},
        {"msg", rm_results[i].second}
    });
  }
  send_json(req, HTTP_OK, arr.dump());
}
void ParseGetRequest(const evhttp_request* req, evkeyvalq* params) {
  if (req == nullptr) {
    return;
  }
  const char* url = evhttp_request_get_uri(req);
  if (url == nullptr) {
    return;
  }
  const evhttp_uri* decoded = evhttp_uri_parse(url);
  if (!decoded) {
    return;
  }
  const char* query = const_cast<char*>(evhttp_uri_get_query(decoded));
  if (query != nullptr) {
    evhttp_parse_query_str(query, params);
  }
  evhttp_uri_free(const_cast<evhttp_uri*>(decoded));
}
void ParsePostRequest(const evhttp_request* req, char* buff) {
  size_t post_size = 0;
  post_size = evbuffer_get_length(req->input_buffer);
  if (post_size <= 0) {
    return;
  }
  const size_t copy_len =
      post_size >= (kRecvBufMaxSize - 1) ? (kRecvBufMaxSize - 1) : post_size;
  memcpy(buff, evbuffer_pullup(req->input_buffer, -1), copy_len);
  buff[copy_len] = '\0';
}
void alarm_sound_cv(evhttp_request* req, void* arg) {
  auto* alarm_ptr = static_cast<AlarmController*>(arg);
  if (!alarm_ptr) {
    send_json(req, HTTP_SERVUNAVAIL, R"({\"msg\":\"alarm sound is not configured\"})");
    return;
  }

  // Parse GET query
  evkeyvalq params{};
  ParseGetRequest(req, &params);
  const char* method = evhttp_find_header(&params, "method");
  if (!method) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"missing parameter 'method'"})");
  } else {
    // Dispatch
    const std::string m(method);
    if (m == "stop") {
      alarm_ptr->stop();
      evhttp_send_reply(req, HTTP_OK, "OK", nullptr);
      LOGD("alarm api: Alarm sound stopped.");
    } else if (m == "start") {
      alarm_ptr->trigger();
      evhttp_send_reply(req, HTTP_OK, "OK", nullptr);
      LOGD("alarm api: Alarm sound started.");
    } else {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"wrong method"})");
    }
  }
  evhttp_clear_headers(&params);
}