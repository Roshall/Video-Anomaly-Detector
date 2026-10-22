#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <netdb.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <mutex>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>

#include "server_apis/api_common.h"

#include "Utils/file_server_cb.h"

using namespace std::chrono_literals;

static std::atomic<int> stream_event_start_count{0};
static std::atomic<int> stream_event_end_count{0};

static void ApiHealthForTest(evhttp_request* req, void*) {
  evbuffer* buff = evbuffer_new();
  if (!buff) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, "No buffer");
    return;
  }
  evbuffer_add_printf(buff, "{\"msg\":\"current service health\",\"code\":1000}");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type",
                   "application/json; charset=utf-8");
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}

static void ApiStreamEventForTest(evhttp_request* req, void*) {
  // Very small query parsing: look for state=start/state=end
  const char* uri = evhttp_request_get_uri(req);
  std::string s = uri ? uri : "";
  if (s.find("state=start") != std::string::npos) {
    stream_event_start_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (s.find("state=end") != std::string::npos) {
    stream_event_end_count.fetch_add(1, std::memory_order_relaxed);
  }
  evbuffer* buff = evbuffer_new();
  if (!buff) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, "No buffer");
    return;
  }
  evbuffer_add_printf(buff, "{\"msg\":\"ok\",\"code\":0}");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type",
                   "application/json; charset=utf-8");
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}

// Test callback exercising the shared request-body parser get_request_json()
// from server_apis/api_common.cpp. Replies with the number of top-level
// elements of the parsed JSON body, or HTTP 400 with count -1 when the body
// cannot be parsed (this is the regression surface for the old 8KB
// ParsePostRequest truncation, report finding #3).
static void ApiEchoCountForTest(evhttp_request* req, void*) {
  nlohmann::json root;
  if (!get_request_json(req, root)) {
    evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, R"({"count":-1,"msg":"invalid request parameter"})");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type",
                     "application/json; charset=utf-8");
    evhttp_send_reply(req, HTTP_BADREQUEST, nullptr, buff);
    evbuffer_free(buff);
    return;
  }
  const auto count = static_cast<long long>(root.size());
  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, R"({"count":%lld})", count);
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type",
                   "application/json; charset=utf-8");
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}

// Helper: simple blocking HTTP POST over TCP, returns full response (headers+body).
static std::string http_post(const char* host, int port, const char* path,
                             const std::string& body,
                             const char* content_type = "application/json") {
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  std::string portstr = std::to_string(port);
  if (getaddrinfo(host, portstr.c_str(), &hints, &res) != 0) return {};

  int sock = -1;
  for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) continue;
    struct timeval tv{}; tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
    close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (sock == -1) return {};

  std::string req = std::string("POST ") + path + " HTTP/1.1\r\nHost: " + host + ":" +
                    portstr + "\r\nContent-Type: " + content_type +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
  ssize_t sent = send(sock, req.data(), static_cast<size_t>(req.size()), 0);
  if (sent <= 0) { close(sock); return {}; }

  std::string resp;
  char buf[4096];
  while (true) {
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      resp.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      break; // peer closed
    }
    break;
  }
  close(sock);
  return resp;
}

// Extract the response body (everything after the CRLFCRLF header separator).
static std::string response_body(const std::string& resp) {
  const auto pos = resp.find("\r\n\r\n");
  if (pos == std::string::npos) return {};
  return resp.substr(pos + 4);
}

// Helper: simple blocking HTTP GET over TCP, returns full response (headers+body).
static std::string http_get(const char* host, int port, const char* path) {
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  std::string portstr = std::to_string(port);
  if (getaddrinfo(host, portstr.c_str(), &hints, &res) != 0) return {};

  int sock = -1;
  for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) continue;
    // set a 2s timeout on connect and recv
    struct timeval tv{}; tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
    close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (sock == -1) return {};

  std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: " + host + ":" + portstr + "\r\nConnection: close\r\n\r\n";
  ssize_t sent = send(sock, req.data(), static_cast<size_t>(req.size()), 0);
  if (sent <= 0) { close(sock); return {}; }

  std::string resp;
  char buf[4096];
  while (true) {
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      resp.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      break; // peer closed
    }
    // timeout or error
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    break;
  }
  close(sock);
  return resp;
}

