#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Grid.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#if defined(__APPLE__)
#include <OpenGL/gl3.h>  // defines OpenGL 3.0+ functions
#else
// Note: GLEW_STATIC is defined by CMake if the static lib is linked
#include <GL/glew.h>
#endif
#include <FL/gl.h>

#include <boost/asio.hpp>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "shader.hpp"

void timer_callback(void *data);

class VideoWindow : public Fl_Gl_Window {
 private:
  GLuint vao_;
  GLuint vbo_;
  GLuint ebo_;
  GLuint image_texture_;
  std::unique_ptr<telefacet::Shader> quad_shader_;
  int image_width_;
  int image_height_;
  constexpr static int kNumChannels = 3;
  constexpr static int kNumUnpackBuffers = 2;
  GLuint pbo_list_[kNumUnpackBuffers];
  int pbo_cursor_;

  constexpr static int kNumVideoFrames = 59;
  std::array<std::vector<GLubyte>, kNumVideoFrames> video_frame_data;
  int frame_id_;

 public:
  VideoWindow(int x, int y, int w, int h)
      : Fl_Gl_Window(x, y, w, h),
        image_width_(-1),
        image_height_(-1),
        pbo_cursor_(0),
        frame_id_(0) {
    mode(FL_RGB8 | FL_DOUBLE | FL_OPENGL3);
    std::cout << "swap interval is " << swap_interval() << std::endl;

    for (int i = 0; i < kNumVideoFrames; ++i) {
      std::stringstream filename_stream;
      filename_stream << "MultiDIC/"
                         "data1213/speckled/103/im_"
                      << std::setfill('0') << std::setw(5) << (i + 1) << ".jpg";
      std::string filename = filename_stream.str();
      Fl_JPEG_Image *example_image = new Fl_JPEG_Image(filename.c_str());
      if (example_image->fail()) {
        std::cout << "Error reading " << filename << std::endl;
      }
      image_width_ = example_image->w();
      image_height_ = example_image->h();
      int frame_size = image_width_ * image_height_ * kNumChannels;

      video_frame_data[i].resize(frame_size);
      memcpy(video_frame_data[i].data(), example_image->array, frame_size);
    }
  }

  void init_gl(void) {
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

  void init_app_gl(void) {
    GLuint vs;
    GLuint fs;
    int Mslv,
        mslv;  // major and minor version numbers of the shading language
    sscanf((char *)glGetString(GL_SHADING_LANGUAGE_VERSION), "%d.%d", &Mslv,
           &mslv);
    std::cout << "Shading Language Version=" << Mslv << "." << mslv
              << std::endl;
    const char *vert_code =
        "#version 330 core\n"
        "layout (location=0) in vec2 pos;\n"
        "layout (location=1) in vec2 inTexcoord;\n"
        "out vec2 texcoord;\n"
        "void main(){\n"
        "  gl_Position = vec4(pos, 0.0, 1.0);\n"
        "  texcoord = vec2(inTexcoord.x, inTexcoord.y);\n"
        "}";
    const char *frag_code =
        "#version 330 core\n"
        "in vec2 texcoord;\n"
        "out vec4 fragColor;\n"
        "uniform sampler2D imageTexture;\n"
        "void main(){\n"
        "  fragColor = texture(imageTexture, texcoord);\n"
        "}";
    quad_shader_.reset(new telefacet::Shader(vert_code, frag_code));

    GLfloat ndc_and_texcoord[] = {1.0f,  1.0f, 1.0f, -1.0f, -1.0f, -1.0f,
                                  -1.0f, 1.0f, 1.0f, 0.0f,  1.0f,  1.0f,
                                  0.0f,  1.0f, 0.0f, 0.0f};
    unsigned int indices[] = {0, 1, 3, 1, 2, 3};

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ndc_and_texcoord), ndc_and_texcoord,
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                 GL_STATIC_DRAW);

    // position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glEnableVertexAttribArray(0);
    // texcoord attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0,
                          (void *)(8 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glGenTextures(1, &image_texture_);
    glBindTexture(GL_TEXTURE_2D, image_texture_);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image_width_, image_height_, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, 0);
    glGenerateMipmap(GL_TEXTURE_2D);

