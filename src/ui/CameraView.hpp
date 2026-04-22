#pragma once

// Owns one camera's GL_R8 texture, output FBO and the per-frame upload+draw
// path. Drawn inside an ImGui window.

#include <glad/glad.h>

#include <cstdint>
#include <memory>
#include <string>

#include "data/CameraStore.hpp"
#include "gl/Debayer.hpp"

namespace telefacet::ui {

class CameraView {
 public:
  CameraView(std::size_t global_id, data::CameraStore& store,
             gl::Debayer& debayer);
  ~CameraView();

  CameraView(const CameraView&) = delete;
  CameraView& operator=(const CameraView&) = delete;

  std::size_t globalId() const { return global_id_; }

  // Pull the latest frame (if any) and upload it to the GL texture. Called
  // once per UI frame on the GL thread.
  void uploadIfNew();

  // Draw the ImGui window for this camera (image + overlays). Returns false
  // if the window has been closed.
  bool drawWindow();

 private:
  void ensureFbo(int w, int h);
  void ensureSourceTexture(int bytes_per_line, int height);

  std::size_t          global_id_;
  data::CameraStore&   store_;
  gl::Debayer&         debayer_;
  std::uint64_t        seen_seq_ = 0;

  // Source: raw packed bytes uploaded as GL_R8.
  GLuint src_tex_       = 0;
  int    src_tex_w_     = 0;  // bytes_per_line
  int    src_tex_h_     = 0;  // image height

  // Output: debayered RGB framebuffer.
  GLuint fbo_           = 0;
  GLuint fbo_color_     = 0;
  int    fbo_w_         = 0;
  int    fbo_h_         = 0;

  // Most recently displayed metadata (for overlays).
  int           image_w_    = 0;
  int           image_h_    = 0;
  std::uint32_t frame_id_   = 0;
  std::uint32_t frames_saved_ = 0;
  bool          header_only_ = false;
  bool          have_image_  = false;
};

}  // namespace telefacet::ui
