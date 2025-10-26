#include <event2/event.h>
#include <event2/thread.h>

#include <csignal>
#include <pthread.h>

#include "Core/Utils/log.h"
#include "Core/config.h"
#include "Core/scheduler.h"
#include "Core/server.h"
#include "Core/detector/constant_yolo.h"

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

  Scheduler scheduler(config);
  evthread_use_pthreads();
  event_config* evt_config = event_config_new();
  event_base* base = event_base_new_with_config(evt_config);
  auto server_thread = HTTPServerStart(scheduler, base);
  scheduler.SetState(true);
  std::thread scheduler_cleanup_thread(&Scheduler::CleanUpLoop, &scheduler);
  LOGI("Start Success");
  int caught_signal = 0;
  if (sigwait(&mask, &caught_signal) != 0) {
    LOGE("sigwait failed");
  }
  if (base) {
    event_base_loopbreak(base);
  }

  ClearYoloDetectorPool();

  scheduler.SetState(false);
  scheduler.NotifyCleanUp();
  scheduler.ClearWorker();
  scheduler_cleanup_thread.join();

  server_thread.join();
  event_base_free(base);
  event_config_free(evt_config);

  g_th_pool.wait();
  return 0;
}