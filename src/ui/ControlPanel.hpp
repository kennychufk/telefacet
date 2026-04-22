#pragma once

// ImGui control window: connect/discover/configure/start/stop, per-camera
// stream toggles, save mode, header-only, frame-counts reset.

#include <string>

#include "data/CameraStore.hpp"
#include "net/MultiServerManager.hpp"

namespace telefacet::ui {

class ControlPanel {
 public:
  ControlPanel(data::CameraStore& store, net::MultiServerManager& msm);

  void draw();

  bool visible() const { return visible_; }
  void toggle()        { visible_ = !visible_; }

 private:
  data::CameraStore&        store_;
  net::MultiServerManager&  msm_;
  bool                      visible_ = true;
  bool                      header_only_ = false;
  int                       save_mode_idx_ = 0;  // 0 none, 1 buffer, 2 batch, 3 checkerboard
};

}  // namespace telefacet::ui
