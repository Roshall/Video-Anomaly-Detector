#ifndef ANALYZER_SERVER_H
#define ANALYZER_SERVER_H
#include "scheduler.h"

struct evhttp_request;

void HTTPServerStart(avs_analyzer::Scheduler& scheduler);
void ApiIndex(evhttp_request* req, void* arg);
void ApiHealth(evhttp_request* req, void* arg);
void ApiAllMonitors(evhttp_request* req, void* arg);
void ApiMonitor(evhttp_request* req, void* arg);
void ApiAddMonitor(evhttp_request* req, void* arg);
void ApiRemoveMonitor(evhttp_request* req, void* arg);
void ParseGetRequest(const evhttp_request* req, struct evkeyvalq* params);
void ParsePostRequest(const evhttp_request* req, char* buff);

#endif  // ANALYZER_SERVER_H
