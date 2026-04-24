#include "ui/CameraGrid.hpp"

#include <imgui.h>

#include <unordered_set>

namespace telefacet::ui {

CameraGrid::CameraGrid(data::CameraStore& store, gl::YuvRenderer& yuv_renderer)
    : store_(store), yuv_renderer_(yuv_renderer) {}

void CameraGrid::sync() {
  const auto roster = store_.snapshot();
  std::unordered_set<std::size_t> seen;
  for (const auto& info : roster) {
    seen.insert(info.global_id);
    if (!views_.count(info.global_id)) {
      views_[info.global_id] =
          std::make_unique<CameraView>(info.global_id, store_, yuv_renderer_);
    }
  }
  // Remove views for cameras that have disappeared.
  for (auto it = views_.begin(); it != views_.end();) {
    if (!seen.count(it->first)) it = views_.erase(it); else ++it;
  }
}

void CameraGrid::render() {
  sync();
  for (auto& kv : views_) {
    kv.second->uploadIfNew();
    kv.second->drawWindow();
  }
}

}  // namespace telefacet::ui
