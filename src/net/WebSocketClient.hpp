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

// The raw Bayer stream a `configure` with `raw: true` set up (§4.3), which
// stack_capture reads. `valid` false when the server was configured without it
// (or the reply has not arrived).
struct RawStreamInfo {
  bool          valid     = false;
  std::string   format;          // e.g. "SBGGR16"
  std::uint32_t width     = 0;
  std::uint32_t height    = 0;
  std::uint32_t stride    = 0;
  int           bit_depth = 0;   // sensor mode's; samples left-justified in 16
};

// The two on-demand captures (§4.17, §4.19). Both run in any process mode,
// get that mode's detection, save nothing on the server and send their frames
// to the commander after the ack (§5.9). One of either kind at a time.
enum class CaptureKind { Trigger, Stack };

inline const char* captureKindName(CaptureKind k) {
  return k == CaptureKind::Stack ? "stack" : "trigger";
}

// One camera's entry of a `trigger_result` (§4.17): the frame the trigger took.
struct TriggerCapture {
  std::uint32_t camera_id = 0;  // server-local id
  std::uint32_t frame_id  = 0;
};

// Asynchronous ack for a `trigger_capture` request. Arrives once every armed
// camera on that server has delivered its frame — or, with `cancelled` set,
// when stop_cameras abandoned a still-pending trigger (`captures` then holds
// only the cameras that made it). One capture frame per entry follows it.
// `valid` is false until a reply lands.
struct TriggerResult {
  bool                        valid      = false;
  std::uint32_t               trigger_id = 0;
  bool                        cancelled  = false;
  std::vector<TriggerCapture> captures;  // delivery order
  nlohmann::json              raw;       // the message as the server sent it
};

// One camera's entry of a `stack_result` (§4.19). When `error` is set the
// camera failed (settings never settled, kept changing, or cancelled), the
// other fields are partial and no stacked image follows for it.
struct StackCapture {
  std::uint32_t camera_id          = 0;
  std::string   error;
  int           frames_stacked     = 0;
  int           frames_rejected    = 0;
  int           settle_frames      = 0;
  std::uint32_t first_frame_id     = 0;
  std::uint32_t last_frame_id      = 0;
  std::uint64_t first_timestamp_us = 0;
  std::uint64_t last_timestamp_us  = 0;
  std::int64_t  exposure_time      = 0;    // µs, as libcamera reported it
  double        analogue_gain      = 0.0;
  double        black_level        = 0.0;  // 16-bit scale
  int           bit_depth          = 0;
  // 99.5th percentile, brightest sample and worst per-frame clipped share, as
  // fractions of full scale above black. Aim white_fraction at ~0.8.
  double        white_fraction     = 0.0;
  double        peak_fraction      = 0.0;
  double        clipped_fraction   = 0.0;
  std::uint32_t width              = 0;    // the stacked image: half the sensor
  std::uint32_t height             = 0;
  double        accumulate_ms      = 0.0;  // mean summing time per frame
};

// Asynchronous result of a `stack_capture`, sent when every armed camera has
// finished, failed or been cancelled. The corners are not in it: they ride on
// the stacked images that follow (§5.9).
struct StackResult {
  bool                      valid     = false;
  std::uint32_t             stack_id  = 0;
  bool                      cancelled = false;
  std::vector<StackCapture> captures;  // camera_id order
  nlohmann::json            raw;
};

// A stack_capture request (§4.19). Omitted fields take the server's defaults.
struct StackRequest {
  std::optional<std::uint32_t> camera_id;         // omitted ⇒ every camera
  std::optional<int>           num_frames;        // 1..1024 (server: 16)
  std::optional<std::int64_t>  exposure_time_us;  // pin; 1..10 000 000
  std::optional<double>        analogue_gain;     // pin; 1..64
  std::optional<int>           skip_frames;       // 0..100
  std::optional<bool>          send_image;        // server default true
};

// One capture frame (frame_kind Trigger or Stack) as it arrives, with the
// camera's entry from the trigger_result / stack_result that announced it
// (JSON null if the frame could not be matched to one).
struct CaptureFrame {
  std::shared_ptr<data::FrameBuffer> frame;
  nlohmann::json                     result;
};