    glGenBuffers(kNumUnpackBuffers, pbo_list_);
    for (int i = 0; i < kNumUnpackBuffers; ++i) {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[i]);
      glBufferData(GL_PIXEL_UNPACK_BUFFER,
                   image_width_ * image_height_ * kNumChannels, nullptr,
                   GL_STREAM_DRAW);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    quad_shader_->use();
    quad_shader_->set_int("imageTexture", 0);
  }

  void draw(void) FL_OVERRIDE {
    if (!valid()) {  // if screen changes (becoming invalid)
      int w = pixel_w();
      int h = pixel_h();
      glViewport(0, 0, w, h);

      float w_ratio = static_cast<float>(image_width_) / w;
      float h_ratio = static_cast<float>(image_height_) / h;

      float ndc_x_max = 1.0f;
      float ndc_y_max = 1.0f;
      if (h_ratio < w_ratio) {
        ndc_y_max = h_ratio / w_ratio;
      } else {
        ndc_x_max = w_ratio / h_ratio;
      }

      GLfloat ndc[] = {ndc_x_max,  ndc_y_max,  ndc_x_max,  -ndc_y_max,
                       -ndc_x_max, -ndc_y_max, -ndc_x_max, ndc_y_max};
      glBindBuffer(GL_ARRAY_BUFFER, vbo_);
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(ndc), ndc);
    }

    if (quad_shader_) {
      int cursor_next = (pbo_cursor_ + 1) % kNumUnpackBuffers;
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[pbo_cursor_]);
      int data_size = image_width_ * image_height_ * kNumChannels;
      // To avoid stalling:the previous data in PBO will be discarded and
      // glMapBuffer() returns a new allocated pointer immediately even if GPU
      // is still working with the previous data.
      // glBufferData(GL_PIXEL_UNPACK_BUFFER, data_size, 0, GL_STREAM_DRAW);
      GLubyte *pbo_ptr = (GLubyte *)glMapBufferRange(
          GL_PIXEL_UNPACK_BUFFER, 0, data_size,
          GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
      if (pbo_ptr != nullptr) {
        memcpy(pbo_ptr, video_frame_data[frame_id_].data(), data_size);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      } else {
        std::cout << "Failed to get pbo address" << std::endl;
      }

      frame_id_ = (frame_id_ + 1) % kNumVideoFrames;
      glBindTexture(GL_TEXTURE_2D, image_texture_);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_list_[cursor_next]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image_width_, image_height_,
                      GL_RGB, GL_UNSIGNED_BYTE, 0);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      pbo_cursor_ = cursor_next;

      glClearColor(0.08f, 0.8f, 0.8f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, image_texture_);

      quad_shader_->use();
      glBindVertexArray(vao_);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
    Fl_Gl_Window::draw();  // Draw FLTK child widgets.
  }
  int handle(int event) FL_OVERRIDE {
    static int first = 1;
    if (first && event == FL_SHOW && shown()) {
      first = 0;
      init_gl();
    }
    if (!quad_shader_ && event == FL_SHOW && shown()) {
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
};

void timer_callback(void *data) {
  VideoWindow *video_win = reinterpret_cast<VideoWindow *>(data);
  video_win->redraw();
  Fl::repeat_timeout(1.0 / 30.0, timer_callback, data);
}

int main(int argc, char **argv) {
  Fl::visual(FL_RGB8 | FL_DOUBLE);
  Fl_Window *win = new Fl_Window(340, 180);
  Fl_Grid *grid = new Fl_Grid(0, 0, win->w(), win->h());
  grid->layout(1, 2, 10, 10);
  grid->color(FL_WHITE);
  Fl_Box *b0 = new Fl_Box(0, 0, 0, 0, "B0");
  // Fl_Box *b1=new Fl_Box(0,0,0,0,"B1") ;
  VideoWindow *video_win = new VideoWindow(0, 0, 300, 300);
  video_win->end();
  grid->widget(b0, 0, 0);
  // grid->widget(b1,0,1);
  grid->widget(video_win, 0, 1);
  grid->end();
  win->end();
  win->resizable(grid);
  win->size_range(300, 100);

  win->show(argc, argv);
  Fl::add_timeout(1.0 / 30.0, timer_callback, video_win);
  return Fl::run();
}
