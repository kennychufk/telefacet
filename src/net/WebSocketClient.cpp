#include "net/WebSocketClient.hpp"

#include <spdlog/spdlog.h>

#include "net/Protocol.hpp"

namespace telefacet::net {

WebSocketClient::WebSocketClient(std::size_t server_index, std::string address,
                                 data::FrameBufferPool& pool, std::string sensor)
    : server_index_(server_index),
      address_(std::move(address)),
      sensor_(std::move(sensor)),
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
      // Mirror JS: discover and sync server state immediately on connect
      // (don't assume IDLE — see protocol §4.2).
      discover();
      getState();
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
  } else if (type == "state") {
    const std::string state = j.value("state", std::string{});
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      server_state_ = state;
    }
    spdlog::info("[ws#{}] server state: {}", server_index_, state);
  } else if (type == "frame_duration_limits") {
    FrameDurationLimits fdl;
    fdl.valid       = true;
    fdl.min         = j.value("min", std::int64_t{0});
    fdl.max         = j.value("max", std::int64_t{0});
    fdl.num_cameras = j.value("num_cameras", 0);
    if (j.contains("current") && j["current"].is_object()) {
      fdl.has_current = true;
      fdl.current_min = j["current"].value("min", std::int64_t{0});
      fdl.current_max = j["current"].value("max", std::int64_t{0});
    }
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      frame_duration_limits_ = fdl;
    }
  } else if (type == "lens_position_limits") {
    // min/max/default are numbers or JSON null (module has no focuser).
    LensPositionLimits lpl;
    lpl.valid       = true;
    lpl.num_cameras = j.value("num_cameras", 0);
    auto num = [&](const char* k) -> std::optional<double> {
      if (j.contains(k) && !j[k].is_null()) return j[k].get<double>();
      return std::nullopt;
    };
    lpl.min = num("min");
    lpl.max = num("max");
    lpl.def = num("default");
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      lens_position_limits_ = lpl;
    }
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
  nlohmann::json cmd = {{"cmd", proto::cmd::kDiscover}};
  if (!sensor_.empty()) cmd["params"] = {{"sensor", sensor_}};
  return sendCommand(cmd);
}

bool WebSocketClient::getState() {
  return sendCommand({{"cmd", proto::cmd::kGetState}});
}

bool WebSocketClient::configureCameras(std::optional<std::uint32_t> w,
                                       std::optional<std::uint32_t> h) {
  // Only send fields that were configured; omitted ⇒ server keeps its default.
  nlohmann::json params = nlohmann::json::object();
  if (w) params["width"] = *w;
  if (h) params["height"] = *h;
  return sendCommand({{"cmd", proto::cmd::kConfigure}, {"params", params}});
}

bool WebSocketClient::unconfigure() {
  return sendCommand({{"cmd", proto::cmd::kUnconfigure}});
}

bool WebSocketClient::setSaveMode(const std::string& mode,
                                  const nlohmann::json& params) {
  return sendCommand(
      {{"cmd", proto::cmd::kSetProcessMode}, {"mode", mode}, {"params", params}});
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

bool WebSocketClient::setLensPosition(double lens_position) {
  return sendCommand(
      {{"cmd", proto::cmd::kSetLensPosition}, {"lens_position", lens_position}});
}

bool WebSocketClient::setExposureTime(std::int64_t exposure_time_us) {
  return sendCommand({{"cmd", proto::cmd::kSetExposureTime},
                      {"exposure_time", exposure_time_us}});
}

bool WebSocketClient::setFrameDuration(std::int64_t frame_duration_us) {
  return sendCommand({{"cmd", proto::cmd::kSetFrameDuration},
                      {"frame_duration", frame_duration_us}});
}

bool WebSocketClient::getFrameDurationLimits() {
  return sendCommand({{"cmd", proto::cmd::kGetFrameDurationLimits}});
}

bool WebSocketClient::getLensPositionLimits() {
  return sendCommand({{"cmd", proto::cmd::kGetLensPositionLimits}});
}

std::string WebSocketClient::serverState() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return server_state_;
}

FrameDurationLimits WebSocketClient::frameDurationLimits() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return frame_duration_limits_;
}

LensPositionLimits WebSocketClient::lensPositionLimits() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return lens_position_limits_;
}

}  // namespace telefacet::net
