#include <event2/event.h>
#include <event2/thread.h>

#include <csignal>
#include <pthread.h>

#include "Core/Utils/log.h"
#include "Core/config.h"
#include "Core/scheduler.h"
#include "Core/server.h"
#include "Core/server_apis/server_context.h"

using namespace avs_analyzer;


int main(int argc, char** argv) {
  const char* file = nullptr;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (arg[0] != '-') {
      printf("parameter error:%s\n", arg);
      return -1;
    }
    if (arg[1] == 'h' && arg[2] == '\0') {
      printf("-h 打印参数配置信息并退出\n");
      printf("-f 配置文件    如：-f config.json \n");
      return 0;
    }
    if (arg[1] == 'f' && arg[2] == '\0') {
      if (++i >= argc) {
        printf("set parameter error: missing value for -f\n");
        return -1;
      }
      file = argv[i];
      continue;
    }
    printf("set parameter error:%s\n", arg);
    return -1;
  }
  spdlog::set_default_logger(LoggerSingleton::instance().get_logger());

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    LOGE("failed to install signal mask");
    return -1;
  }

  if (file == nullptr) {
    LOGE("failed to read config file");
    return -1;
  }

  Config config(file);
  if (!config.state) {
    LOGE("failed to read config file: {}", file);
    return -1;
  }
  LoggerSingleton::instance().set_log_level(config.log_level);

  if (g_control_pool.get_thread_count() != static_cast<size_t>(config.thread_pool_size)) {
    g_control_pool.reset(config.thread_pool_size);
  }
  if (g_media_pool.get_thread_count() != static_cast<size_t>(config.offline_threads)) {
    g_media_pool.reset(config.offline_threads);
  }
  if (g_alarm_pool.get_thread_count() != static_cast<size_t>(config.alarm_threads)) {
    g_alarm_pool.reset(config.alarm_threads);
  }

  Scheduler scheduler(config);
  evthread_use_pthreads();
  event_config* evt_config = event_config_new();
  event_base* base = event_base_new_with_config(evt_config);
  FileRouteOptions file_opts{
    .root_dir = config.upload_dir,    // Must be absolute path
    .route_prefix = "/video",          // Requests to /video/* will serve from root_dir
    .default_file = "",      // Serve this when accessing /video/
    .max_age_seconds = config.file_cache_timeout,           // second
    .enable_cors = true,               // Enable CORS for cross-origin playback
    .follow_symlinks = false,          // Security: don't follow symlinks
    .enable_directory_listing = false  // Security: no directory listing
  };
  ServerContext server_ctx{&scheduler, base};
  auto server_thread = HTTPServerStart(scheduler, base, file_opts, &server_ctx);
  scheduler.SetState(true);
  std::thread scheduler_cleanup_thread(&Scheduler::CleanUpLoop, &scheduler);
  LOGI("Start Success");
  config.Show();
  int caught_signal = 0;
  if (sigwait(&mask, &caught_signal) != 0) {
    LOGE("sigwait failed");
  }
  if (base) {
    event_base_loopbreak(base);
  }

  scheduler.SetState(false);
  scheduler.NotifyCleanUp();
  scheduler.ClearWorker();
  scheduler_cleanup_thread.join();

  server_thread.join();

  // Deferred API replies are produced on the control pool; wait for them
  // before the event base and scheduler go away. Two rounds drain the
  // cross-submissions (control add -> media analysis -> control completion).
  scheduler.DrainAlarms();
  for (int round = 0; round < 2; ++round) {
    g_media_pool.wait();
    g_control_pool.wait();
  }
  g_alarm_pool.wait();

  event_base_free(base);
  event_config_free(evt_config);
  return 0;
}