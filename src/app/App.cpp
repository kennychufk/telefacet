#include "app/App.hpp"

#include <glad/glad.h>
// glad.h must be included before glfw3.h
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace telefacet::app {

namespace {

void glfwErrorCallback(int code, const char* desc) {
  spdlog::error("glfw error {}: {}", code, desc);
}

}  // namespace

App::App(config::Config cfg, std::string config_path)
    : cfg_(std::move(cfg)), config_path_(std::move(config_path)) {}

App::~App() { shutdown(); }

void App::initWindow() {
  glfwSetErrorCallback(glfwErrorCallback);
  if (!glfwInit()) {
    throw std::runtime_error("glfwInit failed");
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

  window_ = glfwCreateWindow(1600, 1000, "telefacet", nullptr, nullptr);
  if (!window_) {
    glfwTerminate();
    throw std::runtime_error("glfwCreateWindow failed");
  }
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  // A YAML dropped on the window is loaded like one picked in the panel.
  glfwSetWindowUserPointer(window_, this);
  glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
    if (count > 0) static_cast<App*>(glfwGetWindowUserPointer(w))->requestConfig(paths[0]);
  });

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    throw std::runtime_error("gladLoadGLLoader failed");
  }
  spdlog::info("GL {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
}

void App::initImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init("#version 330");
}

void App::shutdown() {
  if (msm_) msm_->disconnectAll();
  msm_.reset();
  capture_saver_.reset();  // writes what is still queued
  grid_.reset();
  panel_.reset();
  yuv_renderer_.reset();
  if (window_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
  }
}

void App::renderDockSpace() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoNavFocus |
                           ImGuiWindowFlags_NoDocking;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##telefacet_root", nullptr, flags);
  ImGui::PopStyleVar(3);
  const ImGuiID dockspace_id = ImGui::GetID("telefacet_dockspace");
  ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                   ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::End();
}

void App::openSession() {
  msm_   = std::make_unique<net::MultiServerManager>(store_, cfg_);
  grid_  = std::make_unique<ui::CameraGrid>(store_, *yuv_renderer_);
  panel_ = std::make_unique<ui::ControlPanel>(store_, *msm_, *capture_saver_);
  panel_->setConfig(config_path_,
                    [this](std::string path) { requestConfig(std::move(path)); });

  // Every capture frame is saved as it arrives (on the receive thread; the
  // saver queues it for its own writer thread).
  msm_->setOnCaptureFrame(
      [this](std::size_t server_index, std::optional<std::size_t>,
             const net::CaptureFrame& cf,
             std::shared_ptr<const data::Capture> stored) {
        capture_saver_->save(cfg_.servers[server_index].address, cf.frame,
                             cf.result, stored ? stored->save : nullptr);
      });

  msm_->connectAll();
}

void App::requestConfig(std::string path) { pending_config_ = std::move(path); }

void App::loadPendingConfig() {
  if (pending_config_.empty()) return;
  const std::string path = std::exchange(pending_config_, {});

  // Parse before touching anything: a bad file keeps the current session.
  config::Config next;
  try {
    next = config::loadFromFile(path);
  } catch (const std::exception& e) {
    spdlog::error("could not load {}: {}", path, e.what());
    panel_->setConfigError(path + ": " + e.what());
    return;
  }
  spdlog::info("loading config {}: {} server(s)", path, next.servers.size());

  // Close the current connections first, and for good: each server has one
  // commander slot, and the new config's connection to the same server would
  // be refused (503) while the old one held it. stop() is synchronous and
  // turns off the automatic reconnection, so once it returns the server has
  // seen the close and nothing will reconnect. The new connection's discover
  // then names the new sensor, and the server releases the cameras of the old
  // one (cherupi-v4l2 protocol §4.1). The cameras are left as they are, as a
  // disconnect always leaves them: loading the same config again finds them
  // still running.
  const bool panel_visible = panel_->visible();
  msm_->disconnectAll();
  panel_.reset();
  grid_.reset();
  msm_.reset();
  store_.clear();

  if (next.captures.dir != capture_saver_->root())
    capture_saver_ = std::make_unique<data::CaptureSaver>(next.captures.dir,
                                                         next.captures.save);
  else
    capture_saver_->setEnabled(next.captures.save);

  cfg_         = std::move(next);
  config_path_ = path;
  openSession();
  if (!panel_visible) panel_->toggle();
}

int App::run() {
  initWindow();
  initImGui();

  yuv_renderer_ = std::make_unique<gl::YuvRenderer>();
  capture_saver_ = std::make_unique<data::CaptureSaver>(cfg_.captures.dir,
                                                       cfg_.captures.save);
  openSession();

  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();
    // Between frames: the views and the panel are rebuilt for the new config.
    loadPendingConfig();

    // Hotkeys: P toggles the control panel, V live vs capture view.
    if (!ImGui::GetIO().WantTextInput) {
      if (ImGui::IsKeyPressed(ImGuiKey_P, false)) panel_->toggle();
      if (ImGui::IsKeyPressed(ImGuiKey_V, false)) panel_->toggleCaptureView();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    renderDockSpace();
    panel_->draw();
    grid_->render(panel_->viewOptions());

    ImGui::Render();
    int fb_w, fb_h;
    glfwGetFramebufferSize(window_, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
  }

  return 0;
}

}  // namespace telefacet::app
