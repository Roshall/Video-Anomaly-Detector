#include "config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "Utils/log.h"
#include <format>
using namespace std::string_literals;

namespace avs_analyzer {
namespace {
template <typename T>
T json_get_or(const nlohmann::json& j, std::initializer_list<const char*> path,
              T def) {
  const nlohmann::json* cur = &j;
  for (const char* key : path) {
    if (!cur->is_object() || !cur->contains(key)) {
      return def;
    }
    cur = &(*cur)[key];
  }
  try {
    return cur->get<T>();
  } catch (...) {
    return def;
  }
}

std::string json_get_or_string(const nlohmann::json& j,
                               std::initializer_list<const char*> path,
                               std::string def = {}) {
  return json_get_or<std::string>(j, path, std::move(def));
}
}  // namespace

Config::Config(const char* file) {
  std::ifstream ifs(file, std::ios::binary);

  if (!ifs.is_open()) {
    LOGE("open {} error", file);
    return;
  }
  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const std::exception& e) {
    LOGE("Json parse error:{}", e.what());
    return;
  }

  try {
    // Prefer new hierarchical schema; fall back to legacy flat keys.
    const std::string admin_host_raw =
        json_get_or_string(root, {"server", "admin", "host"},
                           json_get_or_string(root, {"adminHost"}, ""));
    const std::string analyzer_host_raw =
        json_get_or_string(root, {"server", "analyzer", "host"},
                           json_get_or_string(root, {"analyzerHost"}, ""));
    const std::string media_http_raw =
        json_get_or_string(root, {"server", "media", "http"},
                           json_get_or_string(root, {"mediaHttpHost"}, ""));
    const std::string media_rtsp_raw =
        json_get_or_string(root, {"server", "media", "rtsp"},
                           json_get_or_string(root, {"mediaRtspHost"}, ""));

    if (!admin_host_raw.empty()) {
      admin_host = std::format("http://{}", admin_host_raw);
    }
    if (!analyzer_host_raw.empty()) {
      analyzer_host = std::format("http://{}", analyzer_host_raw);
    }
    if (!media_http_raw.empty()) {
      media_http_host = std::format("http://{}", media_http_raw);
    }
    if (!media_rtsp_raw.empty()) {
      // NOTE: kept as http:// for backward compatibility with existing code paths.
      media_rtsp_host = std::format("http://{}", media_rtsp_raw);
    }

    upload_dir = json_get_or_string(root, {"uploadDir"}, upload_dir);
    offline_video_dir = json_get_or_string(root, {"offlineVideoDir"}, offline_video_dir);
    model_dir = json_get_or_string(root, {"DLModel", "dir"},
                                   json_get_or_string(root, {"modelDir"}, model_dir));

    use_accelerator = json_get_or<bool>(
        root, {"DLModel", "acceleratorInference"},
        json_get_or<bool>(root, {"acceleratorInference"}, use_accelerator));
    log_level = json_get_or_string(root, {"application", "logLevel"}, log_level);
    thread_pool_size = json_get_or<int>(root, {"application", "threadPoolSize"}, thread_pool_size);
    file_cache_timeout = json_get_or<u_int32_t>(
        root, {"application", "fileCacheTimeout"}, file_cache_timeout);
    rtsp_probe_timeout_ms = json_get_or<int>(
      root, {"application", "rtspProbeTimeoutMs"}, rtsp_probe_timeout_ms);

    inference_workers_per_model = json_get_or<int>(
        root, {"performance", "inferenceWorkersPerModel"},
        inference_workers_per_model);
    inference_workers_per_model_accelerated = json_get_or<int>(
        root, {"performance", "inferenceWorkersPerModelAccelerated"},
        inference_workers_per_model_accelerated);
    inference_result_timeout_ms = json_get_or<int>(
        root, {"application", "inferenceResultTimeoutMs"},
        inference_result_timeout_ms);
    offline_pipeline_depth = json_get_or<int>(
        root, {"performance", "offlinePipelineDepth"}, offline_pipeline_depth);
    event_buffer_budget_mb = json_get_or<int>(
        root, {"performance", "eventBufferBudgetMb"}, event_buffer_budget_mb);
    push_queue_max_frames = json_get_or<int>(
        root, {"performance", "pushQueueMaxFrames"}, push_queue_max_frames);
    stream_reconnect_max_attempts = json_get_or<int>(
        root, {"performance", "streamReconnectMaxAttempts"},
        stream_reconnect_max_attempts);
    stream_reconnect_backoff_ms = json_get_or<int>(
        root, {"performance", "streamReconnectBackoffMs"},
        stream_reconnect_backoff_ms);
    stream_reconnect_backoff_max_ms = json_get_or<int>(
        root, {"performance", "streamReconnectBackoffMaxMs"},
        stream_reconnect_backoff_max_ms);
    if (root.contains("performance") && root["performance"].is_object()) {
      const auto& perf = root["performance"];
      if (perf.contains("preloadModels") && perf["preloadModels"].is_array()) {
        std::vector<std::string> models;
        for (const auto& entry : perf["preloadModels"]) {
          if (entry.is_string() && !entry.get<std::string>().empty()) {
            models.push_back(entry.get<std::string>());
          }
        }
        if (!models.empty()) {
          preload_models = std::move(models);
        }
      }
    }

    alarm_sound_path = json_get_or_string(
        root, {"alarm", "soundPath"}, alarm_sound_path);
    alarm_sound_cooldown_sec = json_get_or<int>(
        root, {"alarm", "soundCooldownSec"}, alarm_sound_cooldown_sec);

    font_path = json_get_or_string(root, {"display", "fontPath"}, font_path);
    legend_position = json_get_or_string(
        root, {"display", "legendPosition"}, legend_position);
    legend_prealloc_max_labels = json_get_or<int>(
      root, {"display", "legendPreallocMaxLabels"}, legend_prealloc_max_labels);
    legend_prealloc_max_label_len = json_get_or<int>(
      root, {"display", "legendPreallocMaxLabelLen"}, legend_prealloc_max_label_len);

    bunkoff_overlap_ratio = json_get_or<double>(
        root, {"analyzer", "bunkOffOverlapRatio"}, bunkoff_overlap_ratio);
    
    // Load HLS configuration
    if (root.contains("hls_config")) {
      const auto& hls_obj = root["hls_config"];
      if (hls_obj.contains("segment_seconds")) {
        hls_config.segment_seconds = hls_obj["segment_seconds"];
      }
      if (hls_obj.contains("playlist_size")) {
        hls_config.playlist_size = hls_obj["playlist_size"];
      }
      if (hls_obj.contains("delete_segments")) {
        hls_config.delete_segments = hls_obj["delete_segments"];
      }
      if (hls_obj.contains("segment_format")) {
        std::string fmt = hls_obj["segment_format"];
        if (fmt.ends_with("ts")) {
          hls_config.segment_format = Recorder::HlsSegmentFormat::kMpegts;
        } else {
          hls_config.segment_format = Recorder::HlsSegmentFormat::kFmp4;
        }
      }
    }
  } catch (const std::exception& e) {
    LOGE("Json parse error:{}", e.what());
    return;
  }

