#version 330 core

// BT.601 full-range YCbCr (I420/YU12) → RGB.
// Y texture: GL_RED, width = bytes_per_line, height = image height.
// U/V textures: GL_RED, width = bytes_per_line/2, height = image height/2.
// WebGL uses bilinear interpolation on the half-resolution UV textures;
// GL_LINEAR on those textures achieves the same effect here.

in  vec2 v_texCoord;
out vec4 fragColor;

uniform sampler2D u_textureY;
uniform sampler2D u_textureU;
uniform sampler2D u_textureV;
uniform float u_widthRatio;  // = float(width) / float(bytesPerLine)

void main() {
  vec2 tc = vec2(v_texCoord.x * u_widthRatio, v_texCoord.y);
  float y = texture(u_textureY, tc).r;
  float u = texture(u_textureU, tc).r - 0.5;
  float v = texture(u_textureV, tc).r - 0.5;
  float r = y + 1.402 * v;
  float g = y - 0.344136 * u - 0.714136 * v;
  float b = y + 1.772 * u;
  fragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
