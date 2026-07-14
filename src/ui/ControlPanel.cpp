#include "ui/ControlPanel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace telefacet::ui {

namespace {
const char* kSaveModes[] = {"none", "buffer", "batch", "checkerboard",
                            "checkerboard2x2"};
}

ControlPanel::ControlPanel(data::CameraStore& store,
                           net::MultiServerManager& msm)
    : store_(store), msm_(msm) {
  // Seed the save-mode selector from the loaded config so the panel reflects
  // what the YAML requested.
  const std::string& mode = msm_.configuredSaveMode();
  for (int i = 0; i < IM_ARRAYSIZE(kSaveModes); ++i) {
    if (mode == kSaveModes[i]) {
      save_mode_idx_ = i;
      break;
    }
  }
}

void ControlPanel::draw() {
  if (!visible_) return;
  if (!ImGui::Begin("telefacet", &visible_)) {
    ImGui::End();
    return;
  }

  // ----- Server status -----
  ImGui::SeparatorText("Servers");
  for (std::size_t i = 0; i < msm_.serverCount(); ++i) {
    auto* c = msm_.client(i);
    if (!c) continue;
    const bool ok = c->connected();
    ImGui::TextColored(ok ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                          : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       "[%s] #%zu %s", ok ? "OK" : "..", i,
                       c->address().c_str());
  }

  // ----- Lifecycle -----
  ImGui::SeparatorText("Lifecycle");
  if (ImGui::Button("Discover"))      { for (std::size_t i = 0; i < msm_.serverCount(); ++i)
                                          if (auto* c = msm_.client(i); c && c->connected()) c->discover(); }
  ImGui::SameLine();
  if (ImGui::Button("Configure"))     msm_.configureAll();
  ImGui::SameLine();
  if (ImGui::Button("Start cameras")) msm_.startAllCameras();
  ImGui::SameLine();
  if (ImGui::Button("Stop cameras"))  msm_.stopAllCameras();

  if (ImGui::Button("Reset frame counts")) msm_.resetFrameCountsAll();
  ImGui::SameLine();
  if (ImGui::Checkbox("Header-only mode", &header_only_)) {
    msm_.setHeaderOnlyAll(header_only_);
  }

  // ----- Save mode -----
  ImGui::SeparatorText("Save mode");
  ImGui::SetNextItemWidth(180);
  if (ImGui::Combo("##save_mode", &save_mode_idx_, kSaveModes,
                   IM_ARRAYSIZE(kSaveModes))) {
    msm_.setSaveModeAll(kSaveModes[save_mode_idx_],
                        msm_.savingParamsFromConfig());
  }
  ImGui::SameLine();
  if (ImGui::Button("Apply")) {
    msm_.setSaveModeAll(kSaveModes[save_mode_idx_],
                        msm_.savingParamsFromConfig());
  }

  // ----- Per-camera streams -----
  ImGui::SeparatorText("Streams");
  auto roster = store_.snapshot();
  if (roster.empty()) {
    ImGui::TextDisabled("(no cameras discovered yet)");
  } else {
    for (auto& info : roster) {
      auto* live = store_.stats(info.global_id);
      const float fps = live ? live->fps.load() : 0.0f;
      ImGui::PushID(static_cast<int>(info.global_id));
      bool streaming = info.streaming;
      if (ImGui::Checkbox("##stream", &streaming)) {
        if (streaming) msm_.startStream(info.global_id);
        else           msm_.stopStream(info.global_id);
      }
      ImGui::SameLine();
      ImGui::Text("%s  (server %zu / local %u)  %.1f fps",
                  info.label.c_str(), info.server_index,
                  info.local_camera_id, fps);
      ImGui::PopID();
    }
  }

  ImGui::End();
}

}  // namespace telefacet::ui
