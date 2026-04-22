#pragma once

// Single GL program that unpacks SRGGB10P and bilinear-debayers it. Owns the
// program and uniform locations only — input texture and output FBO are
// passed in by the caller (CameraView).

#include <glad/glad.h>

namespace telefacet::gl {

class Debayer {
 public:
  Debayer();
  ~Debayer();

  Debayer(const Debayer&) = delete;
  Debayer& operator=(const Debayer&) = delete;

  // Render one debayered frame.
  //   src_tex   — GL_R8 texture sized (bytes_per_line, height) holding raw
  //               packed bytes.
  //   width/height        — image dimensions in pixels (not bytes).
  //   bytes_per_line      — texture width in bytes (= ceil(width*5/4) for
  //                         SRGGB10P).
  //   awb_r/g/b           — per-channel gain.
  //   target_fbo          — destination framebuffer (0 = default backbuffer).
  //   target_w/target_h   — viewport dimensions of the target.
  void render(GLuint src_tex, int width, int height, int bytes_per_line,
              float awb_r, float awb_g, float awb_b, GLuint target_fbo,
              int target_w, int target_h);

 private:
  GLuint program_ = 0;
  GLuint vao_     = 0;  // empty VAO required by GL 3.3 core for the
                        // gl_VertexID-based fullscreen triangle
  GLint  loc_tex_size_ = -1;
  GLint  loc_bytes_per_line_ = -1;
  GLint  loc_awb_ = -1;
  GLint  loc_tex_ = -1;
};

}  // namespace telefacet::gl
