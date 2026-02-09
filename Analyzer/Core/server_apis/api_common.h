#ifndef ANALYZER_SERVER_API_COMMON_H
#define ANALYZER_SERVER_API_COMMON_H

#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <string>

#include <nlohmann/json.hpp>

void send_json(evhttp_request* req, int code, const std::string& json);
std::string escape_json(const std::string& s);
bool get_request_json(const evhttp_request* req, nlohmann::json& root);
void ParseGetRequest(const evhttp_request* req, evkeyvalq* params);
void ParsePostRequest(const evhttp_request* req, char* buff);

#endif  // ANALYZER_SERVER_API_COMMON_H
