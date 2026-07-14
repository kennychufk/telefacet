#include "ui/CameraGrid.hpp"

#include <imgui.h>

#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

namespace telefacet::ui {

namespace {

// Optimal (cols, rows) for `count` tiles — ported from cameraStore.js
// getGridDimensions(): a hand-tuned table up to 16, landscape-biased above.
std::pair<int, int> gridDimensions(int count) {
  if (count <= 0)  return {0, 0};
  if (count == 1)  return {1, 1};
  if (count == 2)  return {2, 1};
  if (count <= 4)  return {2, 2};
  if (count <= 6)  return {3, 2};
  if (count <= 9)  return {3, 3};
  if (count <= 12) return {4, 3};
  if (count <= 16) return {4, 4};
  const int cols =
      static_cast<int>(std::ceil(std::sqrt(count * 1.5)));  // prefer landscape
  const int rows = static_cast<int>(std::ceil(static_cast<double>(count) / cols));
  return {cols, rows};
}

}  // namespace

CameraGrid::CameraGrid(data::CameraStore& store, gl::YuvRenderer& yuv_renderer)
    : store_(store), yuv_renderer_(yuv_renderer) {}

void CameraGrid::sync() {
  // Only streaming cameras get a window (mirrors the web's streaming-only grid;
  // also keeps idle-camera windows from stacking over the control panel).
  const auto roster = store_.snapshot();
  std::unordered_set<std::size_t> streaming;
  for (const auto& info : roster) {
    if (!info.streaming) continue;
    streaming.insert(info.global_id);
    if (!views_.count(info.global_id)) {
      views_[info.global_id] =
          std::make_unique<CameraView>(info.global_id, store_, yuv_renderer_);
    }
  }
  // Drop views for cameras that stopped streaming (or disappeared).
  for (auto it = views_.begin(); it != views_.end();) {
    if (!streaming.count(it->first)) it = views_.erase(it); else ++it;
  }
}

void CameraGrid::render() {
  sync();

  // Streaming views in stable global-id order (roster is sorted).
  std::vector<CameraView*> tiles;
  for (const auto& info : store_.snapshot()) {
    if (!info.streaming) continue;
    auto it = views_.find(info.global_id);
    if (it != views_.end()) tiles.push_back(it->second.get());
  }

  const int count = static_cast<int>(tiles.size());
  const auto [cols, rows] = gridDimensions(count);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 origin = vp->WorkPos;
  const ImVec2 size   = vp->WorkSize;
  const float cw = cols > 0 ? size.x / static_cast<float>(cols) : size.x;
  const float ch = rows > 0 ? size.y / static_cast<float>(rows) : size.y;

  for (int i = 0; i < count; ++i) {
    const int c = i % cols;
    const int r = i / cols;
    tiles[i]->uploadIfNew();
    tiles[i]->drawWindow(origin.x + c * cw, origin.y + r * ch, cw, ch);
  }
}

}  // namespace telefacet::ui
