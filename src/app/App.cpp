#include "app/App.hpp"

#include <glad/glad.h>
// glad.h must be included before glfw3.h
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <spdlog/spdlog.h>

#include <stdexcept>

namespace telefacet::app {

namespace {

void glfwErrorCallback(int code, const char* desc) {
  spdlog::error("glfw error {}: {}", code, desc);
}

}  // namespace

App::App(config::Config cfg) : cfg_(std::move(cfg)) {}

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
  grid_.reset();
  panel_.reset();
  debayer_.reset();
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

int App::run() {
  initWindow();
  initImGui();

  debayer_ = std::make_unique<gl::Debayer>();
  msm_     = std::make_unique<net::MultiServerManager>(store_, cfg_);
  grid_    = std::make_unique<ui::CameraGrid>(store_, *debayer_);
  panel_   = std::make_unique<ui::ControlPanel>(store_, *msm_);

  msm_->connectAll();

  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();

    // Hotkey: P toggles control panel.
    if (ImGui::IsKeyPressed(ImGuiKey_P, false) && !ImGui::GetIO().WantTextInput) {
      panel_->toggle();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    renderDockSpace();
    panel_->draw();
    grid_->render();

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
