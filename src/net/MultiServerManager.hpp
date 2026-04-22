#pragma once

// Coordinates N WebSocketClients and maintains the global camera id roster
// in CameraStore. Mirrors telefacet-web/src/services/WebSocketManager.js's
// MultiServerManager.

#include <memory>
#include <string>
#include <vector>

#include "config/ConfigLoader.hpp"
#include "data/CameraStore.hpp"
#include "net/WebSocketClient.hpp"

namespace telefacet::net {

class MultiServerManager {
 public:
  MultiServerManager(data::CameraStore& store, const config::Config& cfg);

  void connectAll();
  void disconnectAll();

  // Broadcast helpers — issued to every connected server.
  void configureAll();
  void startAllCameras();
  void stopAllCameras();
  void setSaveModeAll(const std::string& mode, const nlohmann::json& params);
  void setHeaderOnlyAll(bool enabled);
  void resetFrameCountsAll();

  // Per-camera (global id) routing helpers.
  bool startStream(std::size_t global_camera_id);
  bool stopStream(std::size_t global_camera_id);

  std::size_t serverCount() const { return clients_.size(); }
  bool serverConnected(std::size_t i) const {
    return i < clients_.size() && clients_[i]->connected();
  }
  WebSocketClient* client(std::size_t i) {
    return i < clients_.size() ? clients_[i].get() : nullptr;
  }

 private:
  void onDiscovery(std::size_t server_index,
                   const std::vector<DiscoveredCamera>& cams);
  void onFrame(std::size_t server_index,
               std::shared_ptr<data::FrameBuffer> buf);

  data::CameraStore&                                store_;
  const config::Config&                             cfg_;
  std::vector<std::unique_ptr<WebSocketClient>>     clients_;
};

}  // namespace telefacet::net
