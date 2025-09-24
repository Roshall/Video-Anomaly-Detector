#ifndef ANALYZER_SERVER_H
#define ANALYZER_SERVER_H
#include "Scheduler.h"

struct evhttp_request;

void HTTPServerStart(AVSAnalyzer::Scheduler& scheduler);
void api_index(evhttp_request* req, void* arg);
void api_health(evhttp_request* req, void* arg);
void api_controls(evhttp_request* req, void* arg);
void api_control(evhttp_request* req, void* arg);
void api_control_add(evhttp_request* req, void* arg);
void api_control_cancel(evhttp_request* req, void* arg);
void parse_get(evhttp_request* req, struct evkeyvalq* params);
void parse_post(evhttp_request* req, char* buff);

#endif //ANALYZER_SERVER_H

