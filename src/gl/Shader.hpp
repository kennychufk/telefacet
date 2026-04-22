#pragma once

#include <glad/glad.h>

#include <string>

namespace telefacet::gl {

// Compile + link a vertex/fragment program. Throws std::runtime_error on
// failure with the GL info log embedded.
GLuint linkProgram(const char* vert_src, const char* frag_src);

}  // namespace telefacet::gl
