#include "ui/ControlPanel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace telefacet::ui {

namespace {

const char* kSaveModes[] = {"none", "buffer", "batch", "trigger",
                            "checkerboard", "checkerboard2x2", "aruco",
                            "aruco2x2"};

// Palette (mirrors the web client's --live / --accent / muted tones).
const ImVec4 kGreen  = ImVec4(0.35f, 0.92f, 0.55f, 1.0f);
const ImVec4 kAccent = ImVec4(0.45f, 0.70f, 1.00f, 1.0f);
const ImVec4 kAmber  = ImVec4(0.98f, 0.72f, 0.30f, 1.0f);
const ImVec4 kRed    = ImVec4(1.00f, 0.40f, 0.40f, 1.0f);
const ImVec4 kWhite  = ImVec4(0.92f, 0.92f, 0.94f, 1.0f);

float clampf(float v, float lo, float hi) {
  return std::max(lo, std::min(hi, v));
}
// Snap to the nearest STEP and clean up float error (0.05 * 3 → 0.15).
float quantize(float v, float step) {
  return std::round(v / step) * step;
}

int stageIdx(ControlPanel::Stage s) {
  switch (s) {
    case ControlPanel::Stage::Connected:  return 0;
    case ControlPanel::Stage::Configured: return 1;
    case ControlPanel::Stage::Running:    return 2;
    default:                              return -1;
  }
}

}  // namespace

ControlPanel::ControlPanel(data::CameraStore& store,
                           net::MultiServerManager& msm)
    : store_(store), msm_(msm) {
  // Seed the save-mode selector + editable params from the loaded config so the
  // panel reflects what the YAML requested.
  const auto& s = msm_.savingConfig();
  for (int i = 0; i < IM_ARRAYSIZE(kSaveModes); ++i) {
    if (s.mode == kSaveModes[i]) { save_mode_idx_ = i; break; }
  }
  save_frames_    = s.save_frames;
  std::strncpy(output_dir_, s.output_dir.c_str(), sizeof(output_dir_) - 1);
  prepend_ts_     = s.prepend_timestamp_to_dir;
  batch_size_     = static_cast<int>(s.batch_size);
  writer_threads_ = static_cast<int>(s.writer_threads);
  cb_rows_        = s.checkerboard_rows;
  cb_cols_        = s.checkerboard_cols;
  cb_full_res_    = s.checkerboard_full_res_detection;
  cb_threads_     = s.checkerboard_num_threads;
  aruco_full_res_      = s.aruco_full_res_detection;
  aruco_threads_       = s.aruco_num_threads;
  aruco_corner_refine_ = s.aruco_corner_refine;
  trigger_skip_frames_ = s.trigger_skip_frames;
}

bool ControlPanel::checkerboardMode() const {
  const std::string m = kSaveModes[save_mode_idx_];
  return m == "checkerboard" || m == "checkerboard2x2";
}

bool ControlPanel::arucoMode() const {
  const std::string m = kSaveModes[save_mode_idx_];
  return m == "aruco" || m == "aruco2x2";
}

bool ControlPanel::triggerMode() const {
  return std::string(kSaveModes[save_mode_idx_]) == "trigger";
}

ControlPanel::Agg ControlPanel::computeAgg() const {
  Agg a;
  std::vector<std::string> known;  // non-empty states of connected servers
  for (std::size_t i = 0; i < msm_.serverCount(); ++i) {
    auto* c = msm_.client(i);
    if (!c || !c->connected()) continue;
    a.connected_count++;
    if (a.first_connected < 0) a.first_connected = static_cast<int>(i);
    std::string st = c->serverState();
    if (!st.empty()) known.push_back(std::move(st));
  }
  if (a.connected_count == 0) { a.stage = Stage::None; return a; }
  if (known.empty()) { a.stage = Stage::Connected; return a; }

  // Aggregate like the web client: a stage holds only if *every* known-state
  // server is at least there (minimum state).
  bool configured = true, running = true;
  for (const auto& st : known) {
    configured = configured && (st == "configured" || st == "running");
    running    = running && (st == "running");
  }
  a.stage = running ? Stage::Running
            : configured ? Stage::Configured
                         : Stage::Connected;
  for (const auto& st : known)
    if (st != known.front()) { a.disagree = true; break; }
  return a;
}

