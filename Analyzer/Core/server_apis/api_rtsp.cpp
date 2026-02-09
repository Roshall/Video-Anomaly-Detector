#include "server_apis/api_common.h"

#include <format>
#include <future>
#include <string>
#include <vector>

extern "C" {
  #include <libavformat/avformat.h>
  #include <libavutil/error.h>
}

#include "config.h"
#include "scheduler.h"
#include "Utils/log.h"

namespace {
int probe_rtsp_url(const std::string& url, int timeout_ms) {
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
}

void ApiProbeRtsp(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
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

  const size_t thread_count = avs_analyzer::g_th_pool.get_thread_count();
  const size_t tasks_running = avs_analyzer::g_th_pool.get_tasks_running();
  const size_t tasks_queued = avs_analyzer::g_th_pool.get_tasks_queued();
  const bool use_concurrent = (tasks_running < thread_count) && (tasks_queued < thread_count);

  nlohmann::json result = nlohmann::json::array();

  if (use_concurrent) {
    LOGD("rtsp probe: using concurrent mode for {} urls (pool: {}/{} threads, {} queued)",
         num_urls, tasks_running, thread_count, tasks_queued);

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

      futures.push_back(avs_analyzer::g_th_pool.submit_task([name, url, timeout_ms]() {
        const int code = probe_rtsp_url(url, timeout_ms);
        return std::make_pair(name, code);
      }));

      ++idx;
    }

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
