#pragma once

// Single-server WebSocket client (one ix::WebSocket connection). Owns a
// ChunkReassembler and a FrameBufferPool reference; emits parsed frames and
// JSON metadata via callbacks.

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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

  WebSocketClient(std::size_t server_index, std::string address,
                  data::FrameBufferPool& pool);
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

  // High-level commands (return false if disconnected).
  bool sendCommand(const nlohmann::json& cmd);
  bool discover();
  bool configureCameras(std::uint32_t width, std::uint32_t height,
                        std::uint32_t crop_w, std::uint32_t crop_h,
                        std::uint32_t crop_l, std::uint32_t crop_t);
  bool setSaveMode(const std::string& mode, const nlohmann::json& params);
  bool startCameras();
  bool stopCameras();
  bool startStream(std::uint32_t camera_id);
  bool stopStream(std::uint32_t camera_id);
  bool resetFrameCounts();
  bool setHeaderOnly(bool enabled);

  bool headerOnlyMode() const { return header_only_.load(); }

 private:
  void onMessage(const ix::WebSocketMessagePtr& msg);
  void handleText(const std::string& text);

  std::size_t                  server_index_;
  std::string                  address_;
  data::FrameBufferPool&       pool_;
  ix::WebSocket                ws_;
  ChunkReassembler             reassembler_;

  std::atomic<bool>            connected_{false};
  std::atomic<bool>            header_only_{false};

  std::vector<DiscoveredCamera> cameras_;
  std::mutex                    cameras_mu_;

  FrameCallback        on_frame_;
  DiscoveryCallback    on_discovery_;
  StatusCallback       on_status_;
  ConnectionCallback   on_conn_;
};

}  // namespace telefacet::net