void ControlPanel::refreshLimits(const Agg& agg) {
  // Limits (§4.15/§4.16) only valid in CONFIGURED/RUNNING. Query once on entry,
  // then retry on a timer until a valid reply lands (don't block the UI).
  const bool in_cfg =
      (agg.stage == Stage::Configured || agg.stage == Stage::Running);
  const double now = ImGui::GetTime();
  bool need = false;
  if (in_cfg && !in_config_prev_) {
    need = true;
  } else if (in_cfg) {
    auto* c = agg.first_connected >= 0 ? msm_.client(agg.first_connected)
                                       : nullptr;
    const bool fdl_ok = c && c->frameDurationLimits().valid;
    const bool lpl_ok = c && c->lensPositionLimits().valid;
    if ((!fdl_ok || !lpl_ok) && (now - last_limits_query_) > 1.5) need = true;
  }
  if (need) {
    msm_.getFrameDurationLimits();
    msm_.getLensPositionLimits();
    last_limits_query_ = now;
  }
  in_config_prev_ = in_cfg;
}

void ControlPanel::draw() {
  if (!visible_) return;
  if (!ImGui::Begin("telefacet", &visible_)) {
    ImGui::End();
    return;
  }

  // Keep serverState() fresh: the server signals lifecycle transitions with
  // `status`, not a proactive `state`, so poll get_state on a slow timer.
  const double now = ImGui::GetTime();
  if (now - last_state_poll_ > 0.75) {
    msm_.getStateAll();
    last_state_poll_ = now;
  }

  const Agg agg = computeAgg();
  const auto roster = store_.snapshot();
  refreshLimits(agg);

  drawServers(agg);
  drawPipeline(agg, roster);
  drawCameras(agg, roster);
  drawOptions(agg);
  drawExposure(agg);
  drawFocus(agg);
  drawSaveMode();
  drawTrigger(agg);

  ImGui::End();
}

void ControlPanel::drawServers(const Agg& agg) {
  ImGui::SeparatorText("Servers");
  for (std::size_t i = 0; i < msm_.serverCount(); ++i) {
    auto* c = msm_.client(i);
    if (!c) continue;
    const bool ok = c->connected();
    ImGui::TextColored(ok ? kGreen : kRed, "%s", ok ? "*" : "o");
    ImGui::SameLine();
    ImGui::Text("#%zu %s", i, c->address().c_str());
    if (ok) {
      const std::string st = c->serverState();
      ImGui::SameLine();
      ImGui::TextDisabled("[%s]", st.empty() ? "..." : st.c_str());
    }
  }
  if (agg.disagree)
    ImGui::TextColored(kAmber, "! connected servers report differing states");
}

void ControlPanel::drawPipeline(const Agg& agg,
                                const std::vector<data::CameraInfo>& roster) {
  ImGui::SeparatorText("Pipeline");
  const int idx = stageIdx(agg.stage);

  struct StageDef { const char* label; const char* sub; };
  static const StageDef defs[3] = {
      {"Discover",  "cameras found"},
      {"Configure", "resolution"},
      {"Start",     "pipeline active"},
  };
  for (int i = 0; i < 3; ++i) {
    const bool done   = idx > i;
    const bool active = idx == i;
    const ImVec4 col = done ? kGreen : active ? kAccent : ImVec4(0.5f, 0.5f, 0.5f, 1);
    ImGui::TextColored(col, "%s", (done || active) ? "*" : "o");
    ImGui::SameLine();
    if (active)     ImGui::TextColored(kWhite, "%s", defs[i].label);
    else if (done)  ImGui::TextColored(kGreen, "%s", defs[i].label);
    else            ImGui::TextDisabled("%s", defs[i].label);
    if (active) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", defs[i].sub); }
  }

  const bool any_streaming =
      std::any_of(roster.begin(), roster.end(),
                  [](const data::CameraInfo& c) { return c.streaming; });
  const bool all_streaming =
      !roster.empty() &&
      std::all_of(roster.begin(), roster.end(),
                  [](const data::CameraInfo& c) { return c.streaming; });

  // Fixed-height live slot so the button row doesn't jump.
  if (agg.stage == Stage::Running && any_streaming)
    ImGui::TextColored(kGreen, "* LIVE");
  else
    ImGui::TextDisabled(" ");

  const bool can_retreat = idx > 0;
  bool can_advance = false;
  const char* adv_label = "Done";
  if (idx == 0)      { can_advance = true;            adv_label = "Configure >"; }
  else if (idx == 1) { can_advance = true;            adv_label = "Start >"; }
  else if (idx == 2) { can_advance = !all_streaming;  adv_label = "Stream >"; }

  const char* ret_label = "< Back";
  if (agg.stage == Stage::Running)         ret_label = "< Stop";
  else if (agg.stage == Stage::Configured) ret_label = "< Reset";

  const float w =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

  ImGui::BeginDisabled(!can_retreat);
  if (ImGui::Button(ret_label, ImVec2(w, 0))) retreat(agg);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_advance);
  if (ImGui::Button(adv_label, ImVec2(w, 0))) advance(agg, roster);
  ImGui::EndDisabled();
}

