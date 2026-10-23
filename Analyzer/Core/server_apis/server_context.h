#ifndef ANALYZER_SERVER_CONTEXT_H
#define ANALYZER_SERVER_CONTEXT_H

#include <event2/event.h>
#include <event2/http.h>

#include <string>

namespace avs_analyzer {
class Scheduler;
}

// Callback argument for endpoints that must not block the libevent loop.
// `scheduler` performs the blocking work on a worker pool; `base` is the loop
// used to deliver the reply from that worker.
struct ServerContext {
  avs_analyzer::Scheduler* scheduler{nullptr};
  event_base* base{nullptr};
};

// Takes ownership of `req` (call evhttp_request_own first), schedules a JSON
// reply on `base`, and frees the request afterwards. Must be called after the
// request has been parsed.
void ReplyJsonAsync(event_base* base, evhttp_request* req, int code,
                    std::string body);

#endif  // ANALYZER_SERVER_CONTEXT_H
