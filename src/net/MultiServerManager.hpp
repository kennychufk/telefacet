#pragma once

// Coordinates N WebSocketClients and maintains the global camera id roster
// in CameraStore. Mirrors telefacet-web/src/services/WebSocketManager.js's
// MultiServerManager.

#include <cstdint>
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
  void unconfigureAll();
  void startAllCameras();
  void stopAllCameras();
  // Re-query get_state on every connected server. The server reports lifecycle
  // transitions via `status` (not a proactive `state`), so the UI polls this to
  // keep serverState() honest after configure/start/stop and external changes.
  void getStateAll();
  void setSaveModeAll(const std::string& mode, const nlohmann::json& params);
  void setHeaderOnlyAll(bool enabled);
  void resetFrameCountsAll();
  // Global camera controls (applied identically on every server, §4.12–4.14).
  void setLensPositionAll(double lens_position);
  void setExposureTimeAll(std::int64_t exposure_time_us);
  void setFrameDurationAll(std::int64_t frame_duration_us);
  // Limits share a sensor across a server, so query the first connected one.
  bool getFrameDurationLimits();
  bool getLensPositionLimits();

  // set_save_mode params built from the loaded frame_saving config, and the
  // configured mode string (used to seed the control panel).
  nlohmann::json savingParamsFromConfig() const;
  const std::string& configuredSaveMode() const { return cfg_.saving.mode; }
  // The loaded frame_saving config, used to seed the (now live-editable)
  // save-mode controls in the UI.
  const config::FrameSavingCfg& savingConfig() const { return cfg_.saving; }

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
