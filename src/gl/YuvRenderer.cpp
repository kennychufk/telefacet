#include "gl/YuvRenderer.hpp"

#include "gl/Shader.hpp"
#include "telefacet/EmbeddedShaders.hpp"

namespace telefacet::gl {

YuvRenderer::YuvRenderer() {
  program_         = linkProgram(shaders::kDebayerVert, shaders::kDebayerFrag);
  loc_tex_y_       = glGetUniformLocation(program_, "u_textureY");
  loc_tex_u_       = glGetUniformLocation(program_, "u_textureU");
  loc_tex_v_       = glGetUniformLocation(program_, "u_textureV");
  loc_width_ratio_ = glGetUniformLocation(program_, "u_widthRatio");
  glGenVertexArrays(1, &vao_);
}

YuvRenderer::~YuvRenderer() {
  if (program_) glDeleteProgram(program_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
}

void YuvRenderer::render(GLuint y_tex, GLuint u_tex, GLuint v_tex,
                         int width, int height, int bytes_per_line,
                         GLuint target_fbo, int target_w, int target_h) {
  glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
  glViewport(0, 0, target_w, target_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);

  glUseProgram(program_);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, y_tex);
  glUniform1i(loc_tex_y_, 0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, u_tex);
  glUniform1i(loc_tex_u_, 1);

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, v_tex);
  glUniform1i(loc_tex_v_, 2);

  glUniform1f(loc_width_ratio_,
              static_cast<float>(width) / static_cast<float>(bytes_per_line));

  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);

  glActiveTexture(GL_TEXTURE0);
}

}  // namespace telefacet::gl
