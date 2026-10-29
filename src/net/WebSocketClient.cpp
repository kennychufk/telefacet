#include "net/WebSocketClient.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "net/Protocol.hpp"

namespace telefacet::net {

namespace {

// Body of a `focus_fom_region` reply, or the `focus_fom_region` object on the
// configure status (§4.3, §4.18). Invalid if absent or null.
data::FocusFomRegion parseFocusFomRegion(const nlohmann::json& j) {
  data::FocusFomRegion r;
  if (!j.is_object()) return r;
  r.x            = j.value("x", 0u);
  r.y            = j.value("y", 0u);
  r.width        = j.value("width", 0u);
  r.height       = j.value("height", 0u);
  r.frame_width  = j.value("frame_width", 0u);
  r.frame_height = j.value("frame_height", 0u);
  r.valid        = r.width > 0 && r.height > 0 && r.frame_width > 0 &&
            r.frame_height > 0;
  return r;
}

RawStreamInfo parseRawStream(const nlohmann::json& j) {
  RawStreamInfo r;
  if (!j.is_object()) return r;
  r.format    = j.value("format", std::string{});
  r.width     = j.value("width", 0u);
  r.height    = j.value("height", 0u);
  r.stride    = j.value("stride", 0u);
  r.bit_depth = j.value("bit_depth", 0);
  r.valid     = r.width > 0 && r.height > 0;
  return r;
}

StackCapture parseStackCapture(const nlohmann::json& c) {
  StackCapture sc;
  sc.camera_id          = c.value("camera_id", 0u);
  sc.error              = c.value("error", std::string{});
  sc.frames_stacked     = c.value("frames_stacked", 0);
  sc.frames_rejected    = c.value("frames_rejected", 0);
  sc.settle_frames      = c.value("settle_frames", 0);
  sc.first_frame_id     = c.value("first_frame_id", 0u);
  sc.last_frame_id      = c.value("last_frame_id", 0u);
  sc.first_timestamp_us = c.value("first_timestamp_us", std::uint64_t{0});
  sc.last_timestamp_us  = c.value("last_timestamp_us", std::uint64_t{0});
  sc.exposure_time      = c.value("exposure_time", std::int64_t{0});
  sc.analogue_gain      = c.value("analogue_gain", 0.0);
  sc.black_level        = c.value("black_level", 0.0);
  sc.bit_depth          = c.value("bit_depth", 0);
  sc.white_fraction     = c.value("white_fraction", 0.0);
  sc.peak_fraction      = c.value("peak_fraction", 0.0);
  sc.clipped_fraction   = c.value("clipped_fraction", 0.0);
  sc.width              = c.value("width", 0u);
  sc.height             = c.value("height", 0u);
  sc.accumulate_ms      = c.value("accumulate_ms", 0.0);
  return sc;
}

// Whether an uncoded `error` that arrives while a capture request awaits its
// ack is that request's refusal. The server refuses a capture synchronously,
// so the reply directly follows the request; the wording check only keeps an
// unrelated command's error from the same moment from being taken for one.
// Every capture refusal names the capture, its camera or one of its fields.
bool looksLikeCaptureRefusal(const std::string& m) {
  for (const char* key :
       {"capture", "stack", "trigger", "running", "exposure_time",
        "skip_frames", "num_frames", "analogue_gain", "send_image",
        "camera_id"})
    if (m.find(key) != std::string::npos) return true;
  return false;
}

}  // namespace

WebSocketClient::WebSocketClient(std::size_t server_index, std::string address,
                                 data::FrameBufferPool& pool, std::string sensor)
    : server_index_(server_index),
      address_(std::move(address)),
      sensor_(std::move(sensor)),
      pool_(pool),
      reassembler_(pool_,
                   [this](std::shared_ptr<data::FrameBuffer> f) {
                     onAssembled(std::move(f));
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

void WebSocketClient::onAssembled(std::shared_ptr<data::FrameBuffer> f) {
  {
    std::lock_guard<std::mutex> lk(meta_mu_);
    f->focus_fom_region = focus_fom_region_;
  }
  if (!proto::isCaptureFrameKind(f->frame_kind)) {
    if (on_frame_) on_frame_(server_index_, std::move(f));
    return;
  }

  // A capture frame (§5.9): match it to the result that announced it, by kind
  // and capture_id (the id's low 16 bits), then by camera.
  const CaptureKind kind =
      f->frame_kind == static_cast<std::uint8_t>(proto::FrameKind::Stack)
          ? CaptureKind::Stack
          : CaptureKind::Trigger;
  CaptureFrame cf;
  bool matched = false, complete = false;
  {
    std::lock_guard<std::mutex> lk(meta_mu_);
    if (capture_acked_ && kind == capture_kind_ &&
        f->capture_id == static_cast<std::uint16_t>(capture_id_) &&
        capture_received_ < capture_entries_.size()) {
      for (const auto& e : capture_entries_) {
        if (e.value("camera_id", ~0u) == f->camera_id) {
          cf.result = e;
          break;
        }
      }
      matched  = true;
      complete = ++capture_received_ == capture_entries_.size();
    }
  }
  if (!matched) {
    spdlog::warn("[ws#{}] {} frame #{} from camera {} matches no pending "
                 "capture",
                 server_index_, captureKindName(kind), f->capture_id,
                 f->camera_id);
  } else if (complete) {
    spdlog::info("[ws#{}] {} #{}: all frames received", server_index_,
                 captureKindName(kind), f->capture_id);
  }
  cf.frame = std::move(f);
  if (on_capture_frame_) on_capture_frame_(server_index_, cf);
}

void WebSocketClient::noteCaptureRequested(CaptureKind kind) {
  std::lock_guard<std::mutex> lk(meta_mu_);
  capture_requests_.push_back(kind);
}

void WebSocketClient::noteCaptureAcked(CaptureKind kind, std::uint32_t id,
                                       std::vector<nlohmann::json> entries) {
  std::lock_guard<std::mutex> lk(meta_mu_);
  auto it = std::find(capture_requests_.begin(), capture_requests_.end(), kind);
  if (it != capture_requests_.end()) capture_requests_.erase(it);
  capture_acked_    = true;
  capture_kind_     = kind;
  capture_id_       = id;
  capture_entries_  = std::move(entries);
  capture_received_ = 0;
}

void WebSocketClient::resetCaptures() {
  // The server discards a disconnected commander's queued capture frames, so
  // nothing outstanding survives a reconnect.
  std::lock_guard<std::mutex> lk(meta_mu_);
  capture_requests_.clear();
  capture_acked_ = false;
  capture_entries_.clear();
  capture_received_ = 0;
}

void WebSocketClient::onMessage(const ix::WebSocketMessagePtr& msg) {
  switch (msg->type) {
    case ix::WebSocketMessageType::Open:
      connected_.store(true);
      slot_taken_.store(false);
      resetCaptures();
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
      resetCaptures();
      if (on_conn_) on_conn_(server_index_, false);
      break;

    case ix::WebSocketMessageType::Error:
      // A rejected upgrade carries the HTTP status: 503 means the server's
      // commander slot is already taken by another client (protocol §1.1).
      // ixwebsocket keeps retrying with backoff, so this repeats until the
      // other commander leaves.
      if (msg->errorInfo.http_status == 503) {
        slot_taken_.store(true);
        refused_upgrades_.fetch_add(1);
        spdlog::error("[ws#{}] upgrade refused (HTTP 503): the server already "
                      "has a commander client; retrying",
                      server_index_);
      } else if (msg->errorInfo.http_status != 0) {
        spdlog::error("[ws#{}] upgrade refused (HTTP {}): {}", server_index_,
                      msg->errorInfo.http_status, msg->errorInfo.reason);
      } else {
        spdlog::error("[ws#{}] error: {}", server_index_, msg->errorInfo.reason);
      }
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
    // Both the get_state reply (cause "query") and the server's unsolicited
    // pushes on every transition and on the observer's connect/disconnect
    // (protocol §3.1). Same shape, so the stored state is simply the newest.
    const std::string state = j.value("state", std::string{});
    const bool configured = state == "configured" || state == "running";
    bool need_region = false;
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      server_state_ = state;
      // The region is fixed by the configured mode: forget it in IDLE, and
      // fetch it when we find the server configured without knowing it (e.g.
      // after a reconnect, or when another client did the configuring).
      if (!configured) {
        focus_fom_region_ = {};
        raw_stream_       = {};
      }
      need_region = configured && !focus_fom_region_.valid;
    }
    if (need_region) getFocusFomRegion();
    spdlog::info("[ws#{}] server state: {} ({}{})", server_index_, state,
                 j.value("cause", std::string{"query"}),
                 j.value("observer_connected", false) ? ", observer present"
                                                       : "");
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
  } else if (type == "focus_fom_region") {
    const data::FocusFomRegion r = parseFocusFomRegion(j);
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      focus_fom_region_ = r;
    }
    spdlog::info("[ws#{}] FocusFoM region: {}x{} at ({}, {}) of {}x{}",
                 server_index_, r.width, r.height, r.x, r.y, r.frame_width,
                 r.frame_height);
  } else if (type == "trigger_result") {
    // Asynchronous ack for trigger_capture (§4.17) — it arrives only once the
    // triggered frame(s) have actually been captured, so it is what a robot
    // loop waits on before moving again. One capture frame per entry follows.
    TriggerResult tr;
    tr.valid      = true;
    tr.trigger_id = j.value("trigger_id", 0u);
    tr.cancelled  = j.value("cancelled", false);
    std::vector<nlohmann::json> entries;
    if (j.contains("captures") && j["captures"].is_array()) {
      for (const auto& c : j["captures"]) {
        TriggerCapture tc;
        tc.camera_id = c.value("camera_id", 0u);
        tc.frame_id  = c.value("frame_id", 0u);
        tr.captures.push_back(tc);
        entries.push_back(c);
      }
    }
    tr.raw = std::move(j);
    noteCaptureAcked(CaptureKind::Trigger, tr.trigger_id, std::move(entries));
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      last_trigger_result_ = tr;
    }
    spdlog::info("[ws#{}] trigger {} {}: {} capture(s)", server_index_,
                 tr.trigger_id, tr.cancelled ? "cancelled" : "complete",
                 tr.captures.size());
    if (on_trigger_) on_trigger_(server_index_, tr);
  } else if (type == "stack_result") {
    // Asynchronous result of stack_capture (§4.19). A stacked image follows
    // for every camera without an `error`.
    StackResult sr;
    sr.valid     = true;
    sr.stack_id  = j.value("stack_id", 0u);
    sr.cancelled = j.value("cancelled", false);
    std::vector<nlohmann::json> entries;
    if (j.contains("captures") && j["captures"].is_array()) {
      for (const auto& c : j["captures"]) {
        sr.captures.push_back(parseStackCapture(c));
        if (sr.captures.back().error.empty()) entries.push_back(c);
      }
    }
    sr.raw = std::move(j);
    const std::size_t images = entries.size();
    noteCaptureAcked(CaptureKind::Stack, sr.stack_id, std::move(entries));
    {
      std::lock_guard<std::mutex> lk(meta_mu_);
      last_stack_result_ = sr;
    }
    spdlog::info("[ws#{}] stack {} {}: {} camera(s), {} image(s) to follow",
                 server_index_, sr.stack_id,
                 sr.cancelled ? "cancelled" : "done", sr.captures.size(),
                 images);
    for (const auto& c : sr.captures)
      if (!c.error.empty())
        spdlog::warn("[ws#{}] stack {} camera {}: {}", server_index_,
                     sr.stack_id, c.camera_id, c.error);
    if (on_stack_) on_stack_(server_index_, sr);
  } else if (type == "status") {
    // The configure reply carries the FocusFoM region for the new mode.
    // So does its `raw` (null unless configured with raw: true).
    if (j.contains("focus_fom_region")) {
      std::lock_guard<std::mutex> lk(meta_mu_);
      focus_fom_region_ = parseFocusFomRegion(j["focus_fom_region"]);
      raw_stream_ = j.contains("raw") ? parseRawStream(j["raw"]) : RawStreamInfo{};
    }
    if (on_status_)
      on_status_(server_index_, type, j.value("message", std::string{}));
  } else if (type == "error") {
    // Coded errors (`capture_timeout`, or `forbidden` — which a commander
    // never gets) carry a `code`; plain replies carry only `message`.
    const std::string code = j.value("code", std::string{});
    const std::string message = j.value("message", std::string{});
    spdlog::error("[ws#{}] server error{}: {}", server_index_,
                  code.empty() ? "" : " [" + code + "]", message);
    // A capture request is refused with a plain error instead of its ack.
    std::optional<CaptureKind> refused;
    if (code.empty() && looksLikeCaptureRefusal(message)) {
      std::lock_guard<std::mutex> lk(meta_mu_);
      if (!capture_requests_.empty()) {
        refused = capture_requests_.front();
        capture_requests_.erase(capture_requests_.begin());
        last_capture_refusal_ = message;
      }
    }
    if (refused && on_capture_refused_)
      on_capture_refused_(server_index_, *refused, message);
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
                                       std::optional<std::uint32_t> h,
                                       bool raw) {
  // Only send fields that were configured; omitted ⇒ server keeps its default.
  nlohmann::json params = nlohmann::json::object();
  if (w) params["width"] = *w;
  if (h) params["height"] = *h;
  if (raw) params["raw"] = true;
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

bool WebSocketClient::triggerCapture(std::optional<std::uint32_t> camera_id,
                                     std::optional<int> skip_frames,
                                     std::optional<bool> send_image) {
  nlohmann::json cmd = {{"cmd", proto::cmd::kTriggerCapture}};
  if (camera_id) cmd["camera_id"] = *camera_id;
  // Clamped to the server's accepted range, so an out-of-range value cannot
  // turn into a refusal.
  if (skip_frames && *skip_frames > 0)
    cmd["skip_frames"] = std::min(*skip_frames, 100);
  if (send_image) cmd["send_image"] = *send_image;
  return sendCaptureRequest(CaptureKind::Trigger, cmd);
}

bool WebSocketClient::stackCapture(const StackRequest& req) {
  nlohmann::json cmd = {{"cmd", proto::cmd::kStackCapture}};
  if (req.camera_id) cmd["camera_id"] = *req.camera_id;
  if (req.num_frames) cmd["num_frames"] = std::clamp(*req.num_frames, 1, 1024);
  if (req.exposure_time_us && *req.exposure_time_us > 0)
    cmd["exposure_time"] =
        std::min<std::int64_t>(*req.exposure_time_us, 10000000);
  if (req.analogue_gain && *req.analogue_gain > 0)
    cmd["analogue_gain"] = std::clamp(*req.analogue_gain, 1.0, 64.0);
  if (req.skip_frames) cmd["skip_frames"] = std::clamp(*req.skip_frames, 0, 100);
  if (req.send_image) cmd["send_image"] = *req.send_image;
  return sendCaptureRequest(CaptureKind::Stack, cmd);
}

bool WebSocketClient::sendCaptureRequest(CaptureKind kind,
                                         const nlohmann::json& cmd) {
  // Recorded before sending: the refusal or ack is handled on the receive
  // thread and can land before send() returns here, and must find the request.
  noteCaptureRequested(kind);
  if (sendCommand(cmd)) return true;
  std::lock_guard<std::mutex> lk(meta_mu_);
  auto it = std::find(capture_requests_.rbegin(), capture_requests_.rend(), kind);
  if (it != capture_requests_.rend())
    capture_requests_.erase(std::next(it).base());
  return false;
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

bool WebSocketClient::getFocusFomRegion() {
  return sendCommand({{"cmd", proto::cmd::kGetFocusFomRegion}});
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

data::FocusFomRegion WebSocketClient::focusFomRegion() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return focus_fom_region_;
}

TriggerResult WebSocketClient::lastTriggerResult() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return last_trigger_result_;
}

StackResult WebSocketClient::lastStackResult() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return last_stack_result_;
}

RawStreamInfo WebSocketClient::rawStream() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return raw_stream_;
}

CaptureProgress WebSocketClient::captureProgress() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  CaptureProgress p;
  const bool receiving =
      capture_acked_ && capture_received_ < capture_entries_.size();
  if (!capture_requests_.empty() && !receiving) {
    // Requested, not yet acked.
    p.in_progress = true;
    p.kind        = capture_requests_.front();
    return p;
  }
  if (capture_acked_) {
    p.acked    = true;
    p.kind     = capture_kind_;
    p.id       = capture_id_;
    p.expected = capture_entries_.size();
    p.received = capture_received_;
  }
  p.in_progress = receiving || !capture_requests_.empty();
  return p;
}

std::string WebSocketClient::lastCaptureRefusal() const {
  std::lock_guard<std::mutex> lk(meta_mu_);
  return last_capture_refusal_;
}

}  // namespace telefacet::net
