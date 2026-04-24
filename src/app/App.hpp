#pragma once

// Top-level application: owns GLFW window, GL context, ImGui, the
// CameraStore, the MultiServerManager, and the UI controller objects.

#include <memory>
#include <string>

#include "config/ConfigLoader.hpp"
#include "data/CameraStore.hpp"
#include "gl/YuvRenderer.hpp"
#include "net/MultiServerManager.hpp"
#include "ui/CameraGrid.hpp"
#include "ui/ControlPanel.hpp"

struct GLFWwindow;

namespace telefacet::app {

class App {
 public:
  explicit App(config::Config cfg);
  ~App();

  int run();

 private:
  void initWindow();
  void initImGui();
  void shutdown();
  void renderDockSpace();

  config::Config         cfg_;
  GLFWwindow*            window_ = nullptr;
  data::CameraStore      store_;
  std::unique_ptr<net::MultiServerManager> msm_;
  std::unique_ptr<gl::YuvRenderer>         yuv_renderer_;
  std::unique_ptr<ui::CameraGrid>          grid_;
  std::unique_ptr<ui::ControlPanel>        panel_;
};

}  // namespace telefacet::app
