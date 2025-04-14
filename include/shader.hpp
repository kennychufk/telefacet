#pragma once
#include <string>

namespace telefacet {
class Shader {
 public:
  unsigned int ID;
  Shader(const char *vertex_shader_code, const char *fragment_shader_code);
  void use();
  void set_bool(const std::string &name, bool value) const;
  void set_int(const std::string &name, int value) const;
  void set_float(const std::string &name, float value) const;
  void set_vec2(const std::string &name, float v0, float v1) const;
  void set_vec3(const std::string &name, float v0, float v1, float v2) const;

 private:
  // utility function for checking shader compilation/linking errors.
  void checkCompileErrors(unsigned int shader, std::string type);
};
}  // namespace telefacet
