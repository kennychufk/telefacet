#include "gl/Debayer.hpp"

#include "gl/Shader.hpp"
#include "telefacet/EmbeddedShaders.hpp"

namespace telefacet::gl {

Debayer::Debayer() {
  program_ = linkProgram(shaders::kDebayerVert, shaders::kDebayerFrag);
  loc_tex_size_       = glGetUniformLocation(program_, "u_textureSize");
  loc_bytes_per_line_ = glGetUniformLocation(program_, "u_bytesPerLine");
  loc_awb_            = glGetUniformLocation(program_, "u_awbGains");
  loc_tex_            = glGetUniformLocation(program_, "u_texture");
  glGenVertexArrays(1, &vao_);
}

Debayer::~Debayer() {
  if (program_) glDeleteProgram(program_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
}

void Debayer::render(GLuint src_tex, int width, int height, int bytes_per_line,
                     float awb_r, float awb_g, float awb_b, GLuint target_fbo,
                     int target_w, int target_h) {
  glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
  glViewport(0, 0, target_w, target_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);

  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glUniform1i(loc_tex_, 0);
  glUniform2f(loc_tex_size_, static_cast<float>(width),
              static_cast<float>(height));
  glUniform1f(loc_bytes_per_line_, static_cast<float>(bytes_per_line));
  glUniform3f(loc_awb_, awb_r, awb_g, awb_b);

  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
}

}  // namespace telefacet::gl
