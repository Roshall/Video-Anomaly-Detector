#include "server.h"

#include <event2/event.h>
#include <event2/http.h>

#include <sstream>
#include <thread>

#include "Utils/log.h"
#include "config.h"

namespace {
int extract_port(const std::string& ip_address) {
  std::string port_str = ip_address.substr(ip_address.rfind(':') + 1);
  int port = 0;
  std::istringstream(port_str) >> port;
  return port;
}
}

std::thread HTTPServerStart(avs_analyzer::Scheduler& scheduler, event_base* event_base_p,
                            FileRouteOptions& file_route_opt) {
  int port = extract_port(scheduler.GetConfig().analyzer_host);
  return std::thread([&, event_base_p, port]() {
    LOGI("启动分析器服务：http://0.0.0.0:{}", port);
    evhttp* http = evhttp_new(event_base_p);
    evhttp_set_default_content_type(http, "text/html; charset=utf-8");

    evhttp_set_timeout(http, 0);
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