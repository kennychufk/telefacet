#pragma once

// GL program that converts YUV420/I420 frames to RGB using three single-channel
// textures (Y, U, V). Owns the program and uniform locations only — textures
// and the output FBO are managed by the caller (CameraView).

#include <glad/glad.h>

namespace telefacet::gl {

class YuvRenderer {
 public:
  YuvRenderer();
  ~YuvRenderer();

  YuvRenderer(const YuvRenderer&) = delete;
  YuvRenderer& operator=(const YuvRenderer&) = delete;

  // Render one YUV420 frame to target_fbo.
  //   y_tex/u_tex/v_tex  — GL_RED textures holding the three I420 planes.
  //   width/height        — image dimensions in pixels.
  //   bytes_per_line      — Y-plane stride (may exceed width due to padding).
  //   target_fbo          — destination framebuffer (0 = default backbuffer).
  //   target_w/target_h   — viewport dimensions of the target.
  void render(GLuint y_tex, GLuint u_tex, GLuint v_tex,
              int width, int height, int bytes_per_line,
              GLuint target_fbo, int target_w, int target_h);

 private:
  GLuint program_         = 0;
  GLuint vao_             = 0;
  GLint  loc_tex_y_       = -1;
  GLint  loc_tex_u_       = -1;
  GLint  loc_tex_v_       = -1;
  GLint  loc_width_ratio_ = -1;
};

}  // namespace telefacet::gl
