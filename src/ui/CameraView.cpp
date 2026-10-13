#include "ui/CameraView.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
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

  // Stash per-frame metadata for the draw pass before the pooled buffer is
  // released. corner_sets is strictly per-frame: this copy replaces the last,
  // so a frame with no detections clears the overlay.
  timestamp_us_      = frame->timestamp_us;
  frame_duration_us_ = frame->frame_duration_us;
  lens_position_     = frame->lens_position;
  af_state_          = frame->af_state;
  focus_fom_         = frame->focus_fom;
  focus_fom_region_  = frame->focus_fom_region;
  if (focus_fom_ >= 0) {
    fom_hist_[fom_head_] = static_cast<float>(focus_fom_);
    fom_head_ = (fom_head_ + 1) % kFomHistory;
    if (fom_len_ < kFomHistory) ++fom_len_;
  }
  corner_sets_       = frame->corner_sets;
  aruco_markers_     = frame->aruco_markers;

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

// Human-readable libcamera AfState (0=Idle, 1=Scanning, 2=Focused, 3=Failed).
static const char* afStateName(std::uint8_t s) {
  switch (s) {
    case 0:  return "idle";
    case 1:  return "focusing";
    case 2:  return "focused";
    case 3:  return "failed";
    default: return "n/a";
  }
}

static ImU32 afStateColor(std::uint8_t s) {
  switch (s) {
    case 0:  return IM_COL32(160, 160, 170, 255);  // idle — muted
    case 1:  return IM_COL32(255, 205,  60, 255);  // focusing — amber
    case 2:  return IM_COL32(  0, 255, 128, 255);  // focused — green
    case 3:  return IM_COL32(255,  92,  92, 255);  // failed — red
    default: return IM_COL32(140, 140, 150, 255);  // n/a
  }
}