void ControlPanel::advance(const Agg& agg,
                           const std::vector<data::CameraInfo>& roster) {
  switch (agg.stage) {
    case Stage::Connected:
      msm_.configureAll();
      break;
    case Stage::Configured:
      msm_.startAllCameras();
      break;
    case Stage::Running:
      // Stream = start streams for any cameras not already streaming.
      for (const auto& info : roster)
        if (!info.streaming) msm_.startStream(info.global_id);
      break;
    default:
      break;
  }
  // Refresh state promptly so the pipeline reflects the transition (the reply
  // lands a frame or two later; the periodic poll is the fallback).
  msm_.getStateAll();
}

void ControlPanel::retreat(const Agg& agg) {
  switch (agg.stage) {
    case Stage::Running:
      msm_.stopAllCameras();
      // The server's stop_cameras also tears down all streams — mirror that
      // locally so the per-camera toggles reflect reality.
      for (const auto& info : store_.snapshot())
        if (auto* live = store_.find(info.global_id)) live->streaming = false;
      break;
    case Stage::Configured:
      msm_.unconfigureAll();
      break;
    default:
      break;
  }
  msm_.getStateAll();
}

void ControlPanel::drawCameras(const Agg& agg,
                               const std::vector<data::CameraInfo>& roster) {
  ImGui::SeparatorText("Cameras");
  if (roster.empty()) {
    ImGui::TextDisabled("(no cameras discovered yet)");
    return;
  }
  const bool running = (agg.stage == Stage::Running);
  const bool cb = checkerboardMode() || arucoMode();
  for (const auto& info : roster) {
    auto* live = store_.stats(info.global_id);
    const float fps  = live ? live->fps.load() : 0.0f;
    const float sfps = live ? live->server_fps.load() : 0.0f;
    const std::uint32_t saved = live ? live->frames_saved.load() : 0u;
    ImGui::PushID(static_cast<int>(info.global_id));
    bool streaming = info.streaming;
    ImGui::BeginDisabled(!running);
    if (ImGui::Checkbox("##stream", &streaming)) {
      if (streaming) msm_.startStream(info.global_id);
      else           msm_.stopStream(info.global_id);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%s (srv %zu/loc %u)", info.label.c_str(), info.server_index,
                info.local_camera_id);
    ImGui::SameLine();
    // client / server fps, mirroring the web's clientFps / serverFps readout.
    if (info.streaming) ImGui::TextColored(kGreen, "%.1f / %.1f fps", fps, sfps);
    else                ImGui::TextDisabled("--");
    if (cb) {
      ImGui::SameLine();
      ImGui::TextColored(kAccent, "  saved %u", saved);
    }
    ImGui::PopID();
  }
}

void ControlPanel::drawOptions(const Agg& agg) {
  ImGui::SeparatorText("Options");
  ImGui::BeginDisabled(agg.connected_count == 0);
  if (ImGui::Checkbox("Header-only mode", &header_only_))
    msm_.setHeaderOnlyAll(header_only_);
  ImGui::SameLine();
  if (ImGui::Button("Reset frame counts")) msm_.resetFrameCountsAll();
  ImGui::EndDisabled();
}

