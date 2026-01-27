#ifndef ANALYZER_SERVER_H
#define ANALYZER_SERVER_H
#include <event2/http.h>

#include <thread>

#include "Utils/file_server_cb.h"
#include "scheduler.h"

struct evhttp_request;

std::thread HTTPServerStart(avs_analyzer::Scheduler& scheduler,
                            event_base* event_base_p,
                            FileRouteOptions& file_route_opt);
void ApiIndex(evhttp_request* req, void* arg);
void ApiHealth(evhttp_request* req, void* arg);
void ApiAllMonitors(evhttp_request* req, void* arg);
void ApiMonitor(evhttp_request* req, void* arg);
void ApiAddMonitor(evhttp_request* req, void* arg);
void ApiRemoveMonitor(evhttp_request* req, void* arg);
void ApiProbeRtsp(evhttp_request* req, void* arg);
void ParseGetRequest(const evhttp_request* req, evkeyvalq* params);
void ParsePostRequest(const evhttp_request* req, char* buff);
void ApiSnapShot(evhttp_request* req, void*);
void alarm_sound_cv(evhttp_request* req, void* arg);

#endif  // ANALYZER_SERVER_H