// FoM history as a polyline inside [p0, p1], autoscaled to its own min..max
// (a flat history draws at mid-height). `offset` is the oldest sample's slot;
// the x axis always spans `cap` samples.
static void drawFomCurve(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
                         const float* hist, int len, int offset, int cap,
                         ImU32 col) {
  if (len < 2) return;
  float lo = hist[offset], hi = hist[offset];
  for (int i = 0; i < len; ++i) {
    const float v = hist[(offset + i) % cap];
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  ImVec2 pts[128];
  const int n = std::min(len, 128);
  for (int i = 0; i < n; ++i) {
    const float v = hist[(offset + len - n + i) % cap];
    const float t = hi > lo ? (v - lo) / (hi - lo) : 0.5f;
    // Newest sample on the right edge, so a short history grows leftwards.
    pts[i] = ImVec2(p0.x + (p1.x - p0.x) * (cap - n + i) / (cap - 1),
                    p1.y - (p1.y - p0.y) * t);
  }
  dl->AddPolyline(pts, n, col, ImDrawFlags_None, 1.5f);
}

bool CameraView::drawWindow(float x, float y, float w, float h,
                            bool show_focus_fom) {
  auto* info = store_.find(global_id_);
  const std::string title =
      (info ? info->label : ("cam" + std::to_string(global_id_))) +
      "###cam" + std::to_string(global_id_);
  bool open = true;
  ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
  // Pin each tile to its grid cell; NoMove/NoResize keeps the layout rigid
  // (the grid re-tiles from the streaming set every frame).
  if (!ImGui::Begin(title.c_str(), &open,
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
    ImGui::End();
    return open;
  }

  auto* live = store_.stats(global_id_);
  const float fps  = live ? live->fps.load() : 0.0f;
  const float sfps = live ? live->server_fps.load() : 0.0f;
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
      ImGui::Text("fps (cli/srv)");
      ImGui::TableNextColumn();
      ImGui::Text("%.1f / %.1f", fps, sfps);
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
      if (show_focus_fom) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("focus FoM");
        ImGui::TableNextColumn();
        if (focus_fom_ >= 0)
          ImGui::Text("%d", focus_fom_);
        else
          ImGui::TextUnformatted("--");
      }
      ImGui::EndTable();
    }
    if (show_focus_fom && fom_len_ >= 2) {
      // Fill the rest of the tile (at least 64 px tall).
      const float plot_h =
          std::max(64.0f, ImGui::GetContentRegionAvail().y - 4.0f);
      const int oldest = fom_len_ < kFomHistory ? 0 : fom_head_;
      ImGui::PlotLines("##fom", fom_hist_.data(), fom_len_, oldest,
                       "FoM, last 80 frames", FLT_MAX, FLT_MAX,
                       ImVec2(-1.0f, plot_h));
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
    const ImVec2 img_origin(cursor.x + padding.x, cursor.y + padding.y);
    ImGui::SetCursorScreenPos(img_origin);
    ImGui::Image(reinterpret_cast<ImTextureID>(
                     static_cast<std::intptr_t>(fbo_color_)),
                 disp);
    const bool hovered = ImGui::IsItemHovered();

    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 white  = IM_COL32(255, 255, 255, 255);
    const ImU32 green  = IM_COL32(  0, 255,   0, 255);
    const ImU32 blue   = IM_COL32(120, 180, 255, 255);
    const ImU32 orange = IM_COL32(255, 165,   0, 255);

    // Checkerboard corner overlay. Corner coords are full-frame Y-plane pixels
    // (map with width/height, never the padded stride), so scale by
    // disp/image and offset onto the letterboxed image rect. Color by set_id
    // for the 2x2 case; connect consecutive inner corners to trace scan order.
    if (image_w_ > 0 && image_h_ > 0) {
      const float sx = disp.x / static_cast<float>(image_w_);
      const float sy = disp.y / static_cast<float>(image_h_);
      static const ImU32 kSetColors[4] = {
          IM_COL32(  0, 255, 128, 255), IM_COL32(255,  90,  90, 255),
          IM_COL32( 90, 160, 255, 255), IM_COL32(255, 205,  60, 255)};
      const bool multi = corner_sets_.size() > 1;
      for (const auto& cs : corner_sets_) {
        if (!(cs.flags & 0x01)) continue;  // only full-frame coords are mappable
        const ImU32 col = multi ? kSetColors[cs.set_id & 0x03] : kSetColors[0];
        ImVec2 prev;
        bool have_prev = false;
        for (const auto& c : cs.corners) {
          const ImVec2 p(img_origin.x + c[0] * sx, img_origin.y + c[1] * sy);
          if (have_prev) dl->AddLine(prev, p, col, 1.0f);
          dl->AddCircleFilled(p, 3.0f, col);
          prev = p;
          have_prev = true;
        }
      }

      // ArUco marker overlay. Same coordinate mapping as the checkerboard path
      // (full-frame Y-plane pixels). Amber, distinct from the checkerboard
      // green: closed quad outline + corner dots + centered id label.
      const ImU32 amber = IM_COL32(255, 160, 0, 255);
      for (const auto& m : aruco_markers_) {
        if (!(m.flags & 0x01)) continue;  // only full-frame coords are mappable
        if (m.corners.size() < 4) continue;
        ImVec2 pts[4];
        ImVec2 centroid(0.0f, 0.0f);
        for (int i = 0; i < 4; ++i) {
          pts[i] = ImVec2(img_origin.x + m.corners[i][0] * sx,
                          img_origin.y + m.corners[i][1] * sy);
          centroid.x += pts[i].x * 0.25f;
          centroid.y += pts[i].y * 0.25f;
        }
        dl->AddPolyline(pts, 4, amber, ImDrawFlags_Closed, 1.5f);
        for (int i = 0; i < 4; ++i) dl->AddCircleFilled(pts[i], 3.0f, amber);
        char mbuf[32];
        std::snprintf(mbuf, sizeof(mbuf), "#%d", m.marker_id);
        // Center the label on the centroid, with a dark shadow for legibility.
        const ImVec2 tsz = ImGui::CalcTextSize(mbuf);
        const ImVec2 tpos(centroid.x - tsz.x * 0.5f, centroid.y - tsz.y * 0.5f);
        dl->AddText(ImVec2(tpos.x + 1, tpos.y + 1), IM_COL32(0, 0, 0, 200), mbuf);
        dl->AddText(tpos, amber, mbuf);
      }
    }

    // Text overlays: label (top-left), dual fps (top-right), frame/saved
    // (bottom-left).
    char buf[64];
    if (info) {
      dl->AddText(ImVec2(cursor.x + 8, cursor.y + 6), white, info->label.c_str());
    }
    std::snprintf(buf, sizeof(buf), "%.1f / %.1f fps", fps, sfps);
    const float fps_w = ImGui::CalcTextSize(buf).x;
    dl->AddText(ImVec2(cursor.x + avail.x - fps_w - 8, cursor.y + 6), green, buf);
    std::snprintf(buf, sizeof(buf), "frame %u", fid);
    dl->AddText(ImVec2(cursor.x + 8, cursor.y + avail.y - 36), blue, buf);
    std::snprintf(buf, sizeof(buf), "saved %u", fsv);
    dl->AddText(ImVec2(cursor.x + 8, cursor.y + avail.y - 18), orange, buf);

    // FocusFoM region (hover only): the fixed block the server's IPA
    // averages into focus_fom. Reported in full-resolution frame pixels, so
    // map through frame_width/height, which also covers subsampled streams.
    const auto& rg = focus_fom_region_;
    if (show_focus_fom && hovered && rg.valid) {
      const float rx = disp.x / static_cast<float>(rg.frame_width);
      const float ry = disp.y / static_cast<float>(rg.frame_height);
      const ImVec2 p0(img_origin.x + rg.x * rx, img_origin.y + rg.y * ry);
      const ImVec2 p1(img_origin.x + (rg.x + rg.width) * rx,
                      img_origin.y + (rg.y + rg.height) * ry);
      dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 140), 0.0f, 0, 1.0f);
    }

    // FoM readout + history curve, centred on the rendered frame (the
    // letterboxed image rect) and 45 % of its displayed width, on whenever
    // the FoM display is: a focus sweep is watched, not hovered.
    if (show_focus_fom) {
      char fbuf[24];
      if (focus_fom_ < 0)
        std::snprintf(fbuf, sizeof(fbuf), "FoM --");
      else
        std::snprintf(fbuf, sizeof(fbuf), "FoM %d", focus_fom_);
      const ImVec2 fsz = ImGui::CalcTextSize(fbuf);
      const float  pad = 8.0f;
      const float  cw  = disp.x * 0.45f;                       // curve width
      const float  ch  = std::min(cw / 3.0f, disp.y * 0.35f);  // curve height
      const ImVec2 centre(img_origin.x + disp.x * 0.5f,
                          img_origin.y + disp.y * 0.5f);
      const float  bw = cw + 2 * pad;
      const float  bh = fsz.y + 4.0f + ch + 2 * pad;
      const ImVec2 box0(centre.x - bw * 0.5f, centre.y - bh * 0.5f);
      const ImVec2 box1(box0.x + bw, box0.y + bh);
      dl->AddRectFilled(box0, box1, IM_COL32(0, 0, 0, 100), 4.0f);
      dl->AddText(ImVec2(box0.x + pad, box0.y + pad), white, fbuf);
      const ImVec2 c0(box0.x + pad, box0.y + pad + fsz.y + 4.0f);
      const int oldest = fom_len_ < kFomHistory ? 0 : fom_head_;
      drawFomCurve(dl, c0, ImVec2(c0.x + cw, c0.y + ch), fom_hist_.data(),
                   fom_len_, oldest, kFomHistory, green);
    }

    // Lens / AF hover overlay (bottom-right). Hidden when both are absent.
    if (hovered && !(std::isnan(lens_position_) && af_state_ == 0xFF)) {
      char lbuf[24];
      if (std::isnan(lens_position_))
        std::snprintf(lbuf, sizeof(lbuf), "-- D");
      else
        std::snprintf(lbuf, sizeof(lbuf), "%.2f D", lens_position_);
      const char* afname = afStateName(af_state_);
      const ImU32  afcol = afStateColor(af_state_);
      const float lw = ImGui::CalcTextSize(lbuf).x;
      const float aw = ImGui::CalcTextSize(afname).x;
      const float ty = cursor.y + avail.y - 18;
      const float tx = cursor.x + avail.x - (lw + 8 + aw) - 8;
      dl->AddText(ImVec2(tx, ty), white, lbuf);
      dl->AddText(ImVec2(tx + lw + 8, ty), afcol, afname);
    }
  }

  ImGui::End();
  return open;
}

}  // namespace telefacet::ui
