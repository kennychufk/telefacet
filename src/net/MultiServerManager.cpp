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
    c->configureCameras(sc.width, sc.height);
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

std::size_t MultiServerManager::triggerCaptureAll(
    std::optional<int> skip_frames) {
  std::size_t sent = 0;
  for (auto& c : clients_) {
    // camera_id omitted ⇒ the server arms every camera it has running, which
    // is what a multi-camera calibration pose needs.
    if (c->connected() && c->triggerCapture(std::nullopt, skip_frames)) ++sent;
  }
  return sent;
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
  }
  if (m == "trigger") {
    p["trigger_skip_frames"] = s.trigger_skip_frames;
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

}  // namespace telefacet::net
