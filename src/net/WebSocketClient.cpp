#include "net/WebSocketClient.hpp"

#include <spdlog/spdlog.h>

#include "net/Protocol.hpp"

namespace telefacet::net {

WebSocketClient::WebSocketClient(std::size_t server_index, std::string address,
                                 data::FrameBufferPool& pool)
    : server_index_(server_index),
      address_(std::move(address)),
      pool_(pool),
      reassembler_(
          pool_,
          [this](std::shared_ptr<data::FrameBuffer> f) {
            if (on_frame_) on_frame_(server_index_, std::move(f));
          }) {
  ws_.setUrl(address_);
  ws_.disablePerMessageDeflate();
  // Auto-reconnect with exponential backoff (built-in).
  ws_.enableAutomaticReconnection();
  ws_.setMinWaitBetweenReconnectionRetries(1000);
  ws_.setMaxWaitBetweenReconnectionRetries(30000);
  ws_.setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr& m) { onMessage(m); });
}

WebSocketClient::~WebSocketClient() { stop(); }

void WebSocketClient::start() {
  spdlog::info("[ws#{}] connecting to {}", server_index_, address_);
  ws_.start();
}

void WebSocketClient::stop() {
  ws_.stop();
  reassembler_.reset();
  connected_.store(false);
}

void WebSocketClient::onMessage(const ix::WebSocketMessagePtr& msg) {
  switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      connected_.store(true);
      spdlog::info("[ws#{}] connected", server_index_);
      if (on_conn_) on_conn_(server_index_, true);
      // Mirror JS: discover immediately on connect.
      discover();
      break;

    case ix::WebSocketMessageType::Close:
      connected_.store(false);
      spdlog::warn("[ws#{}] closed: code={} reason={}", server_index_,
                   msg->closeInfo.code, msg->closeInfo.reason);
      reassembler_.reset();
      if (on_conn_) on_conn_(server_index_, false);
      break;

    case ix::WebSocketMessageType::Error:
      spdlog::error("[ws#{}] error: {}", server_index_, msg->errorInfo.reason);
      break;

    case ix::WebSocketMessageType::Message:
      if (msg->binary) {
        reassembler_.onBinary(msg->str.data(), msg->str.size());
      } else {
        handleText(msg->str);
      }
      break;

    default:
      break;
  }
}

void WebSocketClient::handleText(const std::string& text) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (const std::exception& e) {
    spdlog::error("[ws#{}] bad text msg: {}", server_index_, e.what());
    return;
  }
  const std::string type = j.value("type", "");
  if (type == "discovery") {
    std::vector<DiscoveredCamera> cams;
    if (j.contains("cameras") && j["cameras"].is_array()) {
      for (const auto& c : j["cameras"]) {
        DiscoveredCamera dc;
        dc.id          = c.value("id", 0u);
        dc.sensor_type = c.value("type", std::string{});
        cams.push_back(std::move(dc));
      }
    }
    {
      std::lock_guard<std::mutex> lk(cameras_mu_);
      cameras_ = cams;
    }
    spdlog::info("[ws#{}] discovered {} cameras", server_index_, cams.size());
    if (on_discovery_) on_discovery_(server_index_, cams);
  } else if (type == "status") {
    if (on_status_)
      on_status_(server_index_, type, j.value("message", std::string{}));
  } else if (type == "error") {
    spdlog::error("[ws#{}] server error: {}", server_index_,
                  j.value("message", std::string{}));
    if (on_status_)
      on_status_(server_index_, type, j.value("message", std::string{}));
  } else {
    spdlog::warn("[ws#{}] unknown text type: {}", server_index_, type);
  }
}

bool WebSocketClient::sendCommand(const nlohmann::json& cmd) {
  if (!connected_.load()) {
    spdlog::warn("[ws#{}] dropping command while disconnected: {}",
                 server_index_, cmd.dump());
    return false;
  }
  const std::string s = cmd.dump();
  auto info = ws_.send(s);
  return info.success;
}

bool WebSocketClient::discover() {
  return sendCommand({{"cmd", proto::cmd::kDiscover}});
}

bool WebSocketClient::configureCameras(std::uint32_t w, std::uint32_t h,
                                       std::uint32_t cw, std::uint32_t ch,
                                       std::uint32_t cl, std::uint32_t ct) {
  return sendCommand({
      {"cmd", proto::cmd::kConfigure},
      {"params", {{"width", w}, {"height", h}, {"crop_width", cw},
                  {"crop_height", ch}, {"crop_left", cl}, {"crop_top", ct}}},
  });
}

bool WebSocketClient::unconfigure() {
  return sendCommand({{"cmd", proto::cmd::kUnconfigure}});
}

bool WebSocketClient::setSaveMode(const std::string& mode,
                                  const nlohmann::json& params) {
  return sendCommand(
      {{"cmd", proto::cmd::kSetSaveMode}, {"mode", mode}, {"params", params}});
}

bool WebSocketClient::startCameras() {
  return sendCommand({{"cmd", proto::cmd::kStartCameras}});
}

bool WebSocketClient::stopCameras() {
  return sendCommand({{"cmd", proto::cmd::kStopCameras}});
}

bool WebSocketClient::startStream(std::uint32_t camera_id) {
  return sendCommand(
      {{"cmd", proto::cmd::kStartStream}, {"camera_id", camera_id}});
}

bool WebSocketClient::stopStream(std::uint32_t camera_id) {
  return sendCommand(
      {{"cmd", proto::cmd::kStopStream}, {"camera_id", camera_id}});
}

bool WebSocketClient::resetFrameCounts() {
  return sendCommand({{"cmd", proto::cmd::kResetFrameCounts}});
}

bool WebSocketClient::setHeaderOnly(bool enabled) {
  header_only_.store(enabled);
  return sendCommand(
      {{"cmd", proto::cmd::kSetHeaderOnly}, {"enabled", enabled}});
}

}  // namespace telefacet::net
