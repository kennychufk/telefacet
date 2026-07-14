#pragma once

// ImGui control window — ports telefacet-web's pipeline state-machine UX plus
// focus / exposure / frame-duration controls driven by the server-reported
// limits. Sections:
//   Servers   — per-server connection + state
//   Pipeline  — Discover→Configure→Start→Stream advance/retreat state machine
//   Cameras   — per-camera stream toggles, fps, saved-frame count
//   Exposure  — auto AE vs manual shutter + frame-duration lock
//   Focus     — continuous AF vs manual lens position
//   Save mode — mode selector + live-editable saving params

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "data/CameraStore.hpp"
#include "net/MultiServerManager.hpp"

namespace telefacet::ui {

class ControlPanel {
 public:
  ControlPanel(data::CameraStore& store, net::MultiServerManager& msm);

  void draw();

  bool visible() const { return visible_; }
  void toggle()        { visible_ = !visible_; }

  // Aggregate pipeline stage across the connected servers (mirrors the web
  // client's appState: null → connected → configured → running).
  enum class Stage { None, Connected, Configured, Running };

 private:
  struct Agg {
    Stage stage = Stage::None;
    bool  disagree = false;      // connected servers report differing states
    int   connected_count = 0;
    int   first_connected = -1;  // representative client index for limits reads
  };
  Agg computeAgg() const;

  // Section renderers.
  void drawServers(const Agg& agg);
  void drawPipeline(const Agg& agg,
                    const std::vector<data::CameraInfo>& roster);
  void drawCameras(const Agg& agg,
                   const std::vector<data::CameraInfo>& roster);
  void drawOptions(const Agg& agg);
  void drawExposure(const Agg& agg);
  void drawFocus(const Agg& agg);
  void drawSaveMode();

  void advance(const Agg& agg, const std::vector<data::CameraInfo>& roster);
  void retreat(const Agg& agg);

  // Poll the representative server's limits and (re)issue the queries once the
  // pipeline reaches CONFIGURED/RUNNING (§4.15/§4.16 only valid there).
  void refreshLimits(const Agg& agg);

  nlohmann::json buildSaveParams() const;
  bool checkerboardMode() const;

  data::CameraStore&        store_;
  net::MultiServerManager&  msm_;
  bool                      visible_ = true;
  bool                      header_only_ = false;
  int                       save_mode_idx_ = 0;

  // Live-editable save params (seeded from config in the ctor).
  char output_dir_[256]     = {};
  bool prepend_ts_          = false;
  int  batch_size_          = 10;
  int  writer_threads_      = 4;
  int  cb_rows_             = 8;
  int  cb_cols_             = 11;
  bool cb_full_res_         = false;
  int  cb_threads_          = 4;

  // Focus control state. focus_manual_ false ⇒ continuous AF (sends -1).
  bool  focus_manual_ = false;
  float lens_pos_     = 0.0f;   // dioptres

  // Exposure / frame-duration control state.
  bool auto_ae_     = true;     // true ⇒ auto AE (sends -1)
  int  exposure_us_ = 10000;    // manual shutter, µs
  bool fd_locked_   = false;    // false ⇒ unset (sends -1)
  int  fd_us_       = 33333;    // frame-duration lock, µs

  // get_state poll timer — keeps serverState() fresh (the server only reports
  // transitions via `status`, so we re-query periodically + after each action).
  double last_state_poll_   = 0.0;

  // Limits-query latch: query once per entry into CONFIGURED/RUNNING, then
  // retry on a timer until a valid reply lands.
  bool   in_config_prev_    = false;
  double last_limits_query_ = 0.0;
};

}  // namespace telefacet::ui
