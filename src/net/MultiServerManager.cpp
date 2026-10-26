#include "net/MultiServerManager.hpp"

#include <spdlog/spdlog.h>

namespace telefacet::net {

MultiServerManager::MultiServerManager(data::CameraStore& store,
                                       const config::Config& cfg)
    : store_(store), cfg_(cfg) {
  for (std::size_t i = 0; i < cfg_.servers.size(); ++i) {
    auto client = std::make_unique<WebSocketClient>(
        i, cfg_.servers[i].address, store_.pool(), cfg_.servers[i].sensor);
    client->setOnDiscovery([this](std::size_t idx,
                                  const std::vector<DiscoveredCamera>& cs) {
      onDiscovery(idx, cs);
    });
    client->setOnFrame([this](std::size_t idx,
                              std::shared_ptr<data::FrameBuffer> b) {
      onFrame(idx, std::move(b));
    });
    // Re-forward each server's trigger ack to whoever subscribed on the
    // manager (the headless Client, or the UI).
    client->setOnTriggerResult([this](std::size_t idx, const TriggerResult& tr) {
      if (on_trigger_) on_trigger_(idx, tr);
    });
    client->setOnStackResult([this](std::size_t idx, const StackResult& sr) {
      if (on_stack_) on_stack_(idx, sr);
    });
    client->setOnCaptureRefused(
        [this](std::size_t idx, CaptureKind kind, const std::string& msg) {
          if (on_refused_) on_refused_(idx, kind, msg);
        });
    client->setOnCaptureFrame([this](std::size_t idx, const CaptureFrame& cf) {
      onCaptureFrame(idx, cf);
    });
    clients_.push_back(std::move(client));
  }
}

void MultiServerManager::connectAll() {
  for (auto& c : clients_) c->start();
}

void MultiServerManager::disconnectAll() {
  for (auto& c : clients_) c->stop();
}

void MultiServerManager::configureAll() {
  for (auto& c : clients_) {
    if (!c->connected()) continue;
    const auto& sc = cfg_.servers[c->serverIndex()];
    c->configureCameras(sc.width, sc.height, sc.raw);
  }
}

void MultiServerManager::unconfigureAll() {
  for (auto& c : clients_) {
    if (c->connected()) c->unconfigure();
  }
}

void MultiServerManager::startAllCameras() {
  for (auto& c : clients_) {
    if (c->connected()) c->startCameras();
  }
}

void MultiServerManager::stopAllCameras() {
  for (auto& c : clients_) {
    if (c->connected()) c->stopCameras();
  }
}

void MultiServerManager::getStateAll() {
  for (auto& c : clients_) {
    if (c->connected()) c->getState();
  }
}

void MultiServerManager::setSaveModeAll(const std::string& mode,
                                         const nlohmann::json& params) {
  for (auto& c : clients_) {
    if (c->connected()) c->setSaveMode(mode, params);
  }
}

std::vector<std::size_t> MultiServerManager::triggerCaptureAll(
    std::optional<int> skip_frames, std::optional<bool> send_image) {
  std::vector<std::size_t> sent;
  for (auto& c : clients_) {
    // camera_id omitted ⇒ the server arms every camera it has running, which
    // is what a multi-camera calibration pose needs.
    if (c->connected() &&
        c->triggerCapture(std::nullopt, skip_frames, send_image))
      sent.push_back(c->serverIndex());
  }
  return sent;
}

std::vector<std::size_t> MultiServerManager::stackCaptureAll(
    const StackRequest& req) {
  StackRequest all = req;
  all.camera_id.reset();
  std::vector<std::size_t> sent;
  for (auto& c : clients_) {
    // A server configured without `raw` would only refuse.
    if (!c->connected() || !cfg_.servers[c->serverIndex()].raw) continue;
    if (c->stackCapture(all)) sent.push_back(c->serverIndex());
  }
  return sent;
}

bool MultiServerManager::anyCaptureInProgress() const {
  for (const auto& c : clients_)
    if (c->connected() && c->captureInProgress()) return true;
  return false;
}

bool MultiServerManager::anyRawConfigured() const {
  for (const auto& s : cfg_.servers)
    if (s.raw) return true;
  return false;
}

void MultiServerManager::setHeaderOnlyAll(bool enabled) {
  for (auto& c : clients_) {
    if (c->connected()) c->setHeaderOnly(enabled);
  }
}

void MultiServerManager::resetFrameCountsAll() {
  for (auto& c : clients_) {
    if (c->connected()) c->resetFrameCounts();
  }
}

void MultiServerManager::setLensPositionAll(double lens_position) {
  for (auto& c : clients_) {
    if (c->connected()) c->setLensPosition(lens_position);
  }
}

void MultiServerManager::setExposureTimeAll(std::int64_t exposure_time_us) {
  for (auto& c : clients_) {
    if (c->connected()) c->setExposureTime(exposure_time_us);
  }
}

void MultiServerManager::setFrameDurationAll(std::int64_t frame_duration_us) {
  for (auto& c : clients_) {
    if (c->connected()) c->setFrameDuration(frame_duration_us);
  }
}

bool MultiServerManager::getFrameDurationLimits() {
  for (auto& c : clients_) {
    if (c->connected()) return c->getFrameDurationLimits();
  }
  return false;
}

bool MultiServerManager::getLensPositionLimits() {
  for (auto& c : clients_) {
    if (c->connected()) return c->getLensPositionLimits();
  }
  return false;
}

nlohmann::json MultiServerManager::savingParamsFromConfig(
    const std::string& mode) const {
  const auto& s = cfg_.saving;
  const std::string& m = mode.empty() ? s.mode : mode;
  nlohmann::json p = {
      {"save_frames", s.save_frames},
      {"output_dir", s.output_dir},
      {"prepend_timestamp_to_dir", s.prepend_timestamp_to_dir},
      {"batch_size", s.batch_size},
      {"writer_threads", s.writer_threads},
      // Mode-independent resource guards; the server ignores the two byte
      // counts when they are 0 and decides for itself.
      {"backlog_max_bytes", s.backlog_max_bytes},
      {"disk_write_bytes_per_sec", s.disk_write_bytes_per_sec},
      {"allow_overcommit", s.allow_overcommit},
  };
  if (m == "checkerboard" || m == "checkerboard2x2") {
    p["checkerboard_rows"] = s.checkerboard_rows;
    p["checkerboard_cols"] = s.checkerboard_cols;
    p["checkerboard_full_res_detection"] = s.checkerboard_full_res_detection;
    p["checkerboard_num_threads"] = s.checkerboard_num_threads;
    if (s.checkerboard_subpix_window)
      p["checkerboard_subpix_window"] = *s.checkerboard_subpix_window;
  }
  if (m == "aruco" || m == "aruco2x2") {
    p["aruco_full_res_detection"] = s.aruco_full_res_detection;
    p["aruco_num_threads"] = s.aruco_num_threads;
    p["aruco_corner_refine"] = s.aruco_corner_refine;
  }
  return p;
}

bool MultiServerManager::startStream(std::size_t global_camera_id) {
  auto* info = store_.find(global_camera_id);
  if (!info) return false;
  auto* cli = client(info->server_index);
  if (!cli || !cli->connected()) return false;
  if (cli->startStream(info->local_camera_id)) {
    info->streaming = true;
    return true;
  }
  return false;
}

bool MultiServerManager::stopStream(std::size_t global_camera_id) {
  auto* info = store_.find(global_camera_id);
  if (!info) return false;
  auto* cli = client(info->server_index);
  if (!cli || !cli->connected()) return false;
  if (cli->stopStream(info->local_camera_id)) {
    info->streaming = false;
    return true;
  }
  return false;
}

void MultiServerManager::onDiscovery(
    std::size_t server_index, const std::vector<DiscoveredCamera>& cams) {
  for (const auto& dc : cams) {
    store_.addCamera(server_index, dc.id, dc.sensor_type);
  }
  store_.rebuildGlobalIdsSorted();
}

void MultiServerManager::onFrame(std::size_t server_index,
                                  std::shared_ptr<data::FrameBuffer> buf) {
  auto* info = store_.findByServer(server_index, buf->camera_id);
  if (!info) {
    // Drop frame for unknown camera (could happen briefly during discovery).
    store_.pool().release(std::move(buf));
    return;
  }
  store_.publishFrame(info->global_id, std::move(buf));
}

void MultiServerManager::onCaptureFrame(std::size_t server_index,
                                        const CaptureFrame& cf) {
  std::optional<std::size_t> global_id;
  std::shared_ptr<const data::Capture> stored;
  if (auto* info = store_.findByServer(server_index, cf.frame->camera_id)) {
    global_id = info->global_id;
    stored = store_.publishCapture(info->global_id, cf.frame);
  }
  if (on_capture_frame_) on_capture_frame_(server_index, global_id, cf, stored);
}

}  // namespace telefacet::net
