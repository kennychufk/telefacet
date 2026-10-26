#pragma once

// Coordinates N WebSocketClients and maintains the global camera id roster
// in CameraStore. Mirrors telefacet-web/src/services/WebSocketManager.js's
// MultiServerManager.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
  // Fire a trigger_capture on every connected server (§4.17), in any process
  // mode. Each server arms all of its running cameras, acks asynchronously
  // once they have delivered, then sends one capture frame per camera.
  // Returns the indices of the servers the request was sent to.
  std::vector<std::size_t> triggerCaptureAll(
      std::optional<int> skip_frames = std::nullopt,
      std::optional<bool> send_image = std::nullopt);
  // Fire a stack_capture on every connected server configured with `raw`
  // (§4.19); `req.camera_id` is ignored (each server stacks all its cameras).
  // Returns the indices of the servers the request was sent to.
  std::vector<std::size_t> stackCaptureAll(const StackRequest& req);
  // Whether any connected server still has a capture in progress — requested
  // and not refused, or acked with frames still to come.
  bool anyCaptureInProgress() const;
  // Whether the config asks any server for the raw stream stacking needs.
  bool anyRawConfigured() const;

  // Forwarded from every WebSocketClient. Each fires on that client's receive
  // thread, so the handler must be thread-safe.
  void setOnTriggerResult(WebSocketClient::TriggerResultCallback cb) {
    on_trigger_ = std::move(cb);
  }
  void setOnStackResult(WebSocketClient::StackResultCallback cb) {
    on_stack_ = std::move(cb);
  }
  void setOnCaptureRefused(WebSocketClient::CaptureRefusedCallback cb) {
    on_refused_ = std::move(cb);
  }
  // A capture frame has arrived and is now the camera's lastCapture() in the
  // store. `capture` is that stored record; the frame is unknown to the
  // roster (global id unresolved) only during a discovery race, in which case
  // nothing is stored and `capture` is null.
  using CaptureFrameCallback = std::function<void(
      std::size_t /*server_index*/, std::optional<std::size_t> /*global_id*/,
      const CaptureFrame&, std::shared_ptr<const data::Capture>)>;
  void setOnCaptureFrame(CaptureFrameCallback cb) {
    on_capture_frame_ = std::move(cb);
  }
  void setHeaderOnlyAll(bool enabled);
  void resetFrameCountsAll();
  // Global camera controls (applied identically on every server, §4.12–4.14).
  void setLensPositionAll(double lens_position);
  void setExposureTimeAll(std::int64_t exposure_time_us);
  void setFrameDurationAll(std::int64_t frame_duration_us);
  // Limits share a sensor across a server, so query the first connected one.
  bool getFrameDurationLimits();
  bool getLensPositionLimits();

  // set_process_mode params built from the loaded `processing` config, and the
  // configured mode string (used to seed the control panel). `mode` selects
  // which mode-specific keys are included; empty ⇒ configuredSaveMode(). Pass
  // it explicitly when issuing a mode that differs from the config's.
  nlohmann::json savingParamsFromConfig(const std::string& mode = {}) const;
  const std::string& configuredSaveMode() const { return cfg_.saving.mode; }
  // The loaded `processing` config, used to seed the (now live-editable)
  // save-mode controls in the UI.
  const config::FrameSavingCfg& savingConfig() const { return cfg_.saving; }

  // Per-camera (global id) routing helpers.
  bool startStream(std::size_t global_camera_id);
  bool stopStream(std::size_t global_camera_id);

  std::size_t serverCount() const { return clients_.size(); }
  bool serverConnected(std::size_t i) const {
    return i < clients_.size() && clients_[i]->connected();
  }
  const config::ServerCfg& serverConfig(std::size_t i) const {
    return cfg_.servers[i];
  }
  WebSocketClient* client(std::size_t i) {
    return i < clients_.size() ? clients_[i].get() : nullptr;
  }

 private:
  void onDiscovery(std::size_t server_index,
                   const std::vector<DiscoveredCamera>& cams);
  void onFrame(std::size_t server_index,
               std::shared_ptr<data::FrameBuffer> buf);
  void onCaptureFrame(std::size_t server_index, const CaptureFrame& cf);

  data::CameraStore&                                store_;
  const config::Config&                             cfg_;
  std::vector<std::unique_ptr<WebSocketClient>>     clients_;
  WebSocketClient::TriggerResultCallback            on_trigger_;
  WebSocketClient::StackResultCallback              on_stack_;
  WebSocketClient::CaptureRefusedCallback           on_refused_;
  CaptureFrameCallback                              on_capture_frame_;
};

}  // namespace telefacet::net
