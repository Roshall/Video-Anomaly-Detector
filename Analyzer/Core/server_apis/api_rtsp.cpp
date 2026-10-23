#include "server_apis/api_common.h"
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <format>
#include <future>
#include <string>
#include <threadpool/BS_thread_pool.hpp>
#include <utility>
#include <vector>

extern "C" {
  #include <libavformat/avformat.h>
  #include <libavutil/error.h>
}

#include "config.h"
#include "scheduler.h"
#include "Utils/log.h"
#include "server_apis/server_context.h"

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
  auto* ctx = static_cast<ServerContext*>(arg);
  if (!ctx || !ctx->scheduler || !ctx->base) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"server is shutting down"})");
    return;
  }
  const int timeout_ms = ctx->scheduler->GetConfig().rtsp_probe_timeout_ms;

  nlohmann::json root;
  if (!get_request_json(req, root)) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  if (!root.is_array()) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"request body must be an array"})");
    return;
  }

  if (root.empty()) {
    send_json(req, HTTP_OK, "[]");
    return;
  }

  std::vector<std::pair<std::string, std::string>> targets;
  targets.reserve(root.size());
  size_t idx = 0;
  for (const auto& item : root) {
    std::string name;
    std::string url;
    if (item.is_object()) {
      name = item.value("name", "");
      url = item.value("url", item.value("rtsp", item.value("rtsp_url", "")));
    }
    if (name.empty()) {
      name = fmt::format("stream-{}", idx);
    }
    targets.emplace_back(std::move(name), std::move(url));
    ++idx;
  }

  // Probing opens RTSP connections, so it must never run on the event loop.
  evhttp_request_own(req);
  avs_analyzer::g_control_pool.detach_task([base = ctx->base, req,
                              targets = std::move(targets), timeout_ms]() {
    LOGD("rtsp probe: probing {} urls", targets.size());
    // A small request-local pool keeps probes concurrent without occupying
    // every control thread (which could deadlock the offload).
    BS::thread_pool<0> probe_pool{
        std::min<size_t>(targets.size(), 4)};
    std::vector<std::future<std::pair<std::string, int>>> futures;
    futures.reserve(targets.size());
    for (const auto& [name, url] : targets) {
      futures.push_back(probe_pool.submit_task([name, url, timeout_ms]() {
        return std::make_pair(name, probe_rtsp_url(url, timeout_ms));
      }));
    }

    nlohmann::json result = nlohmann::json::array();
    for (auto& future : futures) {
      auto [name, code] = future.get();
      result.push_back({{"name", name}, {"code", code}});
    }
    ReplyJsonAsync(base, req, HTTP_OK, result.dump());
  });
}
