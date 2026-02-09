#include "server_apis/api_common.h"

#include <string>

#include "Utils/log.h"
#include "alarm_sound.h"

void alarm_sound_cv(evhttp_request* req, void* arg) {
  auto* alarm_ptr = static_cast<AlarmController*>(arg);
  if (!alarm_ptr) {
    send_json(req, HTTP_SERVUNAVAIL, R"({\"msg\":\"alarm sound is not configured\"})");
    return;
  }

  evkeyvalq params{};
  ParseGetRequest(req, &params);
  const char* method = evhttp_find_header(&params, "method");
  if (!method) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"missing parameter 'method'"})");
  } else {
    const std::string m(method);
    if (m == "stop") {
      alarm_ptr->stop();
      evhttp_send_reply(req, HTTP_OK, "OK", nullptr);
      LOGD("alarm api: Alarm sound stopped.");
    } else if (m == "start") {
      alarm_ptr->trigger();
      evhttp_send_reply(req, HTTP_OK, "OK", nullptr);
      LOGD("alarm api: Alarm sound started.");
    } else {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"wrong method"})");
    }
  }
  evhttp_clear_headers(&params);
}
