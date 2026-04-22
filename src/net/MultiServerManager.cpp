#include "net/MultiServerManager.hpp"

#include <spdlog/spdlog.h>

namespace telefacet::net {

MultiServerManager::MultiServerManager(data::CameraStore& store,
                                       const config::Config& cfg)
    : store_(store), cfg_(cfg) {
  for (std::size_t i = 0; i < cfg_.servers.size(); ++i) {
    auto client = std::make_unique<WebSocketClient>(i, cfg_.servers[i].address,
                                                    store_.pool());
    client->setOnDiscovery([this](std::size_t idx,
                                  const std::vector<DiscoveredCamera>& cs) {
      onDiscovery(idx, cs);
    });
    client->setOnFrame([this](std::size_t idx,
                              std::shared_ptr<data::FrameBuffer> b) {
      onFrame(idx, std::move(b));
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
    if (c->connected()) {
      c->configureCameras(cfg_.camera.width, cfg_.camera.height,
                          cfg_.camera.crop_width, cfg_.camera.crop_height,
                          cfg_.camera.crop_left, cfg_.camera.crop_top);
    }
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

void MultiServerManager::setSaveModeAll(const std::string& mode,
                                         const nlohmann::json& params) {
  for (auto& c : clients_) {
    if (c->connected()) c->setSaveMode(mode, params);
  }
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
  // Apply per-camera AWB gains from config (keyed by global id).
  auto roster = store_.snapshot();
  for (auto& info : roster) {
    auto gains = cfg_.gainsForGlobal(info.global_id);
    if (auto* live = store_.find(info.global_id)) live->awb = gains;
  }
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