// Wait until the host:port is accepting connections or timeout (ms) elapses.
static bool wait_for_listen(const char* host, int port, int timeout_ms) {
  const int step_ms = 50;
  int waited = 0;
  while (waited < timeout_ms) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portstr = std::to_string(port);
    if (getaddrinfo(host, portstr.c_str(), &hints, &res) != 0) return false;

    int sock = -1;
    bool ok = false;
    for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
      sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (sock == -1) continue;
      // non-blocking connect with short timeout
      int flags = fcntl(sock, F_GETFL, 0);
      fcntl(sock, F_SETFL, flags | O_NONBLOCK);
      int rc = connect(sock, rp->ai_addr, rp->ai_addrlen);
      if (rc == 0) {
        ok = true;
        close(sock);
        break;
      } else if (errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(sock, &wf);
        struct timeval tv{0, step_ms * 1000};
        rc = select(sock + 1, nullptr, &wf, nullptr, &tv);
        if (rc > 0) {
          int so_error = 0;
          socklen_t len = sizeof(so_error);
          getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
          if (so_error == 0) {
            ok = true;
            close(sock);
            break;
          }
        }
      }
      close(sock);
    }
    freeaddrinfo(res);
    if (ok) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    waited += step_ms;
  }
  return false;
}

class HTTPServerFixture : public ::testing::Test {
 protected:
  static constexpr int PORT = 18081; // change if collision
  void SetUp() override {
    static std::once_flag evthread_once;
    std::call_once(evthread_once, []() {
      // Make libevent operations like event_base_loopbreak thread-safe.
      evthread_use_pthreads();
    });

    base_ = event_base_new();
    ASSERT_NE(base_, nullptr);

    // Create a temporary directory to serve files from
    char root_tmpl[] = "/tmp/avst_rootXXXXXX";
    char* root_res = mkdtemp(root_tmpl);
    ASSERT_NE(root_res, nullptr);
    tmp_root_dir_ = root_res;

    // Create a temporary upload directory for config
    char upload_tmpl[] = "/tmp/avst_uploadXXXXXX";
    char* upload_res = mkdtemp(upload_tmpl);
    ASSERT_NE(upload_res, nullptr);
    tmp_upload_dir_ = upload_res;

    // Create a sample file to retrieve via /video
    {
      const std::filesystem::path p = std::filesystem::path(tmp_root_dir_) / "sample.mp4";
      FILE* f = fopen(p.c_str(), "wb");
      ASSERT_NE(f, nullptr);
      const char payload[] = "dummy";
      fwrite(payload, 1, sizeof(payload) - 1, f);
      fclose(f);
    }

    // FileRouteOptions
    file_opts_ = FileRouteOptions{.root_dir = tmp_root_dir_, .route_prefix = "/video"};

    server_thread_ = std::thread([this]() {
      evhttp* http = evhttp_new(base_);
      if (!http) {
        server_failed_.store(true, std::memory_order_release);
        return;
      }
      evhttp_set_default_content_type(http, "text/html; charset=utf-8");
      evhttp_set_timeout(http, 2);
      evhttp_set_cb(http, "/api/health", ApiHealthForTest, nullptr);
      evhttp_set_cb(http, "/api/stream_event", ApiStreamEventForTest, nullptr);
      evhttp_set_cb(http, "/api/echo_count", ApiEchoCountForTest, nullptr);

      // Generic callback serves files under route_prefix (/video/...)
      evhttp_set_gencb(http, file_route_cb, &file_opts_);

      // Bind only to localhost for tests
      if (evhttp_bind_socket(http, "127.0.0.1", PORT) != 0) {
        server_failed_.store(true, std::memory_order_release);
        evhttp_free(http);
        return;
      }
      server_ready_.store(true, std::memory_order_release);
      event_base_dispatch(base_);
      evhttp_free(http);
    });

    // Wait for server thread to bind
    for (int i = 0; i < 100 && !server_ready_.load(std::memory_order_acquire) &&
                    !server_failed_.load(std::memory_order_acquire);
         ++i) {
      std::this_thread::sleep_for(10ms);
    }
    ASSERT_FALSE(server_failed_.load(std::memory_order_acquire));
    ASSERT_TRUE(server_ready_.load(std::memory_order_acquire));

    ASSERT_TRUE(wait_for_listen("127.0.0.1", PORT, 5000));
  }

  void TearDown() override {
    // ask event loop to stop and join thread
    if (base_) {
      event_base_loopbreak(base_);
    }
    if (server_thread_.joinable()) server_thread_.join();
    if (base_) event_base_free(base_);
    base_ = nullptr;

    // Clean up temp dirs
    if (!tmp_root_dir_.empty()) {
      std::filesystem::remove_all(tmp_root_dir_);
    }
    if (!tmp_upload_dir_.empty()) {
      std::filesystem::remove_all(tmp_upload_dir_);
    }
  }

  event_base* base_{nullptr};
  FileRouteOptions file_opts_;
  std::thread server_thread_;
  std::string tmp_root_dir_;
  std::string tmp_upload_dir_;
  std::atomic<bool> server_ready_{false};
  std::atomic<bool> server_failed_{false};
};

