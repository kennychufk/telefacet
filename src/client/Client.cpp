#include "client/Client.hpp"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace telefacet::client {

using namespace std::chrono_literals;

Client::Client(config::Config cfg) : cfg_(std::move(cfg)) {
  msm_ = std::make_unique<net::MultiServerManager>(store_, cfg_);
}

Client::~Client() { stop(); }

bool Client::waitUntil(const std::function<bool()>& pred,
                       std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

void Client::start(const ClientOptions& opts) {
  if (started_) return;
  if (cfg_.servers.empty())
    throw std::runtime_error("telefacet::client: no servers configured");

  const std::size_t n = cfg_.servers.size();

  // 1) Connect. Each WebSocketClient auto-runs `discover` + `get_state` on open.
  msm_->connectAll();
  if (!waitUntil([&] {
        for (std::size_t i = 0; i < n; ++i)
          if (!msm_->serverConnected(i)) return false;
        return true;
      }, opts.start_timeout))
    throw std::runtime_error("telefacet::client: timed out connecting to server(s)");

  // 2) Wait for discovery to populate the camera roster.
  if (!waitUntil([&] { return !store_.snapshot().empty(); }, opts.start_timeout))
    throw std::runtime_error("telefacet::client: no cameras discovered");
  spdlog::info("telefacet::client: discovered {} camera(s)",
               store_.snapshot().size());

  // 3) Optional manual focus / exposure before we start capturing.
  if (opts.lens_position) msm_->setLensPositionAll(*opts.lens_position);
  if (opts.exposure_time_us) msm_->setExposureTimeAll(*opts.exposure_time_us);

  // 4) Configure cameras (width/height come from the per-server config).
  msm_->configureAll();
  msm_->getStateAll();
  if (!waitUntil([&] {
        for (std::size_t i = 0; i < n; ++i) {
          auto* c = msm_->client(i);
          if (!c) return false;
          const std::string st = c->serverState();
          if (st != "configured" && st != "running") return false;
        }
        return true;
      }, opts.start_timeout))
    throw std::runtime_error("telefacet::client: servers did not reach 'configured'");

  // 5) Switch to aruco2x2 and stop persisting frames to disk. The corner block
  //    still rides every frame regardless of save_frames / header_only.
  nlohmann::json params = {
      {"save_frames", opts.save_frames},
      {"aruco_full_res_detection", opts.aruco_full_res_detection},
      {"aruco_num_threads", opts.aruco_num_threads},
      {"aruco_corner_refine", opts.aruco_corner_refine},
  };
  msm_->setSaveModeAll("aruco2x2", params);
  if (opts.header_only) msm_->setHeaderOnlyAll(true);

  // 6) Start capture, then open every camera's stream.
  msm_->startAllCameras();
  msm_->getStateAll();
  if (!waitUntil([&] {
        for (std::size_t i = 0; i < n; ++i) {
          auto* c = msm_->client(i);
          if (!c || c->serverState() != "running") return false;
        }
        return true;
      }, opts.start_timeout))
    throw std::runtime_error("telefacet::client: cameras did not reach 'running'");

  for (const auto& info : store_.snapshot()) msm_->startStream(info.global_id);

  started_ = true;
  spdlog::info("telefacet::client: streaming aruco2x2 on {} camera(s)",
               store_.snapshot().size());
}

void Client::stop() {
  if (!msm_) return;
  msm_->stopAllCameras();  // server tears down streams as a side effect
  msm_->disconnectAll();
  started_ = false;
}

bool Client::connected() const {
  if (!msm_) return false;
  for (std::size_t i = 0; i < cfg_.servers.size(); ++i)
    if (msm_->serverConnected(i)) return true;
  return false;
}

std::vector<data::CameraInfo> Client::cameras() const {
  return store_.snapshot();
}

void Client::copyOut(std::size_t global_id, const data::FrameBuffer& buf,
                     CameraMarkers& out) const {
  out.global_camera_id = global_id;
  out.local_camera_id  = buf.camera_id;
  out.frame_id         = buf.frame_id;
  out.timestamp_us     = buf.timestamp_us;
  out.lens_position    = buf.lens_position;
  out.width            = buf.width;
  out.height           = buf.height;
  out.markers          = buf.aruco_markers;  // small copy off the pooled buffer
}

bool Client::latest(std::size_t global_camera_id, CameraMarkers& out) {
  auto& cursor = seen_seq_[global_camera_id];
  auto buf = store_.consumeIfNew(global_camera_id, cursor);
  if (!buf) return false;
  copyOut(global_camera_id, *buf, out);  // copy immediately (buffer is recycled)
  if (auto* info = store_.find(global_camera_id))
    out.server_index = info->server_index;
  return true;
}

std::vector<CameraMarkers> Client::poll() {
  std::vector<CameraMarkers> out;
  for (const auto& info : store_.snapshot()) {
    CameraMarkers m;
    if (latest(info.global_id, m)) out.push_back(std::move(m));
  }
  return out;
}

}  // namespace telefacet::client
