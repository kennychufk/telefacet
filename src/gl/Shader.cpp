#include "gl/Shader.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace telefacet::gl {

namespace {

GLuint compile(GLenum type, const char* src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log(len > 0 ? len : 1);
    glGetShaderInfoLog(sh, static_cast<GLsizei>(log.size()), nullptr,
                       log.data());
    glDeleteShader(sh);
    throw std::runtime_error(std::string("shader compile failed: ") +
                             log.data());
  }
  return sh;
}

}  // namespace

GLuint linkProgram(const char* vert_src, const char* frag_src) {
  GLuint vs = compile(GL_VERTEX_SHADER, vert_src);
  GLuint fs = compile(GL_FRAGMENT_SHADER, frag_src);
  GLuint p  = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log(len > 0 ? len : 1);
    glGetProgramInfoLog(p, static_cast<GLsizei>(log.size()), nullptr,
                        log.data());
    glDeleteProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    throw std::runtime_error(std::string("program link failed: ") + log.data());
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  return p;
}

}  // namespace telefacet::gl
