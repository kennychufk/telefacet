#include "client/Client.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <utility>

namespace telefacet::client {

using namespace std::chrono_literals;

Client::Client(config::Config cfg) : cfg_(std::move(cfg)) {
  msm_ = std::make_unique<net::MultiServerManager>(store_, cfg_);
  // Collect trigger acks as they arrive (one per server, each on that server's
  // receive thread) so triggerCapture() can block until all of them landed.
  msm_->setOnTriggerResult(
      [this](std::size_t server_index, const net::TriggerResult& tr) {
        {
          std::lock_guard<std::mutex> lk(trigger_mu_);
          trigger_acks_.emplace_back(server_index, tr);
        }
        trigger_cv_.notify_all();
      });
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

  // 3) Configure cameras (width/height come from the per-server config).
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

  // 4) Optional manual focus / exposure. These must come *after* configure —
  //    the server rejects set_lens_position / set_exposure_time unless it is
  //    CONFIGURED or RUNNING (websocket_server.cpp:503,529), and the rejection
  //    is an async `error` reply we'd never notice.
  if (opts.lens_position) msm_->setLensPositionAll(*opts.lens_position);
  if (opts.exposure_time_us) msm_->setExposureTimeAll(*opts.exposure_time_us);
  if (opts.frame_duration_us)
    msm_->setFrameDurationAll(*opts.frame_duration_us);

  // 5) Switch to the requested mode and stop persisting frames to disk. For the
  //    detector modes the corner block still rides every frame regardless of
  //    save_frames / header_only. Start from the config's `processing` params
  //    (output_dir/batch_size/... — mode-gated on the mode we're about to
  //    issue, not the config's), then let ClientOptions' explicit values win.
  nlohmann::json params = msm_->savingParamsFromConfig(opts.save_mode);
  params["save_frames"] = opts.save_frames;
  if (opts.save_mode == "aruco" || opts.save_mode == "aruco2x2") {
    params["aruco_full_res_detection"] = opts.aruco_full_res_detection;
    params["aruco_num_threads"]        = opts.aruco_num_threads;
    params["aruco_corner_refine"]      = opts.aruco_corner_refine;
  }
  msm_->setSaveModeAll(opts.save_mode, params);
  // Always issue the toggle rather than leaning on the server's `false` default,
  // so a caller can force pixels back on for a connection that had it turned on.
  msm_->setHeaderOnlyAll(opts.header_only);

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
  spdlog::info("telefacet::client: streaming {} ({}) on {} camera(s)",
               opts.save_mode, opts.header_only ? "header-only" : "full frames",
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

TriggerOutcome Client::triggerCapture(std::chrono::milliseconds timeout,
                                      std::optional<int> skip_frames) {
  TriggerOutcome out;
  if (!msm_) return out;

  // Drop any ack left over from a previous (timed-out) call before arming, so
  // a late reply can't be mistaken for this trigger's.
  {
    std::lock_guard<std::mutex> lk(trigger_mu_);
    trigger_acks_.clear();
    trigger_expected_ = 0;
  }

  const std::size_t armed = msm_->triggerCaptureAll(skip_frames);
  if (armed == 0) {
    spdlog::warn("telefacet::client: trigger reached no connected server");
    return out;
  }
  {
    std::lock_guard<std::mutex> lk(trigger_mu_);
    trigger_expected_ = armed;
  }

  std::vector<std::pair<std::size_t, net::TriggerResult>> acks;
  {
    std::unique_lock<std::mutex> lk(trigger_mu_);
    out.complete = trigger_cv_.wait_for(lk, timeout, [&] {
      return trigger_acks_.size() >= trigger_expected_;
    });
    acks = std::move(trigger_acks_);
    trigger_acks_.clear();
    trigger_expected_ = 0;
  }

  if (!out.complete)
    spdlog::warn("telefacet::client: trigger timed out — {}/{} server(s) acked",
                 acks.size(), armed);

  for (const auto& [server_index, ack] : acks) {
    if (ack.cancelled) out.cancelled = true;
    for (const auto& cap : ack.captures) {
      TriggerCapture tc;
      tc.server_index    = server_index;
      tc.local_camera_id = cap.camera_id;
      tc.frame_id        = cap.frame_id;
      tc.filename        = cap.filename;
      // Map the server-local id onto the global roster the caller sees. An
      // unknown camera (discovery race) keeps global id 0 rather than dropping
      // the capture, since the filename is still useful.
      if (auto* info = store_.findByServer(server_index, cap.camera_id))
        tc.global_camera_id = info->global_id;
      out.captures.push_back(std::move(tc));
    }
  }
  std::sort(out.captures.begin(), out.captures.end(),
            [](const TriggerCapture& a, const TriggerCapture& b) {
              return a.global_camera_id < b.global_camera_id;
            });
  return out;
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