    // 0: use all core
    thread_pool_size = std::clamp(thread_pool_size, 0, static_cast<int>(std::thread::hardware_concurrency()));
    rtsp_probe_timeout_ms = std::clamp(rtsp_probe_timeout_ms, 1000, 30000);
    inference_workers_per_model = std::clamp(inference_workers_per_model, 1, 8);
    inference_workers_per_model_accelerated =
        std::clamp(inference_workers_per_model_accelerated, 1, 8);
    inference_result_timeout_ms = std::clamp(inference_result_timeout_ms, 100, 60000);
    offline_pipeline_depth = std::clamp(offline_pipeline_depth, 1, 16);
    event_buffer_budget_mb = std::clamp(event_buffer_budget_mb, 32, 4096);
    push_queue_max_frames = std::clamp(push_queue_max_frames, 10, 300);
    stream_reconnect_max_attempts =
        std::clamp(stream_reconnect_max_attempts, 1, 1000);
    stream_reconnect_backoff_ms =
        std::clamp(stream_reconnect_backoff_ms, 100, 60000);
    stream_reconnect_backoff_max_ms =
        std::clamp(stream_reconnect_backoff_max_ms,
                   stream_reconnect_backoff_ms, 300000);
    alarm_sound_cooldown_sec = std::max(0, alarm_sound_cooldown_sec);
    bunkoff_overlap_ratio = std::clamp(bunkoff_overlap_ratio, 0.0, 1.0);
    if (legend_prealloc_max_labels < 1 || legend_prealloc_max_labels > 6) {
      LOGE("display.legendPreallocMaxLabels={} is invalid; allowed range is [1, 6]",
           legend_prealloc_max_labels);
      return;
    }
    if (legend_prealloc_max_label_len < 1 || legend_prealloc_max_label_len > 10) {
      LOGE("display.legendPreallocMaxLabelLen={} is invalid; allowed range is [1, 10]",
           legend_prealloc_max_label_len);
      return;
    }

