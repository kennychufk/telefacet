#pragma once

// Blocking wrapper around telefacet::net::WebSocketClient for end-to-end
// tests. The underlying client is event-driven (callbacks fire on the
// ixwebsocket worker thread); this harness turns those events into synchronous
// wait-points the test thread can drive.
//
// Usage:
//
//   BlockingClient c("ws://localhost:9001");
//   c.connect();
//   auto cams = c.waitForDiscovery();
//   c.sendAndExpectStatus(
//       [&] { return c.raw().configureCameras(1456, 1088, 1456, 1088, 0, 0); },
//       /*contains=*/"YUV420");
//   c.sendAndExpectStatus([&] { return c.raw().startCameras(); });
//   c.sendAndExpectStatus([&] { return c.raw().startStream(cams[0].id); });
//   auto frame = c.nextFrame();

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "data/FrameBufferPool.hpp"
#include "net/WebSocketClient.hpp"

namespace telefacet::test {

class TimeoutError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class CommandError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class BlockingClient {
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::milliseconds;
  static constexpr Duration kDefaultTimeout{5000};

  explicit BlockingClient(std::string address);
  ~BlockingClient();

  BlockingClient(const BlockingClient&) = delete;
  BlockingClient& operator=(const BlockingClient&) = delete;

  // Start background connection and wait for the Open callback.
  void connect(Duration timeout = kDefaultTimeout);

  // Tear down cleanly — safe to call multiple times.
  void disconnect();

  // Wait for the auto-discovery that fires on connect. Subsequent calls wait
  // for the next discovery (rare in practice — mostly called once per session).
  std::vector<net::DiscoveredCamera> waitForDiscovery(
      Duration timeout = kDefaultTimeout);

  // Issue a command (via the lambda) and block until the server responds with
  // a matching status or error. `expect_substring`, if non-empty, must appear
  // in the status message. Throws CommandError on server-side error or if
  // `send_cmd` returned false. Returns the status message text.
  std::string sendAndExpectStatus(
      const std::function<bool()>& send_cmd,
      const std::string& expect_substring = "",
      Duration timeout = kDefaultTimeout);

  // Pop the next frame from the queue.
  std::shared_ptr<data::FrameBuffer> nextFrame(
      Duration timeout = kDefaultTimeout);

  // Clear any buffered frames (useful after a mode toggle).
  void drainFrames();

  // Access to the underlying client for sending commands inside
  // sendAndExpectStatus lambdas.
  net::WebSocketClient& raw() { return *client_; }

 private:
  void onConnection(std::size_t, bool connected);
  void onDiscovery(std::size_t,
                   const std::vector<net::DiscoveredCamera>& cams);
  void onStatus(std::size_t, const std::string& type,
                const std::string& message);
  void onFrame(std::size_t, std::shared_ptr<data::FrameBuffer> buf);

  data::FrameBufferPool pool_;
  std::unique_ptr<net::WebSocketClient> client_;

  std::mutex mu_;
  std::condition_variable cv_;
  bool connected_ = false;

  std::optional<std::vector<net::DiscoveredCamera>> last_discovery_;

  struct StatusMsg {
    std::string type;
    std::string message;
  };
  std::deque<StatusMsg> status_queue_;

  std::deque<std::shared_ptr<data::FrameBuffer>> frame_queue_;
};

}  // namespace telefacet::test