// Where this connection's capture stands. A capture is in progress from the
// moment it is requested until its last frame has arrived (or the server
// refused it): the server accepts no other capture meanwhile.
struct CaptureProgress {
  bool          in_progress = false;
  bool          acked       = false;  // the result has arrived
  CaptureKind   kind        = CaptureKind::Trigger;
  std::uint32_t id          = 0;      // trigger_id / stack_id, once acked
  std::size_t   expected    = 0;      // frames announced by the result
  std::size_t   received    = 0;
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
  using StackResultCallback =
      std::function<void(std::size_t /*server_index*/, const StackResult&)>;
  using CaptureFrameCallback =
      std::function<void(std::size_t /*server_index*/, const CaptureFrame&)>;
  // A capture request the server answered with an immediate `error`.
  using CaptureRefusedCallback =
      std::function<void(std::size_t /*server_index*/, CaptureKind,
                         const std::string& /*message*/)>;

  // `sensor` is the per-server sensor substring passed to `discover` (empty ⇒
  // omit the param and let the server use its default).
  WebSocketClient(std::size_t server_index, std::string address,
                  data::FrameBufferPool& pool, std::string sensor = {});
  ~WebSocketClient();

  void start();   // begin background connection
  void stop();    // graceful shutdown

  bool connected() const { return connected_.load(); }
  // The last connection attempt was refused with HTTP 503: another commander
  // holds the server (protocol §1.1). Cleared on connect.
  bool commanderSlotTaken() const { return slot_taken_.load(); }
  // Upgrades refused with 503 since construction.
  int refusedUpgrades() const { return refused_upgrades_.load(); }
  std::size_t serverIndex() const { return server_index_; }
  const std::string& address() const { return address_; }
  const std::vector<DiscoveredCamera>& cameras() const { return cameras_; }

  void setOnFrame(FrameCallback cb)        { on_frame_     = std::move(cb); }
  void setOnDiscovery(DiscoveryCallback cb){ on_discovery_ = std::move(cb); }
  void setOnStatus(StatusCallback cb)      { on_status_    = std::move(cb); }
  void setOnConnection(ConnectionCallback cb){ on_conn_    = std::move(cb); }
  // The capture callbacks all fire on the WebSocket's receive thread, in wire
  // order: a result always precedes its frames. Live frames go to setOnFrame;
  // capture frames only ever to setOnCaptureFrame.
  void setOnTriggerResult(TriggerResultCallback cb) {
    on_trigger_ = std::move(cb);
  }
  void setOnStackResult(StackResultCallback cb) { on_stack_ = std::move(cb); }
  void setOnCaptureFrame(CaptureFrameCallback cb) {
    on_capture_frame_ = std::move(cb);
  }
  void setOnCaptureRefused(CaptureRefusedCallback cb) {
    on_capture_refused_ = std::move(cb);
  }

  // High-level commands (return false if disconnected). Omitted `width`/
  // `height` in configureCameras ⇒ the param is not sent (server default).
  bool sendCommand(const nlohmann::json& cmd);
  bool discover();
  bool getState();
  // `raw` adds the unpacked raw Bayer stream stack_capture needs (§4.3); it
  // costs width × height × 2 bytes per buffer, so it is off unless asked for.
  bool configureCameras(std::optional<std::uint32_t> width,
                        std::optional<std::uint32_t> height, bool raw = false);
  bool unconfigure();
  bool setSaveMode(const std::string& mode, const nlohmann::json& params);
  // Take the next frame of every running camera (or just `camera_id`), in any
  // process mode (§4.17). `skip_frames` (0..100) discards settling frames per
  // camera first; `send_image` false sends the frames header and detection
  // only. Returns whether the request was sent — the capture is confirmed by
  // the trigger-result callback and its frames by the capture-frame callback.
  bool triggerCapture(std::optional<std::uint32_t> camera_id = std::nullopt,
                      std::optional<int> skip_frames = std::nullopt,
                      std::optional<bool> send_image = std::nullopt);
  // Average raw frames per camera (§4.19). Needs RUNNING and a `raw`
  // configure. Answered by the stack-result callback, then one stacked image
  // per camera that did not fail.
  bool stackCapture(const StackRequest& req);
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
  // Ask for the region averaged into focus_fom (§4.18). Sent automatically
  // whenever the server is CONFIGURED/RUNNING and no region is known yet; the
  // configure reply also carries it.
  bool getFocusFomRegion();

