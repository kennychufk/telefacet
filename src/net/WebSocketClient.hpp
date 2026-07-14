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

  // High-level commands (return false if disconnected). Omitted `width`/
  // `height` in configureCameras ⇒ the param is not sent (server default).
  bool sendCommand(const nlohmann::json& cmd);
  bool discover();
  bool getState();
  bool configureCameras(std::optional<std::uint32_t> width,
                        std::optional<std::uint32_t> height);
  bool unconfigure();
  bool setSaveMode(const std::string& mode, const nlohmann::json& params);
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

  FrameCallback        on_frame_;
  DiscoveryCallback    on_discovery_;
  StatusCallback       on_status_;
  ConnectionCallback   on_conn_;
};

}  // namespace telefacet::net
