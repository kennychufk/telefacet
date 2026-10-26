#include "client/Client.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "net/Protocol.hpp"
#include <stdexcept>
#include <thread>
#include <utility>

namespace telefacet::client {

using namespace std::chrono_literals;

Client::Client(config::Config cfg) : cfg_(std::move(cfg)) {
  msm_ = std::make_unique<net::MultiServerManager>(store_, cfg_);
  // Collect capture results, refusals and frames as they arrive (each on its
  // server's receive thread) so a capture call can block until all landed.
  msm_->setOnTriggerResult([this](std::size_t i, const net::TriggerResult& tr) {
    onCaptureResult(i, net::CaptureKind::Trigger, tr.trigger_id,
                    tr.captures.size(), &tr, nullptr);
  });
  msm_->setOnStackResult([this](std::size_t i, const net::StackResult& sr) {
    std::size_t images = 0;
    for (const auto& c : sr.captures)
      if (c.error.empty()) ++images;
    onCaptureResult(i, net::CaptureKind::Stack, sr.stack_id, images, nullptr,
                    &sr);
  });
  msm_->setOnCaptureRefused(
      [this](std::size_t i, net::CaptureKind kind, const std::string& msg) {
        onCaptureRefused(i, kind, msg);
      });
  msm_->setOnCaptureFrame(
      [this](std::size_t i, std::optional<std::size_t>,
             const net::CaptureFrame& cf, std::shared_ptr<const data::Capture>) {
        onCaptureFrame(i, cf.frame);
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

void Client::onCaptureResult(std::size_t server, net::CaptureKind kind,
                             std::uint32_t id, std::size_t expected,
                             const net::TriggerResult* tr,
                             const net::StackResult* sr) {
  {
    std::lock_guard<std::mutex> lk(capture_mu_);
    auto it = capture_pending_.find(server);
    if (!capture_active_ || kind != capture_kind_ ||
        it == capture_pending_.end() || it->second.acked)
      return;
    auto& p = it->second;
    p.acked    = true;
    p.id       = id;
    p.expected = expected;
    if (tr) p.trigger = *tr;
    if (sr) p.stack = *sr;
    p.done = expected == 0;
  }
  capture_cv_.notify_all();
}

void Client::onCaptureRefused(std::size_t server, net::CaptureKind kind,
                              const std::string& message) {
  {
    std::lock_guard<std::mutex> lk(capture_mu_);
    auto it = capture_pending_.find(server);
    if (!capture_active_ || kind != capture_kind_ ||
        it == capture_pending_.end() || it->second.acked)
      return;
    it->second.refusal = message;
    it->second.done    = true;
  }
  capture_cv_.notify_all();
}

void Client::onCaptureFrame(std::size_t server,
                            std::shared_ptr<const data::FrameBuffer> frame) {
  const auto kind =
      frame->frame_kind == static_cast<std::uint8_t>(proto::FrameKind::Stack)
          ? net::CaptureKind::Stack
          : net::CaptureKind::Trigger;
  {
    std::lock_guard<std::mutex> lk(capture_mu_);
    auto it = capture_pending_.find(server);
    if (!capture_active_ || kind != capture_kind_ ||
        it == capture_pending_.end() || !it->second.acked || it->second.done)
      return;
    auto& p = it->second;
    if (frame->capture_id != static_cast<std::uint16_t>(p.id)) return;
    p.frames.push_back(std::move(frame));
    p.done = p.frames.size() >= p.expected;
  }
  capture_cv_.notify_all();
}

bool Client::runCapture(net::CaptureKind kind,
                        const std::function<std::vector<std::size_t>()>& send,
                        std::chrono::milliseconds timeout,
                        std::unordered_map<std::size_t, PendingServer>& out,
                        std::vector<std::string>& refusals) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  // A capture still in flight (a previous call that timed out, or the other
  // kind) would make the servers refuse this one, and a late result of it
  // could be taken for this one's; let it finish first.
  if (!waitUntil([&] { return !msm_->anyCaptureInProgress(); }, timeout)) {
    refusals.push_back("an earlier capture is still in progress");
    return false;
  }

  // Arm for every connected server before sending: a result can follow the
  // request faster than this thread returns from send().
  {
    std::lock_guard<std::mutex> lk(capture_mu_);
    capture_pending_.clear();
    for (std::size_t i = 0; i < msm_->serverCount(); ++i)
      if (msm_->serverConnected(i)) capture_pending_[i] = PendingServer{};
    capture_kind_   = kind;
    capture_active_ = true;
  }
  const std::vector<std::size_t> sent = send();

  bool finished = false;
  {
    std::unique_lock<std::mutex> lk(capture_mu_);
    for (auto it = capture_pending_.begin(); it != capture_pending_.end();) {
      if (std::find(sent.begin(), sent.end(), it->first) == sent.end())
        it = capture_pending_.erase(it);
      else
        ++it;
    }
    if (!capture_pending_.empty()) {
      finished = capture_cv_.wait_until(lk, deadline, [&] {
        for (const auto& [_, p] : capture_pending_)
          if (!p.done) return false;
        return true;
      });
    }
    out = std::move(capture_pending_);
    capture_pending_.clear();
    capture_active_ = false;
  }
  if (sent.empty()) {
    refusals.push_back("the request reached no connected server");
    return false;
  }
  for (const auto& [server, p] : out) {
    if (!p.refusal.empty())
      refusals.push_back("server " + std::to_string(server) + ": " + p.refusal);
    else if (!p.done)
      spdlog::warn("telefacet::client: server {} {} — {}/{} frame(s) in time",
                   server, p.acked ? "acked" : "did not answer",
                   p.frames.size(), p.expected);
  }
  return finished && refusals.empty();
}

TriggerOutcome Client::triggerCapture(std::chrono::milliseconds timeout,
                                      const TriggerOptions& opts) {
  TriggerOutcome out;
  if (!msm_) return out;
  const int skip = opts.skip_frames.value_or(cfg_.saving.trigger_skip_frames);
  std::unordered_map<std::size_t, PendingServer> got;
  out.complete = runCapture(
      net::CaptureKind::Trigger,
      [&] { return msm_->triggerCaptureAll(skip, opts.send_image); }, timeout,
      got, out.refusals);
  if (!out.complete)
    spdlog::warn("telefacet::client: trigger incomplete{}",
                 out.refusals.empty() ? " (timed out)" : "");

  for (const auto& [server_index, p] : got) {
    if (p.trigger.cancelled) out.cancelled = true;
    for (const auto& f : p.frames) {
      CapturedFrame cf;
      cf.server_index    = server_index;
      cf.local_camera_id = f->camera_id;
      cf.frame_id        = f->frame_id;
      cf.frame           = f;
      // Map the server-local id onto the global roster the caller sees. An
      // unknown camera (discovery race) keeps global id 0 rather than dropping
      // the frame.
      if (auto* info = store_.findByServer(server_index, f->camera_id))
        cf.global_camera_id = info->global_id;
      out.captures.push_back(std::move(cf));
    }
  }
  std::sort(out.captures.begin(), out.captures.end(),
            [](const CapturedFrame& a, const CapturedFrame& b) {
              return a.global_camera_id < b.global_camera_id;
            });
  return out;
}

StackOutcome Client::stackCapture(std::chrono::milliseconds timeout,
                                  const StackOptions& opts) {
  StackOutcome out;
  if (!msm_) return out;
  const auto& d = cfg_.saving;
  net::StackRequest req;
  req.num_frames  = opts.num_frames.value_or(d.stack_num_frames);
  req.skip_frames = opts.skip_frames.value_or(d.stack_skip_frames);
  const std::int64_t exposure = opts.exposure_time_us.value_or(d.stack_exposure_time);
  const double gain = opts.analogue_gain.value_or(d.stack_analogue_gain);
  if (exposure > 0) req.exposure_time_us = exposure;
  if (gain > 0) req.analogue_gain = gain;
  req.send_image = opts.send_image;

  std::unordered_map<std::size_t, PendingServer> got;
  out.complete = runCapture(
      net::CaptureKind::Stack, [&] { return msm_->stackCaptureAll(req); },
      timeout, got, out.refusals);
  if (!out.complete)
    spdlog::warn("telefacet::client: stack incomplete{}",
                 out.refusals.empty() ? " (timed out)" : "");

  for (const auto& [server_index, p] : got) {
    if (p.stack.cancelled) out.cancelled = true;
    for (const auto& c : p.stack.captures) {
      StackedCamera sc;
      sc.server_index    = server_index;
      sc.local_camera_id = c.camera_id;
      sc.stats           = c;
      if (auto* info = store_.findByServer(server_index, c.camera_id))
        sc.global_camera_id = info->global_id;
      for (const auto& f : p.frames)
        if (f->camera_id == c.camera_id) sc.frame = f;
      out.cameras.push_back(std::move(sc));
    }
  }
  std::sort(out.cameras.begin(), out.cameras.end(),
            [](const StackedCamera& a, const StackedCamera& b) {
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