TEST_F(HTTPServerFixture, HealthEndpointReturnsExpectedJson) {
  std::string resp = http_get("127.0.0.1", PORT, "/api/health");
  ASSERT_FALSE(resp.empty()) << "no response from server";

  // Basic checks: HTTP status line and expected body text
  ASSERT_NE(resp.find("HTTP/1.1"), std::string::npos);
  ASSERT_NE(resp.find("current service health"), std::string::npos);
  ASSERT_NE(resp.find("\"code\""), std::string::npos);
}

TEST_F(HTTPServerFixture, VideoRouteRespondsToRequest) {
  // Retrieve a small test file served by file_route_cb.
  std::string resp = http_get("127.0.0.1", PORT, "/video/sample.mp4");
  ASSERT_FALSE(resp.empty()) << "no response from /video route";

  const auto line_end = resp.find("\r\n");
  ASSERT_NE(line_end, std::string::npos);
  const std::string status_line = resp.substr(0, line_end);
  // libevent may respond with HTTP/1.0 or HTTP/1.1
  ASSERT_NE(status_line.find(" 200"), std::string::npos) << "status_line=" << status_line << "\nresp=" << resp;
  ASSERT_NE(resp.find("Content-Type: video/mp4"), std::string::npos);
}

TEST_F(HTTPServerFixture, StreamEventEndpointRecordsStartEnd) {
  stream_event_start_count.store(0, std::memory_order_relaxed);
  stream_event_end_count.store(0, std::memory_order_relaxed);

  std::string resp1 = http_get("127.0.0.1", PORT, "/api/stream_event?state=start&streamCode=test123");
  ASSERT_FALSE(resp1.empty());
  std::this_thread::sleep_for(10ms);
  EXPECT_EQ(stream_event_start_count.load(std::memory_order_relaxed), 1);

  std::string resp2 = http_get("127.0.0.1", PORT, "/api/stream_event?state=end&streamCode=test123");
  ASSERT_FALSE(resp2.empty());
  std::this_thread::sleep_for(10ms);
  EXPECT_EQ(stream_event_end_count.load(std::memory_order_relaxed), 1);
}

// ---------------------------------------------------------------------------
// get_request_json() body parsing (report finding #3: the old
// ParsePostRequest truncated request bodies at 8KB).
// ---------------------------------------------------------------------------
TEST_F(HTTPServerFixture, PostSmallJsonBodyParses) {
  const std::string body = R"({"cameraId":"cam-1","sourceType":"stream"})";
  std::string resp = http_post("127.0.0.1", PORT, "/api/echo_count", body);
  ASSERT_FALSE(resp.empty()) << "no response from server";
  ASSERT_NE(resp.find("HTTP/1.1 200"), std::string::npos) << "resp=" << resp;
  EXPECT_NE(response_body(resp).find("\"count\":2"), std::string::npos)
      << "body=" << response_body(resp);
}

// Regression test: a POST body larger than the old 8KB limit must be parsed
// COMPLETELY. A truncated JSON array would fail to parse (count -1 / 400).
TEST_F(HTTPServerFixture, PostLargeBodyBeyond8KBParsesCompletely) {
  // Build a JSON array of 200 strings, each ~100 chars => ~20KB body,
  // well above the old kRecvBufMaxSize (8192) truncation point.
  nlohmann::json arr = nlohmann::json::array();
  for (int i = 0; i < 200; ++i) {
    std::string s = "media/path/";
    s += std::to_string(1000000 + i);
    while (s.size() < 100) s += "-padding-";
    arr.push_back(s);
  }
  const std::string body = arr.dump();
  ASSERT_GT(body.size(), 8192u) << "test body must exceed the old 8KB limit";

  std::string resp = http_post("127.0.0.1", PORT, "/api/echo_count", body);
  ASSERT_FALSE(resp.empty()) << "no response from server";
  ASSERT_NE(resp.find("HTTP/1.1 200"), std::string::npos)
      << "large body must not be rejected\nresp=" << resp.substr(0, 200);
  EXPECT_NE(response_body(resp).find("\"count\":200"), std::string::npos)
      << "all 200 elements must survive parsing\nbody=" << response_body(resp);
}

TEST_F(HTTPServerFixture, PostEmptyBodyFails) {
  std::string resp = http_post("127.0.0.1", PORT, "/api/echo_count", "");
  ASSERT_FALSE(resp.empty()) << "no response from server";
  // get_request_json() must reject an empty body: callback answers 400 / -1.
  EXPECT_NE(resp.find(" 400"), std::string::npos) << "resp=" << resp;
  EXPECT_NE(response_body(resp).find("\"count\":-1"), std::string::npos)
      << "body=" << response_body(resp);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
