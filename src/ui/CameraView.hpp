#pragma once

// Owns one camera's YUV420 plane textures, output FBO and the per-frame
// upload+draw path. Drawn inside an ImGui window.
//
// The tile shows either the live stream or, in capture view, the camera's
// last trigger / stack capture (protocol §5.9) — a still, drawn into the FBO
// once. Live frames keep their readouts current meanwhile but are not drawn.

#include <glad/glad.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data/CameraStore.hpp"
#include "gl/YuvRenderer.hpp"

namespace telefacet::ui {

// Per-frame display options, shared by every tile.
struct ViewOptions {
  bool show_focus_fom  = false;  // FoM readout + history curve + region
  bool capture_view    = false;  // show each camera's last capture
  bool capture_overlay = false;  // last capture's corners over live frames
};

class CameraView {
 public:
  CameraView(std::size_t global_id, data::CameraStore& store,
             gl::YuvRenderer& yuv_renderer);
  ~CameraView();

  CameraView(const CameraView&) = delete;
  CameraView& operator=(const CameraView&) = delete;

  std::size_t globalId() const { return global_id_; }

  // Pull the latest frame (if any) and upload it to the GL texture — or, in
  // capture view, draw the camera's last capture if it changed. Called once
  // per UI frame on the GL thread, before drawWindow().
  void uploadIfNew(bool capture_view = false);

  // Draw the ImGui window for this camera (image + overlays) pinned to the
  // grid cell (x, y, w, h) in screen coords. Returns false if the window has
  // been closed.
  bool drawWindow(float x, float y, float w, float h,
                  const ViewOptions& opts = {});

 private:
  enum class FboContent { None, Live, Capture };

  void ensureFbo(int w, int h);
  void ensureYuvTextures(int bytes_per_line, int height);
  // Upload one I420 image to the plane textures and render it into the FBO.
  void renderYuv(const std::uint8_t* data, int width, int height, int bpl);
  // Draw a capture into the FBO: a trigger frame as YUV, a stack as gray.
  void renderCapture(const data::Capture& cap);

  std::size_t          global_id_;
  data::CameraStore&   store_;
  gl::YuvRenderer&     yuv_renderer_;
  std::uint64_t        seen_seq_ = 0;

  // YUV420 source textures (GL_RED, single channel).
  GLuint y_tex_     = 0;  // bytes_per_line × height
  GLuint u_tex_     = 0;  // (bytes_per_line/2) × (height/2)
  GLuint v_tex_     = 0;  // (bytes_per_line/2) × (height/2)
  int    y_tex_w_   = 0;  // bytes_per_line
  int    y_tex_h_   = 0;  // image height

  // Output: debayered RGB framebuffer.
  GLuint fbo_           = 0;
  GLuint fbo_color_     = 0;
  int    fbo_w_         = 0;
  int    fbo_h_         = 0;

  // Most recently displayed metadata (for overlays). Copied off the frame in
  // uploadIfNew() before the pooled buffer is released.
  int           image_w_    = 0;
  int           image_h_    = 0;
  std::uint32_t frame_id_   = 0;
  std::uint32_t frames_saved_ = 0;
  bool          header_only_ = false;
  // What the FBO holds. Back in live view after a capture, the tile shows
  // no image until the next live frame repaints it.
  FboContent    fbo_content_ = FboContent::None;
  std::uint64_t capture_seq_shown_ = 0;  // Capture::seq in the FBO
  std::vector<std::uint8_t> stack_i420_;  // scratch: stack as gray I420

  // v3/v5/v7 per-frame metadata for the lens/AF/FoM + corner overlays.
  std::uint64_t timestamp_us_      = 0;
  std::uint32_t frame_duration_us_ = 0;
  float         lens_position_     = NAN;
  std::uint8_t  af_state_          = 0xFF;
  std::int32_t  focus_fom_         = -1;
  data::FocusFomRegion focus_fom_region_;

  // The kFomHistory most recent FoM values (frames without one are skipped),
  // kept whether or not the FoM display is on so the curve is full the moment
  // it is switched on. Ring buffer: once full, the oldest is at fom_head_.
  static constexpr int kFomHistory = 80;
  std::array<float, kFomHistory> fom_hist_{};
  int fom_len_  = 0;
  int fom_head_ = 0;  // next write slot
  std::vector<data::CornerSet> corner_sets_;
  std::vector<data::ArucoMarker> aruco_markers_;
};

}  // namespace telefacet::ui
