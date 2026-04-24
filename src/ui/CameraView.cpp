#include "ui/CameraView.hpp"

#include <imgui.h>

#include <cstdio>

namespace telefacet::ui {

CameraView::CameraView(std::size_t global_id, data::CameraStore& store,
                       gl::YuvRenderer& yuv_renderer)
    : global_id_(global_id), store_(store), yuv_renderer_(yuv_renderer) {}

CameraView::~CameraView() {
  if (fbo_color_) glDeleteTextures(1, &fbo_color_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (y_tex_) glDeleteTextures(1, &y_tex_);
  if (u_tex_) glDeleteTextures(1, &u_tex_);
  if (v_tex_) glDeleteTextures(1, &v_tex_);
}

static GLuint makeRedTex(int w, int h, GLenum min_filter, GLenum mag_filter) {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  return tex;
}

void CameraView::ensureYuvTextures(int bpl, int h) {
  if (y_tex_ && y_tex_w_ == bpl && y_tex_h_ == h) return;
  if (y_tex_) glDeleteTextures(1, &y_tex_);
  if (u_tex_) glDeleteTextures(1, &u_tex_);
  if (v_tex_) glDeleteTextures(1, &v_tex_);
  y_tex_ = makeRedTex(bpl,     h,     GL_NEAREST, GL_NEAREST);
  u_tex_ = makeRedTex(bpl / 2, h / 2, GL_LINEAR,  GL_LINEAR);
  v_tex_ = makeRedTex(bpl / 2, h / 2, GL_LINEAR,  GL_LINEAR);
  y_tex_w_ = bpl;
  y_tex_h_ = h;
}

void CameraView::ensureFbo(int w, int h) {
  if (fbo_ && fbo_w_ == w && fbo_h_ == h) return;
  if (fbo_color_) glDeleteTextures(1, &fbo_color_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  glGenTextures(1, &fbo_color_);
  glBindTexture(GL_TEXTURE_2D, fbo_color_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         fbo_color_, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  fbo_w_ = w;
  fbo_h_ = h;
}

void CameraView::uploadIfNew() {
  auto frame = store_.consumeIfNew(global_id_, seen_seq_);
  if (!frame) return;

  frame_id_     = frame->frame_id;
  frames_saved_ = frame->frames_saved;
  header_only_  = frame->header_only;
  image_w_      = static_cast<int>(frame->width);
  image_h_      = static_cast<int>(frame->height);

  if (frame->header_only) {
    store_.pool().release(std::move(frame));
    return;
  }

  const int bpl      = static_cast<int>(frame->bytes_per_line);
  const int uvStride = bpl / 2;
  const int uvHeight = image_h_ / 2;
  const int ySize    = bpl * image_h_;
  const int uvSize   = uvStride * uvHeight;

  ensureYuvTextures(bpl, image_h_);
  ensureFbo(image_w_, image_h_);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  glBindTexture(GL_TEXTURE_2D, y_tex_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bpl, image_h_, GL_RED,
                  GL_UNSIGNED_BYTE, frame->data.data());

  glBindTexture(GL_TEXTURE_2D, u_tex_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvStride, uvHeight, GL_RED,
                  GL_UNSIGNED_BYTE, frame->data.data() + ySize);

  glBindTexture(GL_TEXTURE_2D, v_tex_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvStride, uvHeight, GL_RED,
                  GL_UNSIGNED_BYTE, frame->data.data() + ySize + uvSize);

  yuv_renderer_.render(y_tex_, u_tex_, v_tex_, image_w_, image_h_, bpl,
                       fbo_, image_w_, image_h_);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  have_image_ = true;
  store_.pool().release(std::move(frame));
}

bool CameraView::drawWindow() {
  auto* info = store_.find(global_id_);
  const std::string title =
      (info ? info->label : ("cam" + std::to_string(global_id_))) +
      "###cam" + std::to_string(global_id_);
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(640, 540), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(title.c_str(), &open)) {
    ImGui::End();
    return open;
  }

  auto* live = store_.stats(global_id_);
  const float fps = live ? live->fps.load() : 0.0f;
  const std::uint32_t fid =
      live ? live->last_frame_id.load() : frame_id_;
  const std::uint32_t fsv =
      live ? live->frames_saved.load() : frames_saved_;

  if (header_only_ || !have_image_) {
    // 2x2 stat grid (matches the JS header-only display).
    if (ImGui::BeginTable("stats", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("name");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(info ? info->label.c_str() : "?");
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("fps");
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", fps);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("frame_id");
      ImGui::TableNextColumn();
      ImGui::Text("%u", fid);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("frames_saved");
      ImGui::TableNextColumn();
      ImGui::Text("%u", fsv);
      ImGui::EndTable();
    }
  } else {
    // Image area: letterbox to preserve aspect ratio.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 1.0f) avail.y = 1.0f;
    const float img_aspect =
        static_cast<float>(image_w_) / static_cast<float>(image_h_);
    const float box_aspect = avail.x / avail.y;
    ImVec2 disp = avail;
    if (img_aspect > box_aspect) {
      disp.y = avail.x / img_aspect;
    } else {
      disp.x = avail.y * img_aspect;
    }
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 padding((avail.x - disp.x) * 0.5f, (avail.y - disp.y) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(cursor.x + padding.x, cursor.y + padding.y));
    ImGui::Image(reinterpret_cast<ImTextureID>(
                     static_cast<std::intptr_t>(fbo_color_)),
                 disp);

    // Overlays
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 white  = IM_COL32(255, 255, 255, 255);
    const ImU32 green  = IM_COL32(  0, 255,   0, 255);
    const ImU32 blue   = IM_COL32(120, 180, 255, 255);
    const ImU32 orange = IM_COL32(255, 165,   0, 255);
    char buf[64];
    if (info) {
      dl->AddText(ImVec2(cursor.x + 8, cursor.y + 6), white, info->label.c_str());
    }
    std::snprintf(buf, sizeof(buf), "%.1f fps", fps);
    dl->AddText(ImVec2(cursor.x + avail.x - 80, cursor.y + 6), green, buf);
    std::snprintf(buf, sizeof(buf), "frame %u", fid);
    dl->AddText(ImVec2(cursor.x + 8, cursor.y + avail.y - 36), blue, buf);
    std::snprintf(buf, sizeof(buf), "saved %u", fsv);
    dl->AddText(ImVec2(cursor.x + 8, cursor.y + avail.y - 18), orange, buf);
  }

  ImGui::End();
  return open;
}

}  // namespace telefacet::ui