void ControlPanel::drawExposure(const Agg& agg) {
  ImGui::SeparatorText("Exposure & frame rate");
  const bool enabled =
      (agg.stage == Stage::Configured || agg.stage == Stage::Running);
  ImGui::BeginDisabled(!enabled);

  net::FrameDurationLimits fdl;
  if (agg.first_connected >= 0)
    if (auto* c = msm_.client(agg.first_connected)) fdl = c->frameDurationLimits();

  // Effective fps (mirrors ExposureSection.vue's readout logic).
  double eff = -1.0;
  if (fd_locked_ && auto_ae_)        eff = 1e6 / fd_us_;
  else if (!fd_locked_ && !auto_ae_) eff = 1e6 / exposure_us_;
  else if (fd_locked_ && !auto_ae_)  eff = 1e6 / std::max(exposure_us_, fd_us_);
  if (eff < 0) ImGui::Text("Effective: -- fps");
  else         ImGui::Text("Effective: %.1f fps", eff);

  // Mode toggles: auto AE (exposure<0) and frame-duration lock (fd<=0 unset).
  if (ImGui::Checkbox("Auto AE", &auto_ae_)) {
    if (auto_ae_) {
      msm_.setExposureTimeAll(-1);
    } else {
      if (fd_locked_ && exposure_us_ > fd_us_) exposure_us_ = fd_us_;
      msm_.setExposureTimeAll(exposure_us_);
    }
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Lock fps", &fd_locked_)) {
    if (fd_locked_) {
      if (!auto_ae_ && fd_us_ < exposure_us_) fd_us_ = exposure_us_;
      msm_.setFrameDurationAll(fd_us_);
    } else {
      msm_.setFrameDurationAll(-1);
    }
  }

  // Manual shutter (µs) — clamp [1, 1e6]; when fd is locked, cap at fd so the
  // sensor can actually deliver at the requested cadence.
  ImGui::BeginDisabled(auto_ae_);
  int exp = exposure_us_;
  const int exp_max = fd_locked_ ? std::min(1000000, std::max(1, fd_us_)) : 1000000;
  ImGui::SetNextItemWidth(220);
  if (ImGui::SliderInt("Shutter (us)", &exp, 1, 1000000, "%d",
                       ImGuiSliderFlags_Logarithmic)) {
    exposure_us_ = std::clamp(exp, 1, exp_max);
    msm_.setExposureTimeAll(exposure_us_);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("~%.0f fps", 1e6 / std::max(1, exposure_us_));
  ImGui::EndDisabled();

  // Frame-duration lock (µs) — range from the sensor's advertised limits.
  ImGui::BeginDisabled(!fd_locked_);
  int fd = fd_us_;
  int fd_min = 1000, fd_max = 1000000;  // fallback ~1..1000 fps
  if (fdl.valid && fdl.max > 0) {
    fd_min = static_cast<int>(std::max<std::int64_t>(1, fdl.min));
    fd_max = static_cast<int>(std::min<std::int64_t>(1000000, fdl.max));
  }
  fd = std::clamp(fd, fd_min, fd_max);
  ImGui::SetNextItemWidth(220);
  if (ImGui::SliderInt("Frame dur (us)", &fd, fd_min, fd_max, "%d",
                       ImGuiSliderFlags_Logarithmic)) {
    fd_us_ = std::clamp(fd, fd_min, fd_max);
    if (!auto_ae_ && fd_us_ < exposure_us_) fd_us_ = exposure_us_;
    msm_.setFrameDurationAll(fd_us_);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%.1f fps", 1e6 / std::max(1, fd_us_));
  ImGui::EndDisabled();

  if (fdl.valid)
    ImGui::TextDisabled("hw frame-dur: %lld-%lld us", static_cast<long long>(fdl.min),
                        static_cast<long long>(fdl.max));

  ImGui::EndDisabled();
}

void ControlPanel::drawFocus(const Agg& agg) {
  ImGui::SeparatorText("Focus");
  const bool enabled =
      (agg.stage == Stage::Configured || agg.stage == Stage::Running);
  ImGui::BeginDisabled(!enabled);

  net::LensPositionLimits lpl;
  if (agg.first_connected >= 0)
    if (auto* c = msm_.client(agg.first_connected)) lpl = c->lensPositionLimits();

  const bool no_focuser =
      lpl.valid && !lpl.min.has_value() && !lpl.max.has_value();

  if (no_focuser) {
    ImGui::TextDisabled("(module has no focuser - fixed focus)");
    ImGui::EndDisabled();
    return;
  }

  // Real hardware range, with safe fallbacks for null / not-yet-fetched fields.
  const float fmin = (lpl.valid && lpl.min) ? static_cast<float>(*lpl.min) : 0.0f;
  const float fmax = (lpl.valid && lpl.max) ? static_cast<float>(*lpl.max) : 10.0f;

  // When real limits arrive and the current position falls outside the range,
  // pull it back in (and push, since we're in manual).
  if (focus_manual_ && lpl.valid) {
    const float c = quantize(clampf(lens_pos_, fmin, fmax), 0.05f);
    if (c != lens_pos_) { lens_pos_ = c; msm_.setLensPositionAll(lens_pos_); }
  }

  // Auto (continuous AF) vs Manual segmented control.
  int mode = focus_manual_ ? 1 : 0;
  bool changed = false;
  changed |= ImGui::RadioButton("Auto (continuous AF)", &mode, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("Manual", &mode, 1);
  if (changed) {
    const bool now_manual = (mode == 1);
    if (now_manual != focus_manual_) {
      focus_manual_ = now_manual;
      if (focus_manual_)
        msm_.setLensPositionAll(quantize(clampf(lens_pos_, fmin, fmax), 0.05f));
      else
        msm_.setLensPositionAll(-1);  // continuous AF
    }
  }

  ImGui::BeginDisabled(!focus_manual_);
  float lp = lens_pos_;
  ImGui::SetNextItemWidth(220);
  if (ImGui::SliderFloat("Lens (dpt)", &lp, fmin, fmax, "%.2f")) {
    lens_pos_ = quantize(clampf(lp, fmin, fmax), 0.05f);
    msm_.setLensPositionAll(lens_pos_);
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("inf ... %.1f dpt%s", fmax,
                      lpl.valid ? "" : " (default range)");

  ImGui::EndDisabled();
}

void ControlPanel::drawSaveMode() {
  ImGui::SeparatorText("Save mode");
  ImGui::SetNextItemWidth(200);
  ImGui::Combo("##save_mode", &save_mode_idx_, kSaveModes,
               IM_ARRAYSIZE(kSaveModes));

  // Decouples detection from disk writing: when off, detector modes still run
  // and stream corners/markers but write nothing.
  ImGui::Checkbox("save frames to disk", &save_frames_);

  ImGui::BeginDisabled(!save_frames_);
  ImGui::SetNextItemWidth(220);
  ImGui::InputText("output_dir", output_dir_, sizeof(output_dir_));
  ImGui::Checkbox("prepend timestamp to dir", &prepend_ts_);
  ImGui::SetNextItemWidth(120);
  ImGui::InputInt("batch_size", &batch_size_);
  ImGui::SetNextItemWidth(120);
  ImGui::InputInt("writer_threads", &writer_threads_);
  ImGui::EndDisabled();

  if (checkerboardMode()) {
    ImGui::SeparatorText("Checkerboard params");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("rows", &cb_rows_);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("cols", &cb_cols_);
    ImGui::Checkbox("full-res detection", &cb_full_res_);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("detect threads", &cb_threads_);
  }

  if (triggerMode()) {
    ImGui::SeparatorText("Trigger params");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("skip_frames", &trigger_skip_frames_);
    ImGui::TextDisabled("frames discarded per camera before the kept one");
  }

  if (arucoMode()) {
    ImGui::SeparatorText("ArUco params");
    ImGui::Checkbox("full-res detection", &aruco_full_res_);
    ImGui::Checkbox("corner refine (subpix)", &aruco_corner_refine_);
    // num_threads applies to aruco2x2 quadrant parallelism only.
    ImGui::BeginDisabled(std::string(kSaveModes[save_mode_idx_]) != "aruco2x2");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("detect threads", &aruco_threads_);
    ImGui::EndDisabled();
  }

  if (ImGui::Button("Apply save mode")) {
    batch_size_     = std::max(1, batch_size_);
    writer_threads_ = std::max(1, writer_threads_);
    cb_rows_        = std::max(1, cb_rows_);
    cb_cols_        = std::max(1, cb_cols_);
    cb_threads_     = std::max(1, cb_threads_);
    aruco_threads_  = std::clamp(aruco_threads_, 1, 4);
    trigger_skip_frames_ = std::max(0, trigger_skip_frames_);
    msm_.setSaveModeAll(kSaveModes[save_mode_idx_], buildSaveParams());
    // Remember what the servers are actually running: the combo alone is just
    // an unapplied selection, and the trigger button must follow the former.
    applied_save_mode_ = kSaveModes[save_mode_idx_];
  }
}

// Manual shutter button for the `trigger` process mode — the same request an
// automated calibration rig issues once its arm has come to a complete stop.
void ControlPanel::drawTrigger(const Agg& agg) {
  const bool applied = applied_save_mode_ == "trigger";
  // Show the section as soon as the mode is *selected* so the button is
  // discoverable, but keep it inert until that mode has been pushed.
  if (!applied && !triggerMode()) return;

  ImGui::SeparatorText("Trigger");

  // Newest ack across the connected servers (each acks its own cameras).
  std::uint32_t newest_id = 0;
  std::size_t   captures  = 0;
  bool          any_ack   = false;
  bool          cancelled = false;
  for (std::size_t i = 0; i < msm_.serverCount(); ++i) {
    auto* c = msm_.client(i);
    if (!c) continue;
    const auto tr = c->lastTriggerResult();
    if (!tr.valid) continue;
    any_ack   = true;
    newest_id = std::max(newest_id, tr.trigger_id);
    captures += tr.captures.size();
    cancelled = cancelled || tr.cancelled;
  }
  if (trigger_pending_ && any_ack && newest_id != trigger_baseline_id_)
    trigger_pending_ = false;

  const bool ready = applied && agg.stage == Stage::Running;
  ImGui::BeginDisabled(!ready || trigger_pending_);
  if (ImGui::Button("Trigger capture", ImVec2(-1, 0))) {
    trigger_baseline_id_ = newest_id;
    trigger_pending_ =
        msm_.triggerCaptureAll(std::max(0, trigger_skip_frames_)) > 0;
  }
  ImGui::EndDisabled();

  if (!applied) {
    ImGui::TextDisabled("press \"Apply save mode\" to switch the servers to trigger");
  } else if (agg.stage != Stage::Running) {
    ImGui::TextDisabled("cameras must be running");
  } else if (trigger_pending_) {
    ImGui::TextColored(kAmber, "waiting for the triggered frame...");
  } else if (any_ack) {
    ImGui::TextColored(cancelled ? kAmber : kGreen, "trigger #%u: %zu frame(s)%s",
                       newest_id, captures, cancelled ? " (cancelled)" : "");
  } else {
    ImGui::TextDisabled("saves one frame per running camera");
  }
}

nlohmann::json ControlPanel::buildSaveParams() const {
  nlohmann::json p = {
      {"save_frames", save_frames_},
      {"output_dir", std::string(output_dir_)},
      {"prepend_timestamp_to_dir", prepend_ts_},
      {"batch_size", batch_size_},
      {"writer_threads", writer_threads_},
      // Not exposed as widgets: these come from the loaded `processing` config
      // and are passed straight through, so hitting Apply cannot silently
      // reset them to the server's defaults.
      {"backlog_max_bytes", msm_.savingConfig().backlog_max_bytes},
      {"disk_write_bytes_per_sec", msm_.savingConfig().disk_write_bytes_per_sec},
      {"allow_overcommit", msm_.savingConfig().allow_overcommit},
  };
  if (checkerboardMode()) {
    p["checkerboard_rows"] = cb_rows_;
    p["checkerboard_cols"] = cb_cols_;
    p["checkerboard_full_res_detection"] = cb_full_res_;
    p["checkerboard_num_threads"] = cb_threads_;
  }
  if (arucoMode()) {
    p["aruco_full_res_detection"] = aruco_full_res_;
    p["aruco_num_threads"] = aruco_threads_;
    p["aruco_corner_refine"] = aruco_corner_refine_;
  }
  if (triggerMode()) {
    p["trigger_skip_frames"] = trigger_skip_frames_;
  }
  return p;
}

}  // namespace telefacet::ui