    std::filesystem::path path(upload_dir);
    try {
      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directory(path);
      }

      state = true;
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("could not find upload dir: {}",e.what());
    }
  ifs.close();
}

void Config::Show() {
  printf("Configuration Settings:\n"
         "----------------------\n"
         "State               : %s\n"
         "Thread Pool Size    : %d\n"
         "File Cache Timeout  : %u seconds\n"
         "Admin Host         : %s\n"
         "Analyzer Host      : %s\n"
         "Media HTTP Host    : %s\n"
         "Media RTSP Host    : %s\n"
         "Upload Directory   : %s\n"
         "offline Video Dir   : %s\n"
         "Model Directory    : %s\n"
         "Use Accelerator    : %s\n"
         "Log Level          : %s\n"
         "RTSP Probe Timeout : %d ms\n"
         "Inference Workers  : %d (accelerated %d)\n"
         "Inference Timeout  : %d ms\n"
         "Offline Depth      : %d\n"
         "Event Buffer Budget: %d MB\n"
         "Push Queue Max     : %d\n"
         "Reconnect Attempts : %d\n"
         "Reconnect Backoff  : %d ms (max %d ms)\n"
         "Alarm Sound Path   : %s\n"
         "Alarm Cooldown Sec : %d\n"
         "Font Path          : %s\n"
         "Legend Position    : %s\n"
         "Legend Max Labels  : %d\n"
         "Legend Max Length  : %d\n"
         "BunkOff Overlap    : %.3f\n"
         "HLS Segment Seconds: %d\n"
         "HLS Playlist Size  : %d\n"
         "HLS Delete Segments: %s\n"
         "HLS Segment Format : %s\n"
         "----------------------\n",
         state ? "true" : "false",
         thread_pool_size,
         file_cache_timeout,
         admin_host.c_str(),
         analyzer_host.c_str(),
         media_http_host.c_str(),
         media_rtsp_host.c_str(),
         upload_dir.data(),
         model_dir.data(),
         offline_video_dir.data(),
         use_accelerator ? "true" : "false",
         log_level.c_str(),
         rtsp_probe_timeout_ms,
         inference_workers_per_model,
         inference_workers_per_model_accelerated,
         inference_result_timeout_ms,
         offline_pipeline_depth,
         event_buffer_budget_mb,
         push_queue_max_frames,
         stream_reconnect_max_attempts,
         stream_reconnect_backoff_ms,
         stream_reconnect_backoff_max_ms,
         alarm_sound_path.c_str(),
         alarm_sound_cooldown_sec,
         font_path.c_str(),
         legend_position.c_str(),
         legend_prealloc_max_labels,
         legend_prealloc_max_label_len,
         bunkoff_overlap_ratio,
         hls_config.segment_seconds,
         hls_config.playlist_size,
         hls_config.delete_segments ? "true" : "false",
         hls_config.segment_format == Recorder::HlsSegmentFormat::kMpegts ? "mpegts" : "fmp4");
}
}  // namespace avs_analyzer