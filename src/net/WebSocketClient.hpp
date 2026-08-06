#pragma once

// Single-server WebSocket client (one ix::WebSocket connection). Owns a
// ChunkReassembler and a FrameBufferPool reference; emits parsed frames and
// JSON metadata via callbacks.

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "data/FrameBufferPool.hpp"
#include "net/ChunkReassembler.hpp"

namespace telefacet::net {

struct DiscoveredCamera {
  std::uint32_t id = 0;          // server-local id
  std::string   sensor_type;     // e.g. "IMX296"
};

// Sensor's hardware FrameDurationLimits (µs) plus the currently-applied lock.
// From the `frame_duration_limits` response (§4.15). `valid` false until a
// reply arrives.
struct FrameDurationLimits {
  bool          valid       = false;
  std::int64_t  min         = 0;
  std::int64_t  max         = 0;
  int           num_cameras = 0;
  bool          has_current = false;  // a lock is in effect
  std::int64_t  current_min = 0;
  std::int64_t  current_max = 0;
};

// Sensor's hardware LensPosition range (dioptres) from `lens_position_limits`
// (§4.16). Each field is optional because the server reports JSON null when the
// module has no focuser. `valid` false until a reply arrives.
struct LensPositionLimits {
  bool                  valid       = false;
  std::optional<double> min;
  std::optional<double> max;
  std::optional<double> def;         // IPA default
  int                   num_cameras = 0;
};

// One camera's contribution to a `trigger_result` (§4.17): the frame the
// trigger kept, and where the server queued it.
struct TriggerCapture {
  std::uint32_t camera_id = 0;  // server-local id
  std::uint32_t frame_id  = 0;
  std::string   filename;       // empty when the mode runs with save_frames off
};

// Asynchronous ack for a `trigger_capture` request. Arrives once every armed
// camera on that server has delivered its frame — or, with `cancelled` set,
// when stop_cameras abandoned a still-pending trigger (`captures` then holds
// only the cameras that made it). `valid` is false until a reply lands.
struct TriggerResult {
  bool                        valid      = false;
  std::uint32_t               trigger_id = 0;
  bool                        cancelled  = false;
  std::vector<TriggerCapture> captures;
};

class WebSocketClient {
 public:
  using FrameCallback =
      std::function<void(std::size_t /*server_index*/,
                         std::shared_ptr<data::FrameBuffer>)>;
  using DiscoveryCallback =
      std::function<void(std::size_t /*server_index*/,
                         const std::vector<DiscoveredCamera>&)>;
  using StatusCallback =
      std::function<void(std::size_t /*server_index*/,
                         const std::string& /*type*/,
                         const std::string& /*message*/)>;
  using ConnectionCallback =
      std::function<void(std::size_t /*server_index*/, bool /*connected*/)>;
  using TriggerResultCallback =
      std::function<void(std::size_t /*server_index*/, const TriggerResult&)>;

  // `sensor` is the per-server sensor substring passed to `discover` (empty ⇒
  // omit the param and let the server use its default).
  WebSocketClient(std::size_t server_index, std::string address,
                  data::FrameBufferPool& pool, std::string sensor = {});
  ~WebSocketClient();

  void start();   // begin background connection
  void stop();    // graceful shutdown

  bool connected() const { return connected_.load(); }
  std::size_t serverIndex() const { return server_index_; }
  const std::string& address() const { return address_; }
  const std::vector<DiscoveredCamera>& cameras() const { return cameras_; }

  void setOnFrame(FrameCallback cb)        { on_frame_     = std::move(cb); }
  void setOnDiscovery(DiscoveryCallback cb){ on_discovery_ = std::move(cb); }
  void setOnStatus(StatusCallback cb)      { on_status_    = std::move(cb); }
  void setOnConnection(ConnectionCallback cb){ on_conn_    = std::move(cb); }
  // Fires on the WebSocket's receive thread when a `trigger_result` lands.
  void setOnTriggerResult(TriggerResultCallback cb) {
    on_trigger_ = std::move(cb);
  }

  // High-level commands (return false if disconnected). Omitted `width`/
  // `height` in configureCameras ⇒ the param is not sent (server default).
  bool sendCommand(const nlohmann::json& cmd);
  bool discover();
  bool getState();
  bool configureCameras(std::optional<std::uint32_t> width,
                        std::optional<std::uint32_t> height);
  bool unconfigure();
  bool setSaveMode(const std::string& mode, const nlohmann::json& params);
  // Save-on-demand shutter for the `trigger` process mode (§4.17). Sending it
  // in any other mode draws an `error` from the server. `camera_id` omitted ⇒
  // every running camera on this server is armed; `skip_frames` omitted ⇒ the
  // server's configured `trigger_skip_frames`. Returns whether the request was
  // sent — the capture itself is confirmed later via the trigger-result
  // callback / lastTriggerResult().
  bool triggerCapture(std::optional<std::uint32_t> camera_id = std::nullopt,
                      std::optional<int> skip_frames = std::nullopt);
  bool startCameras();
  bool stopCameras();
  bool startStream(std::uint32_t camera_id);
  bool stopStream(std::uint32_t camera_id);
  bool resetFrameCounts();
  bool setHeaderOnly(bool enabled);
  // lens_position < 0 ⇒ continuous AF; >= 0 ⇒ manual at that dioptre value.
  bool setLensPosition(double lens_position);
  // exposure_time < 0 ⇒ auto AE; > 0 ⇒ manual shutter (µs, [1, 1000000]).
  bool setExposureTime(std::int64_t exposure_time_us);
  // frame_duration <= 0 ⇒ unset (libcamera default); > 0 ⇒ lock to that µs.
  bool setFrameDuration(std::int64_t frame_duration_us);
  bool getFrameDurationLimits();
  bool getLensPositionLimits();

  bool headerOnlyMode() const { return header_only_.load(); }

  // Latest server-reported state ("", "idle", "configured" or "running").
  std::string serverState() const;
  FrameDurationLimits frameDurationLimits() const;
  LensPositionLimits  lensPositionLimits() const;
  // Most recent `trigger_result` from this server (`valid` false until one
  // arrives). Polling this is the simplest way for a UI to show the outcome.
  TriggerResult       lastTriggerResult() const;

 private:
  void onMessage(const ix::WebSocketMessagePtr& msg);
  void handleText(const std::string& text);

  std::size_t                  server_index_;
  std::string                  address_;
  std::string                  sensor_;   // discover param; empty ⇒ omitted
  data::FrameBufferPool&       pool_;
  ix::WebSocket                ws_;
  ChunkReassembler             reassembler_;

  std::atomic<bool>            connected_{false};
  std::atomic<bool>            header_only_{false};

  std::vector<DiscoveredCamera> cameras_;
  std::mutex                    cameras_mu_;

  // Latest server metadata from text responses (state + limits).
  mutable std::mutex           meta_mu_;
  std::string                  server_state_;
  FrameDurationLimits          frame_duration_limits_;
  LensPositionLimits           lens_position_limits_;
  TriggerResult                last_trigger_result_;

  FrameCallback         on_frame_;
  DiscoveryCallback     on_discovery_;
  StatusCallback        on_status_;
  ConnectionCallback    on_conn_;
  TriggerResultCallback on_trigger_;
};

}  // namespace telefacet::net
