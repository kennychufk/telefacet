#pragma once

// Owns one camera's YUV420 plane textures, output FBO and the per-frame
// upload+draw path. Drawn inside an ImGui window.

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

class CameraView {
 public:
  CameraView(std::size_t global_id, data::CameraStore& store,
             gl::YuvRenderer& yuv_renderer);
  ~CameraView();

  CameraView(const CameraView&) = delete;
  CameraView& operator=(const CameraView&) = delete;

  std::size_t globalId() const { return global_id_; }

  // Pull the latest frame (if any) and upload it to the GL texture. Called
  // once per UI frame on the GL thread.
  void uploadIfNew();

  // Draw the ImGui window for this camera (image + overlays) pinned to the
  // grid cell (x, y, w, h) in screen coords. `show_focus_fom` adds the FoM
  // readout + history curve and the FoM-region outline. Returns false if the
  // window has been closed.
  bool drawWindow(float x, float y, float w, float h,
                  bool show_focus_fom = false);

 private:
  void ensureFbo(int w, int h);
  void ensureYuvTextures(int bytes_per_line, int height);

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
  bool          have_image_  = false;

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