  bool headerOnlyMode() const { return header_only_.load(); }

  // Latest server-reported state ("", "idle", "configured" or "running").
  std::string serverState() const;
  FrameDurationLimits frameDurationLimits() const;
  LensPositionLimits  lensPositionLimits() const;
  // Last reported FocusFoM region (`valid` false while IDLE / unknown). Also
  // stamped on every delivered FrameBuffer.
  data::FocusFomRegion focusFomRegion() const;
  // Most recent `trigger_result` / `stack_result` from this server (`valid`
  // false until one arrives). Polling these is the simplest way for a UI to
  // show the outcome.
  TriggerResult       lastTriggerResult() const;
  StackResult         lastStackResult() const;
  // The raw stream from the last configure reply.
  RawStreamInfo       rawStream() const;
  CaptureProgress     captureProgress() const;
  bool captureInProgress() const { return captureProgress().in_progress; }
  // The message of the last capture refusal (empty if none since connect).
  std::string         lastCaptureRefusal() const;

 private:
  void onMessage(const ix::WebSocketMessagePtr& msg);
  void handleText(const std::string& text);
  // Reassembled frame: live frames to on_frame_, capture frames to
  // on_capture_frame_ (matched against the current capture).
  void onAssembled(std::shared_ptr<data::FrameBuffer> f);
  // Record a capture request; the server's ack or refusal resolves it.
  void noteCaptureRequested(CaptureKind kind);
  // Record, then send; the record is withdrawn if the send fails.
  bool sendCaptureRequest(CaptureKind kind, const nlohmann::json& cmd);
  // A result arrived: the request it answers becomes the current capture.
  // `entries` holds one JSON entry per frame that will follow.
  void noteCaptureAcked(CaptureKind kind, std::uint32_t id,
                        std::vector<nlohmann::json> entries);
  void resetCaptures();

  std::size_t                  server_index_;
  std::string                  address_;
  std::string                  sensor_;   // discover param; empty ⇒ omitted
  data::FrameBufferPool&       pool_;
  ix::WebSocket                ws_;
  ChunkReassembler             reassembler_;

  std::atomic<bool>            connected_{false};
  std::atomic<bool>            header_only_{false};
  std::atomic<bool>            slot_taken_{false};
  std::atomic<int>             refused_upgrades_{0};

  std::vector<DiscoveredCamera> cameras_;
  std::mutex                    cameras_mu_;

  // Latest server metadata from text responses (state + limits).
  mutable std::mutex           meta_mu_;
  std::string                  server_state_;
  FrameDurationLimits          frame_duration_limits_;
  LensPositionLimits           lens_position_limits_;
  data::FocusFomRegion         focus_fom_region_;
  TriggerResult                last_trigger_result_;
  StackResult                  last_stack_result_;
  RawStreamInfo                raw_stream_;

  // Capture bookkeeping (guarded by meta_mu_). Requests the server has neither
  // acked nor refused, oldest first — normally at most one, but the client
  // never refuses locally, so a second request (which the server will refuse)
  // can be outstanding behind the first.
  std::vector<CaptureKind>     capture_requests_;
  // The acked capture whose frames are arriving.
  bool                         capture_acked_ = false;
  CaptureKind                  capture_kind_  = CaptureKind::Trigger;
  std::uint32_t                capture_id_    = 0;
  std::vector<nlohmann::json>  capture_entries_;  // one per expected frame
  std::size_t                  capture_received_ = 0;
  std::string                  last_capture_refusal_;

  FrameCallback         on_frame_;
  DiscoveryCallback     on_discovery_;
  StatusCallback        on_status_;
  ConnectionCallback    on_conn_;
  TriggerResultCallback on_trigger_;
  StackResultCallback   on_stack_;
  CaptureFrameCallback  on_capture_frame_;
  CaptureRefusedCallback on_capture_refused_;
};

}  // namespace telefacet::net
