#include "video_window.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

void timer_callback(void *data) {
  VideoWindow *video_win = reinterpret_cast<VideoWindow *>(data);
  video_win->redraw();
  Fl::repeat_timeout(1.0 / 30.0, timer_callback, data);
}

VideoWindow::VideoWindow(int x, int y, int w, int h)
    : Fl_Gl_Window(x, y, w, h),
      dirty_image_dim_(false),
      image_width_(-1),
      image_height_(-1),
      viewport_width_(w),
      viewport_height_(h),
      pbo_cursor_(0) {
  mode(FL_RGB8 | FL_DOUBLE | FL_OPENGL3);

  // TODO: need to get image info during configuration stage
  image_width_ = 30;
  image_height_ = 30;

  latest_frame_.resize(kNumCameras);
  for (int i = 0; i < kNumCameras; ++i) {
    latest_frame_[i].resize(image_width_ * image_height_);
  }
}

void VideoWindow::init_gl(void) {
  make_current();
#ifndef __APPLE__
  GLenum err =
      glewInit();  // defines pters to functions of OpenGL V 1.2 and above
#ifdef FLTK_USE_WAYLAND
  // glewInit returns GLEW_ERROR_NO_GLX_DISPLAY with Wayland
  // see https://github.com/nigels-com/glew/issues/273
  if (fl_wl_display() && err == GLEW_ERROR_NO_GLX_DISPLAY) err = GLEW_OK;
#endif
  if (err)
    Fl::warning("glewInit() failed returning %u", err);
  else
    std::cout << "Using GLEW " << glewGetString(GLEW_VERSION) << std::endl;
#endif
  const uchar *glv = glGetString(GL_VERSION);
  std::cout << "GL_VERSION=%s" << glv << std::endl;
  redraw();
}

