#pragma once

// Top-level application: owns GLFW window, GL context, ImGui, the
// CameraStore, the MultiServerManager, and the UI controller objects.
//
// Another YAML can be loaded while the app runs (the panel's Config section,
// or a file dropped on the window) — e.g. to move the same Pi from an IMX477
// to an IMX708 config. The server has one commander slot, so the current
// connections are closed before the new config's open (see loadPendingConfig).

#include <memory>
#include <string>

#include "config/ConfigLoader.hpp"
#include "data/CameraStore.hpp"
#include "data/CaptureSaver.hpp"
#include "gl/YuvRenderer.hpp"
#include "net/MultiServerManager.hpp"
#include "ui/CameraGrid.hpp"
#include "ui/ControlPanel.hpp"

struct GLFWwindow;

namespace telefacet::app {

class App {
 public:
  // `config_path` is the file `cfg` came from, shown in the panel.
  App(config::Config cfg, std::string config_path);
  ~App();

  int run();

 private:
  void initWindow();
  void initImGui();
  void shutdown();
  void renderDockSpace();
  // msm_, grid_ and panel_ for cfg_, and connect.
  void openSession();
  // Load another YAML; applied by loadPendingConfig() between frames.
  void requestConfig(std::string path);
  void loadPendingConfig();

  config::Config         cfg_;
  std::string            config_path_;
  std::string            pending_config_;  // empty ⇒ no load requested
  GLFWwindow*            window_ = nullptr;
  data::CameraStore      store_;
  std::unique_ptr<net::MultiServerManager> msm_;
  // Writes every trigger / stack capture to this machine (the server keeps
  // none). Outlives msm_'s callbacks: destroyed after the disconnect.
  std::unique_ptr<data::CaptureSaver>      capture_saver_;
  std::unique_ptr<gl::YuvRenderer>         yuv_renderer_;
  std::unique_ptr<ui::CameraGrid>          grid_;
  std::unique_ptr<ui::ControlPanel>        panel_;
};

}  // namespace telefacet::app
