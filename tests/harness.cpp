#include "harness.hpp"

#include <thread>
#include <utility>

namespace telefacet::test {

BlockingClient::BlockingClient(std::string address)
    : pool_(),
      client_(std::make_unique<net::WebSocketClient>(0, std::move(address),
                                                      pool_)) {
  client_->setOnConnection(
      [this](std::size_t i, bool c) { onConnection(i, c); });
  client_->setOnDiscovery(
      [this](std::size_t i, const std::vector<net::DiscoveredCamera>& cams) {
        onDiscovery(i, cams);
      });
  client_->setOnStatus([this](std::size_t i, const std::string& t,
                              const std::string& m) { onStatus(i, t, m); });
  client_->setOnFrame(
      [this](std::size_t i, std::shared_ptr<data::FrameBuffer> b) {
        onFrame(i, std::move(b));
      });
}

BlockingClient::~BlockingClient() {
  // Best-effort: walk the state machine back to IDLE so the next test
  // starts fresh. Each fire-and-forget command will be rejected if the
  // server isn't in the right state — that's fine, we just want to cover
  // each edge once. The server retains state across client disconnects,
  // so without this the next test would start from CONFIGURED or RUNNING.
  if (client_ && client_->connected()) {
    client_->setHeaderOnly(false);
    client_->stopCameras();     // RUNNING  → CONFIGURED (no-op otherwise)
    client_->unconfigure();      // CONFIGURED → IDLE     (no-op otherwise)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  disconnect();
}

void BlockingClient::connect(Duration timeout) {
  client_->start();
  std::unique_lock<std::mutex> lk(mu_);
  if (!cv_.wait_for(lk, timeout, [this] { return connected_; })) {
    throw TimeoutError("Timed out waiting for WebSocket connect");
  }
}

void BlockingClient::disconnect() {
  if (!client_) return;
  client_->stop();
  std::lock_guard<std::mutex> lk(mu_);
  connected_ = false;
}

std::vector<net::DiscoveredCamera> BlockingClient::waitForDiscovery(
    Duration timeout) {
  std::unique_lock<std::mutex> lk(mu_);
  if (!cv_.wait_for(lk, timeout, [this] {
        return last_discovery_.has_value();
      })) {
    throw TimeoutError("Timed out waiting for discovery response");
  }
  auto cams = *last_discovery_;
  last_discovery_.reset();
  return cams;
}

std::string BlockingClient::sendAndExpectStatus(
    const std::function<bool()>& send_cmd,
    const std::string& expect_substring, Duration timeout) {
  // Clear any old status before sending so we only observe the response to
  // this command.
  {
    std::lock_guard<std::mutex> lk(mu_);
    status_queue_.clear();
  }

  if (!send_cmd()) {
    throw CommandError("Failed to send command (not connected?)");
  }

  std::unique_lock<std::mutex> lk(mu_);
  if (!cv_.wait_for(lk, timeout, [this] { return !status_queue_.empty(); })) {
    throw TimeoutError("Timed out waiting for status response");
  }

  auto msg = status_queue_.front();
  status_queue_.pop_front();

  if (msg.type == "error") {
    throw CommandError("Server error: " + msg.message);
  }
  if (!expect_substring.empty() &&
      msg.message.find(expect_substring) == std::string::npos) {
    throw CommandError("Status message missing '" + expect_substring +
                       "': " + msg.message);
  }
  return msg.message;
}

std::shared_ptr<data::FrameBuffer> BlockingClient::nextFrame(Duration timeout) {
  std::unique_lock<std::mutex> lk(mu_);
  if (!cv_.wait_for(lk, timeout,
                    [this] { return !frame_queue_.empty(); })) {
    throw TimeoutError("Timed out waiting for next frame");
  }
  auto f = std::move(frame_queue_.front());
  frame_queue_.pop_front();
  return f;
}

void BlockingClient::drainFrames() {
  std::lock_guard<std::mutex> lk(mu_);
  frame_queue_.clear();
}

void BlockingClient::onConnection(std::size_t, bool connected) {
  std::lock_guard<std::mutex> lk(mu_);
  connected_ = connected;
  cv_.notify_all();
}

void BlockingClient::onDiscovery(
    std::size_t, const std::vector<net::DiscoveredCamera>& cams) {
  std::lock_guard<std::mutex> lk(mu_);
  last_discovery_ = cams;
  cv_.notify_all();
}

void BlockingClient::onStatus(std::size_t, const std::string& type,
                              const std::string& message) {
  std::lock_guard<std::mutex> lk(mu_);
  status_queue_.push_back({type, message});
  cv_.notify_all();
}

void BlockingClient::onFrame(std::size_t,
                             std::shared_ptr<data::FrameBuffer> buf) {
  std::lock_guard<std::mutex> lk(mu_);
  frame_queue_.push_back(std::move(buf));
  cv_.notify_all();
}

}  // namespace telefacet::test