void VideoWindow::init_app_gl(void) {
  GLuint vs;
  GLuint fs;
  int Mslv,
      mslv;  // major and minor version numbers of the shading language
  sscanf((char *)glGetString(GL_SHADING_LANGUAGE_VERSION), "%d.%d", &Mslv,
         &mslv);
  std::cout << "Shading Language Version=" << Mslv << "." << mslv << std::endl;
  const char *debayer_vert_code =
      "#version 330 core\n"
      "layout (location=0) in vec2 pos;\n"
      "layout (location=1) in vec2 inTexcoord;\n"
      "uniform vec2 imageSize;\n"
      "out vec2 texcoord;\n"
      "out vec2 pixelCoordWithOffset;\n"
      "out vec4 xAdjacent;\n"
      "out vec4 yAdjacent;\n"
      "void main(){\n"
      "  gl_Position = vec4(pos, 0.0, 1.0);\n"
      "  texcoord = inTexcoord;\n"
      "  vec2 firstRed = vec2(1,1);\n"
      "  pixelCoordWithOffset = inTexcoord * imageSize + firstRed;\n"
      "  vec2 invSize = 1 / imageSize;\n"
      "  xAdjacent = texcoord.x + vec4(-2*invSize.x, -invSize.x, invSize.x, "
      "2*invSize.x);\n"
      "  yAdjacent = texcoord.y + vec4(-2*invSize.y, -invSize.y, invSize.y, "
      "2*invSize.y);\n"
      "}";
  const char *debayer_frag_code =
      "#version 330 core\n"
      "uniform vec3 awb_gain;\n"
      "uniform sampler2D imageTexture;\n"
      "in vec2 texcoord;\n"
      "in vec2 pixelCoordWithOffset;\n"
      "in vec4 xAdjacent;\n"
      "in vec4 yAdjacent;\n"
      "out vec4 fragColor;\n"
      "void main(){\n"
      "  float C = texture(imageTexture, texcoord).r;\n"
      "  const vec4 kC = vec4(4.0,  6.0,  5.0,  5.0) / 8.0;\n"
      "  // Determine which of four types of pixels we are on.\n"
      "  vec2 alternate = mod(pixelCoordWithOffset, 2.0);\n"
      "  vec4 Dvec = vec4(\n"
      "    texture(imageTexture, vec2(xAdjacent[1], yAdjacent[1])).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[1], yAdjacent[2])).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[2], yAdjacent[1])).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[2], yAdjacent[2])).r);\n"
      "  vec4 PATTERN = (kC.xyz * C).xyzz;\n"
      "  Dvec.xy += Dvec.zw;\n"
      "  Dvec.x  += Dvec.y;\n"  // Dvec.x=Dvec.x+Dvec.y+Dvec.z+Dvec.w
      "  vec4 value = vec4(\n"
      "    texture(imageTexture, vec2(texcoord.x, yAdjacent[0])).r,\n"
      "    texture(imageTexture, vec2(texcoord.x, yAdjacent[1])).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[0], texcoord.y)).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[1], texcoord.y)).r);\n"
      "  vec4 temp = vec4(\n"
      "    texture(imageTexture, vec2(texcoord.x, yAdjacent[3])).r,\n"
      "    texture(imageTexture, vec2(texcoord.x, yAdjacent[2])).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[3], texcoord.y)).r,\n"
      "    texture(imageTexture, vec2(xAdjacent[2], texcoord.y)).r);\n"
      "  const vec4 kA = vec4(-1.0, -1.5,  0.5, -1.0) / 8.0;\n"
      "  const vec4 kB = vec4( 2.0,  0.0,  0.0,  4.0) / 8.0;\n"
      "  const vec4 kD = vec4( 0.0,  2.0, -1.0, -1.0) / 8.0;\n"
      "  #define kE (kA.xywz)\n"
      "  #define kF (kB.xywz)\n"
      "  value += temp;\n"
      "  #define A (value[0])\n"
      "  #define B (value[1])\n"
      "  #define D (Dvec.x)\n"
      "  #define E (value[2])\n"
      "  #define F (value[3])\n"
      "  PATTERN.yzw += (kD.yz * D).xyy;\n"
      "  PATTERN += (kA.xyz * A).xyzx + (kE.xyw * E).xyxz;\n"
      "  PATTERN.xw  += kB.xw * B;\n"
      "  PATTERN.xz  += kF.xz * F;\n"
      "  fragColor.rgb = ((alternate.y < 1.0) ?\n"
      "      ((alternate.x < 1.0) ?\n"
      "          vec3(C, PATTERN.xy) :\n"
      "          vec3(PATTERN.z, C, PATTERN.w)) :\n"
      "      ((alternate.x < 1.0) ?\n"
      "          vec3(PATTERN.w, C, PATTERN.z) :\n"
      "          vec3(PATTERN.yx, C))) * awb_gain;\n"
      "}";
  debayer_shader_.reset(
      new telefacet::Shader(debayer_vert_code, debayer_frag_code));

  const char *screen_vert_code =
      "#version 330 core\n"
      "layout (location=0) in vec2 pos;\n"
      "layout (location=1) in vec2 inTexcoord;\n"
      "out vec2 texcoord;\n"
      "void main(){\n"
      "  gl_Position = vec4(pos, 0.0, 1.0);\n"
      "  texcoord = vec2(inTexcoord.x, inTexcoord.y);\n"
      "}";
  const char *screen_frag_code =
      "#version 330 core\n"
      "in vec2 texcoord;\n"
      "out vec4 fragColor;\n"
      "uniform sampler2D imageTexture;\n"
      "void main(){\n"
      "  fragColor = texture(imageTexture, texcoord);\n"
      "}";
  screen_shader_.reset(
      new telefacet::Shader(screen_vert_code, screen_frag_code));

  GLfloat ndc_and_texcoord[] = {-1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f,
                                -1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 1.0f,
                                0.0f,  0.0f, 0.0f,  1.0f,  1.0f, 1.0f,
                                0.0f,  0.0f, 1.0f,  1.0f,  1.0f, 0.0f};

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);

  glBindVertexArray(vao_);

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(ndc_and_texcoord), ndc_and_texcoord,
               GL_STATIC_DRAW);

  // position attribute
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
  glEnableVertexAttribArray(0);
  // texcoord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0,
                        (void *)(12 * sizeof(float)));
  glEnableVertexAttribArray(1);

  glGenVertexArrays(1, &debayer_vao_);
  glGenBuffers(1, &debayer_vbo_);

  glBindVertexArray(debayer_vao_);

  GLfloat screen_ndc_and_texcoord[] = {-1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f,
                                       -1.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 1.0f,
                                       0.0f,  1.0f, 0.0f,  0.0f,  1.0f, 0.0f,
                                       0.0f,  1.0f, 1.0f,  0.0f,  1.0f, 1.0f};
  glBindBuffer(GL_ARRAY_BUFFER, debayer_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(screen_ndc_and_texcoord),
               screen_ndc_and_texcoord, GL_STATIC_DRAW);

  // position attribute
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
  glEnableVertexAttribArray(0);
  // texcoord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0,
                        (void *)(12 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // Generate textures and framebuffers for both cameras
  glGenTextures(kNumCameras, bayer_texture_);
  glGenFramebuffers(kNumCameras, debayer_fbo_);
  glGenTextures(kNumCameras, debayer_texture_);

  // Set up textures and framebuffers for both cameras
  for (int i = 0; i < kNumCameras; ++i) {
    // Set up bayer texture
    glBindTexture(GL_TEXTURE_2D, bayer_texture_[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, image_width_, image_height_, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Set up debayer framebuffer and texture
    glBindFramebuffer(GL_FRAMEBUFFER, debayer_fbo_[i]);
    glBindTexture(GL_TEXTURE_2D, debayer_texture_[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image_width_, image_height_, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glGenerateMipmap(GL_TEXTURE_2D);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           debayer_texture_[i], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      std::cout << "ERROR::FRAMEBUFFER:: Framebuffer " << i
                << " is not complete!" << std::endl;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glGenBuffers(kNumUnpackBuffers, pbo_list_);
  for (int i = 0; i < kNumUnpackBuffers; ++i) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[i]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER,
                 image_width_ * image_height_ * kNumChannels, nullptr,
                 GL_STREAM_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  debayer_shader_->use();
  debayer_shader_->set_int("imageTexture", 0);
  debayer_shader_->set_vec2("imageSize", image_width_, image_height_);
  const float gain = 2.64;
  debayer_shader_->set_vec3("awb_gain", 2.2 * gain, 1.0 * gain, 2.26 * gain);

  screen_shader_->use();
  screen_shader_->set_int("imageTexture", 0);
}

void VideoWindow::update_ndc_values() {
  // For the two-column layout, we need different NDC values for each half
  float w_ratio = static_cast<float>(image_width_) / (viewport_width_ / 2);
  float h_ratio = static_cast<float>(image_height_) / viewport_height_;

  float ndc_x_max = 1.0f;
  float ndc_y_max = 1.0f;
  if (h_ratio < w_ratio) {
    ndc_y_max = h_ratio / w_ratio;
  } else {
    ndc_x_max = w_ratio / h_ratio;
  }

  GLfloat ndc[] = {-ndc_x_max, ndc_y_max,  -ndc_x_max, -ndc_y_max,
                   ndc_x_max,  -ndc_y_max, -ndc_x_max, ndc_y_max,
                   ndc_x_max,  -ndc_y_max, ndc_x_max,  ndc_y_max};

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(ndc), ndc);
}

void VideoWindow::draw(void) {
  if (dirty_image_dim_) {
    resize_image();
  }

  if (!valid() || dirty_image_dim_) {
    viewport_width_ = pixel_w();
    viewport_height_ = pixel_h();
    std::cout << "current Fl_Gl_Window size: " << viewport_width_ << ","
              << viewport_height_ << std::endl;

    update_ndc_values();
  }

  dirty_image_dim_ = false;

  // Process both camera frames
  if (debayer_shader_) {
    for (int i = 0; i < kNumCameras; ++i) {
      process_camera_frame(i);
    }
  }

  // Render to screen
  if (screen_shader_) {
    // Clear the entire screen
    glViewport(0, 0, viewport_width_, viewport_height_);
    glClearColor(0.98f, 0.98f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    screen_shader_->use();
    glBindVertexArray(vao_);
    glDisable(GL_DEPTH_TEST);

    // Left half - first camera
    glViewport(0, 0, viewport_width_ / 2, viewport_height_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, debayer_texture_[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Right half - second camera
    glViewport(viewport_width_ / 2, 0, viewport_width_ / 2, viewport_height_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, debayer_texture_[1]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
  }

  Fl_Gl_Window::draw();  // Draw FLTK child widgets.
}

void VideoWindow::process_camera_frame(int camera_index) {
  int cursor_next = (pbo_cursor_ + 1) % kNumUnpackBuffers;
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[cursor_next]);

  int data_size = image_width_ * image_height_ * sizeof(GLubyte);

  // Copy camera data to PBO
  if (!latest_frame_[camera_index].empty()) {
    GLubyte *pbo_ptr = (GLubyte *)glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER, 0, data_size,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (pbo_ptr != nullptr) {
      memcpy(pbo_ptr, latest_frame_[camera_index].data(), data_size);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    } else {
      std::cout << "Failed to get pbo address for camera " << camera_index
                << std::endl;
    }
  }

  pbo_cursor_ = cursor_next;

  // Update texture with PBO data
  glBindTexture(GL_TEXTURE_2D, bayer_texture_[camera_index]);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[pbo_cursor_]);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image_width_, image_height_, GL_RED,
                  GL_UNSIGNED_BYTE, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  // Debayer the frame
  glBindFramebuffer(GL_FRAMEBUFFER, debayer_fbo_[camera_index]);
  glViewport(0, 0, image_width_, image_height_);
  glClearColor(0.08f, 0.8f, 0.8f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, bayer_texture_[camera_index]);

  debayer_shader_->use();
  glBindVertexArray(debayer_vao_);
  glDisable(GL_DEPTH_TEST);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int VideoWindow::handle(int event) {
  static int first = 1;
  if (first && event == FL_SHOW && shown()) {
    first = 0;
    init_gl();
  }
  if (!debayer_shader_ && event == FL_SHOW && shown()) {
    init_app_gl();
  }

  // std::cout << "event: " << event << std::endl;
  int retval = Fl_Gl_Window::handle(event);
  if (retval) return retval;

  int x = Fl::event_x();
  int y = Fl::event_y();
  int num_clicks = Fl::event_clicks();
  // std::cout << num_clicks << ": " << x << "," << y << std::endl;
  // redraw();
  // std::cout << "push  Fl_Gl_Window::pixels_per_unit()=" <<
  // pixels_per_unit()
  //           << std::endl;
  // return 1; // return 1 if this event is handled by this method
  return retval;
}

void VideoWindow::update_pbo(ResponseHeader const &header, uint8_t const *src) {
  if (header.camera_id >= 0 && header.camera_id < kNumCameras) {
    memcpy(latest_frame_[header.camera_id].data(), src,
           image_width_ * image_height_);
  }
}

void VideoWindow::notify_image_dim(int width, int height) {
  dirty_image_dim_ = true;
  image_width_ = width;
  image_height_ = height;
}

void VideoWindow::resize_image() {
  // Resize the frame buffers for both cameras
  for (int i = 0; i < kNumCameras; ++i) {
    latest_frame_[i].resize(image_width_ * image_height_);

    // Update textures for this camera
    glBindTexture(GL_TEXTURE_2D, bayer_texture_[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, image_width_, image_height_, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, debayer_texture_[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image_width_, image_height_, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);
  }

  // Update PBOs
  for (int i = 0; i < kNumUnpackBuffers; ++i) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[i]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER,
                 image_width_ * image_height_ * kNumChannels, nullptr,
                 GL_STREAM_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  debayer_shader_->use();
  debayer_shader_->set_vec2("imageSize", image_width_, image_height_);
}
