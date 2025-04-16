#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Grid.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <memory>
#if defined(__APPLE__)
#include <OpenGL/gl3.h>  // defines OpenGL 3.0+ functions
#else
// Note: GLEW_STATIC is defined by CMake if the static lib is linked
#include <GL/glew.h>
#endif
#include <FL/gl.h>

#include <cstdint>
#include <vector>

#include "response_header.hpp"
#include "shader.hpp"

void timer_callback(void* data);
class VideoWindow : public Fl_Gl_Window {
 public:
  VideoWindow(int x, int y, int w, int h);

  void init_gl(void);
  void init_app_gl(void);

  void draw(void) FL_OVERRIDE;
  int handle(int event) FL_OVERRIDE;
  void notify_image_dim(int width, int height);
  void update_pbo(ResponseHeader const& header, uint8_t const* src);

 private:
  GLuint vao_;
  GLuint vbo_;
  GLuint debayer_vao_;
  GLuint debayer_vbo_;  // constant ndc

  // Two cameras setup
  static const int kNumCameras = 2;
  GLuint debayer_fbo_[kNumCameras];
  GLuint debayer_texture_[kNumCameras];
  GLuint bayer_texture_[kNumCameras];

  std::unique_ptr<telefacet::Shader> debayer_shader_;
  std::unique_ptr<telefacet::Shader> screen_shader_;
  bool dirty_image_dim_;
  int image_width_;
  int image_height_;
  int viewport_width_;
  int viewport_height_;
  constexpr static int kNumChannels = 3;
  constexpr static int kNumUnpackBuffers = 2;
  GLuint pbo_list_[kNumUnpackBuffers];
  int pbo_cursor_;
  std::vector<std::vector<uint8_t>> latest_frame_;

  void resize_image();
  void process_camera_frame(int camera_index);
  void update_ndc_values();
};
