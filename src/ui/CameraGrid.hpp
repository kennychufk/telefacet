#pragma once

// Manages a collection of CameraView windows inside an ImGui DockSpace,
// adding/removing views as the camera roster changes.

#include <memory>
#include <unordered_map>

#include "data/CameraStore.hpp"
#include "gl/YuvRenderer.hpp"
#include "ui/CameraView.hpp"

namespace telefacet::ui {

class CameraGrid {
 public:
  CameraGrid(data::CameraStore& store, gl::YuvRenderer& yuv_renderer);

  // Sync the set of CameraViews with the current roster snapshot. Cheap to
  // call every frame.
  void sync();

  // Per-frame upload + draw. Pass the dockspace ID so windows dock here.
  void render();

 private:
  data::CameraStore&   store_;
  gl::YuvRenderer&     yuv_renderer_;
  std::unordered_map<std::size_t, std::unique_ptr<CameraView>> views_;
};

}  // namespace telefacet::ui
