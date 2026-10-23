#include "server_apis/api_common.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

#include "Utils/log.h"
#include "server_apis/server_context.h"

namespace {
constexpr int kRecvBufMaxSize = 1024 * 8;

struct PendingReply {
  evhttp_request* req{nullptr};
  int code{0};
  std::string body;
};

void AsyncReplyCb(evutil_socket_t, short, void* arg) {
  auto* reply = static_cast<PendingReply*>(arg);
  // libevent frees the (owned) request from evhttp_send_done once the reply
  // has been written, so it must not be freed here.
  send_json(reply->req, reply->code, reply->body);
  delete reply;
}
}  // namespace

void ReplyJsonAsync(event_base* base, evhttp_request* req, int code,
                    std::string body) {
  auto* reply = new PendingReply{req, code, std::move(body)};
  timeval zero{0, 0};
  if (event_base_once(base, -1, EV_TIMEOUT, AsyncReplyCb, reply, &zero) != 0) {
    LOGE("failed to schedule deferred HTTP reply");
    evhttp_request_free(reply->req);
    delete reply;
  }
}

void send_json(evhttp_request* req, int code, const std::string& json) {
  evbuffer* out = evbuffer_new();
  if (!out) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, "No buffer");
    return;
  }
  auto parsed = nlohmann::json::parse(json, nullptr, false);
  if (parsed.is_object() && parsed.contains("mime")) {
    LOGI("sending http code: {}, json: {}", code, json);
  } else {
    LOGI("sending http code: {}, json with mime data", code);
  }
  evbuffer_add(out, json.data(), json.size());
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json; charset=utf-8");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
  evhttp_send_reply(req, code, nullptr, out);
  evbuffer_free(out);
}

std::string escape_json(const std::string& s) {
  std::ostringstream os;
  for (unsigned char c : s) {
    switch (c) {
      case '"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\b': os << "\\b"; break;
      case '\f': os << "\\f"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (c < 0x20) {
          os << "\\u" << std::hex << std::uppercase << static_cast<int>(c);
        } else {
          os << c;
        }
    }
  }
  return os.str();
}

bool get_request_json(const evhttp_request* req, nlohmann::json& root) {
  if (req == nullptr || req->input_buffer == nullptr) {
    return false;
  }
  const size_t post_size = evbuffer_get_length(req->input_buffer);
  if (post_size == 0) {
    return false;
  }
  const char* data = reinterpret_cast<const char*>(evbuffer_pullup(req->input_buffer, -1));
  if (!data) {
    return false;
  }

  try {
    root = nlohmann::json::parse(data, data + post_size);
    return true;
  } catch (...) {
    return false;
  }
}

void ParseGetRequest(const evhttp_request* req, evkeyvalq* params) {
  if (req == nullptr) {
    return;
  }
  const char* url = evhttp_request_get_uri(req);
  if (url == nullptr) {
    return;
  }
  const evhttp_uri* decoded = evhttp_uri_parse(url);
  if (!decoded) {
    return;
  }
  const char* query = const_cast<char*>(evhttp_uri_get_query(decoded));
  if (query != nullptr) {
    evhttp_parse_query_str(query, params);
  }
  evhttp_uri_free(const_cast<evhttp_uri*>(decoded));
}

void ParsePostRequest(const evhttp_request* req, char* buff) {
  size_t post_size = 0;
  post_size = evbuffer_get_length(req->input_buffer);
  if (post_size <= 0) {
    return;
  }
  const size_t copy_len =
      post_size >= (kRecvBufMaxSize - 1) ? (kRecvBufMaxSize - 1) : post_size;
  memcpy(buff, evbuffer_pullup(req->input_buffer, -1), copy_len);
  buff[copy_len] = '\0';
}
