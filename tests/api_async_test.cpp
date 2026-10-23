#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "server_apis/server_context.h"
#include <threadpool/BS_thread_pool.hpp>

using namespace std::chrono_literals;

namespace {
BS::thread_pool<0> g_test_pool{2};
std::atomic<bool> g_slow_started{false};

void ApiHealthForTest(evhttp_request* req, void*) {
  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "{\"msg\":\"current service health\",\"code\":1000}");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type",
                    "application/json; charset=utf-8");
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}

// Mirrors the add/cancel/probe pattern: parse on the loop, own the request,
// run the blocking part on a pool, reply from the loop thread.
void ApiSlowForTest(evhttp_request* req, void* arg) {
  auto* base = static_cast<event_base*>(arg);
  g_slow_started.store(true, std::memory_order_release);
  evhttp_request_own(req);
  g_test_pool.detach_task([base, req]() {
    std::this_thread::sleep_for(800ms);
    ReplyJsonAsync(base, req, HTTP_OK, R"({"code":1000,"msg":"slow done"})");
  });
}

std::string http_request(const char* host, int port, const char* path,
                         const char* method) {
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const std::string portstr = std::to_string(port);
  if (getaddrinfo(host, portstr.c_str(), &hints, &res) != 0) {
    return {};
  }

  int sock = -1;
  for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) {
      continue;
    }
    timeval tv{2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    close(sock);
    sock = -1;
  }
  freeaddrinfo(res);
  if (sock == -1) {
    return {};
  }

  const std::string request = std::string(method) + " " + path +
                              " HTTP/1.1\r\nHost: " + host + ":" + portstr +
                              "\r\nConnection: close\r\n\r\n";
  if (send(sock, request.data(), request.size(), 0) <= 0) {
    close(sock);
    return {};
  }

  std::string response;
  char buf[4096];
  while (true) {
    const ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      response.append(buf, static_cast<size_t>(n));
      continue;
    }
    break;
  }
  close(sock);
  return response;
}

class AsyncApiFixture : public ::testing::Test {
 protected:
  static constexpr int kPort = 18082;

  void SetUp() override {
    static std::once_flag evthread_once;
    std::call_once(evthread_once, [] { evthread_use_pthreads(); });

    base_ = event_base_new();
    ASSERT_NE(base_, nullptr);
    server_thread_ = std::thread([this]() {
      evhttp* http = evhttp_new(base_);
      if (http == nullptr) {
        ready_.store(false, std::memory_order_release);
        return;
      }
      evhttp_set_default_content_type(http, "text/html; charset=utf-8");
      evhttp_set_timeout(http, 5);
      evhttp_set_cb(http, "/api/health", ApiHealthForTest, nullptr);
      evhttp_set_cb(http, "/api/slow", ApiSlowForTest, base_);
      if (evhttp_bind_socket(http, "127.0.0.1", kPort) != 0) {
        ready_.store(false, std::memory_order_release);
        evhttp_free(http);
        return;
      }
      ready_.store(true, std::memory_order_release);
      event_base_dispatch(base_);
      evhttp_free(http);
    });

    for (int i = 0; i < 200 && !ready_.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(ready_.load(std::memory_order_acquire));
  }

  void TearDown() override {
    if (base_) {
      event_base_loopbreak(base_);
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    if (base_) {
      event_base_free(base_);
    }
    base_ = nullptr;
  }

  event_base* base_{nullptr};
  std::thread server_thread_;
  std::atomic<bool> ready_{false};
};

// While a slow handler is blocked on a pool, unrelated endpoints must still
// answer promptly.
TEST_F(AsyncApiFixture, SlowRequestDoesNotBlockHealth) {
  g_slow_started.store(false, std::memory_order_release);

  std::string slow_response;
  std::thread slow_thread(
      [&slow_response]() {
        slow_response =
            http_request("127.0.0.1", kPort, "/api/slow", "POST");
      });

  for (int i = 0; i < 200 && !g_slow_started.load(std::memory_order_acquire);
       ++i) {
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(g_slow_started.load(std::memory_order_acquire));

  const auto start = std::chrono::steady_clock::now();
  const std::string health =
      http_request("127.0.0.1", kPort, "/api/health", "GET");
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_LT(elapsed_ms, 200) << "health was blocked by an in-flight request";
  EXPECT_NE(health.find("current service health"), std::string::npos)
      << health;

  slow_thread.join();
  EXPECT_NE(slow_response.find("slow done"), std::string::npos)
      << slow_response;
}
}  // namespace
