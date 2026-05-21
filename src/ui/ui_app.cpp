#include "ui/ui_app.h"

#include <chrono>
#include <thread>

#include <algorithm>
#include <string>

#include "i18n.h"
#include "log.h"
#include "ui/ui_fonts.h"

namespace jusiai {
namespace {

bool is_active(AssistantState s) {
  return s != AssistantState::Idle && s != AssistantState::Error;
}

constexpr int kIconSize = 26;  // control-button glyph canvas size (px)

// Create a flat circular button with a soft drop shadow.
lv_obj_t* make_round_button(lv_obj_t* parent, int diameter) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_size(b, diameter, diameter);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0xf2f3f5), 0);
  lv_obj_set_style_shadow_width(b, 14, 0);
  lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
  lv_obj_set_style_shadow_color(b, lv_color_black(), 0);
  return b;
}

}  // namespace

UiApp::UiApp(AssistantController* controller, const AppConfig& config)
    : controller_(controller), config_(config) {}

bool UiApp::init() {
  if (!display_.init("JuSi AI Assistant", config_.window_width,
                     config_.window_height, config_.fullscreen)) {
    return false;
  }
  build_ui();
  refresh_status();
  return true;
}

void UiApp::build_ui() {
  const int W = display_.width();
  const int H = display_.height();
  view_w_ = W;
  view_h_ = H;
  cam_w_ = config_.camera_width > 0 ? config_.camera_width : 640;
  cam_h_ = config_.camera_height > 0 ? config_.camera_height : 480;

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // --- Full-window camera preview ----------------------------------------
  canvas_buf_.assign(static_cast<std::size_t>(W) * H, lv_color_hex(0x000000));
  canvas_ = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvas_, canvas_buf_.data(), W, H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(canvas_, 0, 0);
  build_scale_luts();

  // --- Top: title --------------------------------------------------------
  title_label_ = lv_label_create(scr);
  lv_label_set_text(title_label_, tr(Msg::AppTitle));
  lv_obj_set_style_text_font(title_label_, &lv_font_zh_20, 0);
  lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
  lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 18);

  // --- Floating status / hint pill ---------------------------------------
  status_pill_ = lv_label_create(scr);
  lv_label_set_recolor(status_pill_, true);
  lv_obj_set_style_bg_color(status_pill_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(status_pill_, LV_OPA_50, 0);
  lv_obj_set_style_radius(status_pill_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(status_pill_, 22, 0);
  lv_obj_set_style_pad_ver(status_pill_, 11, 0);
  lv_obj_set_style_text_color(status_pill_, lv_color_white(), 0);
  lv_obj_set_style_text_font(status_pill_, &lv_font_zh_16, 0);
  lv_obj_set_style_text_align(status_pill_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(status_pill_, "");
  lv_obj_align(status_pill_, LV_ALIGN_BOTTOM_MID, 0, -150);

  // --- Bottom: floating circular controls --------------------------------
  lv_obj_t* bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);  // transparent layout container
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 38, 0);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -46);

  // Microphone (left).
  mic_btn_ = make_round_button(bar, 70);
  lv_obj_add_event_cb(mic_btn_, &UiApp::on_mute_clicked, LV_EVENT_CLICKED, this);
  mic_icon_ = lv_canvas_create(mic_btn_);
  mic_icon_buf_.assign(
      LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kIconSize, kIconSize), 0);
  lv_canvas_set_buffer(mic_icon_, mic_icon_buf_.data(), kIconSize, kIconSize,
                       LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_obj_center(mic_icon_);

  // Call / hang-up (centre, larger).
  call_btn_ = make_round_button(bar, 84);
  lv_obj_add_event_cb(call_btn_, &UiApp::on_primary_clicked, LV_EVENT_CLICKED,
                      this);
  call_label_ = lv_label_create(call_btn_);
  lv_obj_set_style_text_font(call_label_, &lv_font_montserrat_28, 0);
  lv_label_set_text(call_label_, LV_SYMBOL_CALL);
  lv_obj_center(call_label_);

  // Camera (right). LVGL has no camera symbol, so the glyph is hand-drawn on
  // a canvas — matching the microphone button.
  cam_btn_ = make_round_button(bar, 70);
  lv_obj_add_event_cb(cam_btn_, &UiApp::on_camera_clicked, LV_EVENT_CLICKED,
                      this);
  cam_icon_ = lv_canvas_create(cam_btn_);
  cam_icon_buf_.assign(
      LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kIconSize, kIconSize), 0);
  lv_canvas_set_buffer(cam_icon_, cam_icon_buf_.data(), kIconSize, kIconSize,
                       LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_obj_center(cam_icon_);

  // --- Bottom: AI-content disclaimer -------------------------------------
  lv_obj_t* disclaimer = lv_label_create(scr);
  lv_label_set_text(disclaimer, tr(Msg::Disclaimer));
  lv_obj_set_style_text_font(disclaimer, &lv_font_zh_14, 0);
  lv_obj_set_style_text_color(disclaimer, lv_color_hex(0xc4c8cf), 0);
  lv_obj_set_style_text_opa(disclaimer, LV_OPA_70, 0);
  lv_obj_align(disclaimer, LV_ALIGN_BOTTOM_MID, 0, -14);

  render_mic_icon(false);
  render_cam_icon(false);
}

// Build the cover-scale lookup tables: the camera frame is scaled to fill the
// whole window, with the overflow cropped (preserving aspect ratio).
void UiApp::build_scale_luts() {
  const int W = view_w_;
  const int H = view_h_;
  const int w = cam_w_;
  const int h = cam_h_;
  if (W <= 0 || H <= 0 || w <= 0 || h <= 0) return;

  int scaled_w, scaled_h;
  if (static_cast<long>(W) * h >= static_cast<long>(H) * w) {
    scaled_w = W;
    scaled_h = static_cast<int>(static_cast<long>(h) * W / w);
  } else {
    scaled_h = H;
    scaled_w = static_cast<int>(static_cast<long>(w) * H / h);
  }
  const int ox = (scaled_w - W) / 2;
  const int oy = (scaled_h - H) / 2;

  sx_lut_.resize(W);
  sy_lut_.resize(H);
  for (int dx = 0; dx < W; ++dx) {
    int sx = static_cast<int>(static_cast<long>(dx + ox) * w / scaled_w);
    sx_lut_[dx] = std::clamp(sx, 0, w - 1);
  }
  for (int dy = 0; dy < H; ++dy) {
    int sy = static_cast<int>(static_cast<long>(dy + oy) * h / scaled_h);
    sy_lut_[dy] = std::clamp(sy, 0, h - 1);
  }
}

void UiApp::refresh_status() {
  const UiSnapshot s = controller_->snapshot();

  const bool changed =
      !snapshot_valid_ || s.state != last_.state || s.status != last_.status ||
      s.detail != last_.detail || s.mic_muted != last_.mic_muted ||
      s.cam_muted != last_.cam_muted;
  if (!changed) return;
  last_ = s;
  snapshot_valid_ = true;

  // Status pill. In the idle state the pill is purely a call-to-action, so it
  // shows only the hint ("Tap the call button to begin") without the "Ready"
  // status word. Every other state shows the status, optionally followed by a
  // dimmed hint (e.g. "Connecting...,  ...").
  std::string pill;
  if (s.state == AssistantState::Idle) {
    pill = s.detail.empty() ? s.status : s.detail;
  } else {
    pill = s.status;
    if (!s.detail.empty()) pill += ",  #9aa0a8 " + s.detail + "#";
  }
  lv_label_set_text(status_pill_, pill.c_str());

  // Call / hang-up button — green to start, red to hang up, grey while stopping.
  if (s.state == AssistantState::Stopping) {
    lv_obj_set_style_bg_color(call_btn_, lv_color_hex(0x3a3f47), 0);
    lv_obj_add_state(call_btn_, LV_STATE_DISABLED);
  } else if (is_active(s.state)) {
    lv_obj_set_style_bg_color(call_btn_, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_clear_state(call_btn_, LV_STATE_DISABLED);
  } else {
    lv_obj_set_style_bg_color(call_btn_, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_clear_state(call_btn_, LV_STATE_DISABLED);
  }
  lv_obj_set_style_text_color(call_label_, lv_color_white(), 0);

  // Microphone + camera buttons — usable only during a live call.
  const bool in_call = s.state == AssistantState::InCall;

  if (in_call) {
    lv_obj_clear_state(mic_btn_, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(
        mic_btn_,
        s.mic_muted ? lv_palette_main(LV_PALETTE_RED) : lv_color_hex(0xf2f3f5),
        0);
  } else {
    lv_obj_set_style_bg_color(mic_btn_, lv_color_hex(0xf2f3f5), 0);
    lv_obj_add_state(mic_btn_, LV_STATE_DISABLED);
  }
  render_mic_icon(in_call && s.mic_muted);

  if (in_call) {
    lv_obj_clear_state(cam_btn_, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(
        cam_btn_,
        s.cam_muted ? lv_color_hex(0x3a3f47) : lv_color_hex(0xf2f3f5), 0);
  } else {
    lv_obj_set_style_bg_color(cam_btn_, lv_color_hex(0xf2f3f5), 0);
    lv_obj_add_state(cam_btn_, LV_STATE_DISABLED);
  }
  render_cam_icon(in_call && s.cam_muted);
}

void UiApp::refresh_preview() {
  // Camera off — paint the canvas dark once and stop pulling frames.
  if (controller_->snapshot().cam_muted) {
    if (!cam_dark_) {
      std::fill(canvas_buf_.begin(), canvas_buf_.end(), lv_color_hex(0x0a0c10));
      lv_obj_invalidate(canvas_);
      cam_dark_ = true;
    }
    return;
  }
  cam_dark_ = false;

  std::vector<std::uint8_t> rgba;
  int w = 0;
  int h = 0;
  if (!controller_->preview()->fetch(preview_version_, rgba, w, h)) return;
  if (w != cam_w_ || h != cam_h_) return;
  if (sx_lut_.empty() || sy_lut_.empty()) return;

  // Cover-scale the RGBA camera frame into the canvas (RGBA -> LVGL colour).
  for (int dy = 0; dy < view_h_; ++dy) {
    const std::uint8_t* srow =
        rgba.data() + static_cast<std::size_t>(sy_lut_[dy]) * w * 4;
    lv_color_t* drow =
        canvas_buf_.data() + static_cast<std::size_t>(dy) * view_w_;
    for (int dx = 0; dx < view_w_; ++dx) {
      const std::uint8_t* px = srow + static_cast<std::size_t>(sx_lut_[dx]) * 4;
      lv_color_t c;
      c.ch.red = px[0];
      c.ch.green = px[1];
      c.ch.blue = px[2];
      c.ch.alpha = 0xff;
      drow[dx] = c;
    }
  }
  lv_obj_invalidate(canvas_);
}

// Draw a microphone glyph (capsule head + cradle arc + stem + base) onto the
// canvas. LVGL has no built-in microphone symbol, so it is drawn directly.
void UiApp::render_mic_icon(bool muted) {
  if (!mic_icon_) return;
  // Dark glyph on the light button; white glyph on the red (muted) button.
  const lv_color_t fg = muted ? lv_color_white() : lv_color_hex(0x1c2128);

  lv_canvas_fill_bg(mic_icon_, lv_color_black(), LV_OPA_TRANSP);

  // Head — a vertical capsule.
  lv_draw_rect_dsc_t head;
  lv_draw_rect_dsc_init(&head);
  head.bg_color = fg;
  head.bg_opa = LV_OPA_COVER;
  head.radius = LV_RADIUS_CIRCLE;
  lv_canvas_draw_rect(mic_icon_, 9, 3, 8, 13, &head);

  // Cradle — a U-shaped arc hugging the lower half of the head.
  lv_draw_arc_dsc_t cradle;
  lv_draw_arc_dsc_init(&cradle);
  cradle.color = fg;
  cradle.width = 2;
  cradle.opa = LV_OPA_COVER;
  lv_canvas_draw_arc(mic_icon_, 13, 10, 8, 20, 160, &cradle);

  // Stem and base.
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = fg;
  line.width = 2;
  line.opa = LV_OPA_COVER;
  lv_point_t stem[2] = {{13, 18}, {13, 22}};
  lv_canvas_draw_line(mic_icon_, stem, 2, &line);
  lv_point_t base[2] = {{8, 22}, {18, 22}};
  lv_canvas_draw_line(mic_icon_, base, 2, &line);

  // Muted — a diagonal slash across the glyph.
  if (muted) {
    lv_draw_line_dsc_t slash;
    lv_draw_line_dsc_init(&slash);
    slash.color = lv_color_black();
    slash.width = 3;
    slash.opa = LV_OPA_COVER;
    lv_point_t sl[2] = {{4, 4}, {22, 22}};
    lv_canvas_draw_line(mic_icon_, sl, 2, &slash);
  }

  lv_obj_invalidate(mic_icon_);
}

// Draw a camera glyph (body + viewfinder bump + lens ring) onto the canvas.
// LVGL has no built-in camera symbol, so it is drawn directly — matching the
// hand-drawn microphone button. `off` adds a slash and switches to a white
// glyph for the darkened "camera off" button.
void UiApp::render_cam_icon(bool off) {
  if (!cam_icon_) return;
  const lv_color_t fg = off ? lv_color_white() : lv_color_hex(0x1c2128);

  lv_canvas_fill_bg(cam_icon_, lv_color_black(), LV_OPA_TRANSP);

  // Body — a rounded-rectangle outline.
  lv_draw_rect_dsc_t body;
  lv_draw_rect_dsc_init(&body);
  body.bg_opa = LV_OPA_TRANSP;
  body.radius = 3;
  body.border_color = fg;
  body.border_width = 2;
  body.border_opa = LV_OPA_COVER;
  lv_canvas_draw_rect(cam_icon_, 2, 8, 22, 15, &body);

  // Viewfinder bump — a small filled tab straddling the body's top edge.
  lv_draw_rect_dsc_t bump;
  lv_draw_rect_dsc_init(&bump);
  bump.bg_color = fg;
  bump.bg_opa = LV_OPA_COVER;
  bump.radius = 1;
  lv_canvas_draw_rect(cam_icon_, 8, 3, 10, 6, &bump);

  // Lens — a ring centred in the body.
  lv_draw_rect_dsc_t lens;
  lv_draw_rect_dsc_init(&lens);
  lens.bg_opa = LV_OPA_TRANSP;
  lens.radius = LV_RADIUS_CIRCLE;
  lens.border_color = fg;
  lens.border_width = 2;
  lens.border_opa = LV_OPA_COVER;
  lv_canvas_draw_rect(cam_icon_, 7, 10, 12, 12, &lens);

  // Camera off — a diagonal slash across the glyph.
  if (off) {
    lv_draw_line_dsc_t slash;
    lv_draw_line_dsc_init(&slash);
    slash.color = fg;
    slash.width = 2;
    slash.opa = LV_OPA_COVER;
    lv_point_t sl[2] = {{4, 4}, {22, 22}};
    lv_canvas_draw_line(cam_icon_, sl, 2, &slash);
  }

  lv_obj_invalidate(cam_icon_);
}

void UiApp::run() {
  LOG_INFO("ui: entering render loop");
  while (!display_.quit_requested()) {
    display_.poll_events();
    refresh_status();
    refresh_preview();
    display_.render();
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  LOG_INFO("ui: window closed");
}

void UiApp::on_primary_clicked(lv_event_t* e) {
  auto* self = static_cast<UiApp*>(lv_event_get_user_data(e));
  const AssistantState st = self->controller_->snapshot().state;
  if (st == AssistantState::Idle || st == AssistantState::Error) {
    self->controller_->request_start();
  } else if (st != AssistantState::Stopping) {
    self->controller_->request_stop();
  }
}

void UiApp::on_mute_clicked(lv_event_t* e) {
  auto* self = static_cast<UiApp*>(lv_event_get_user_data(e));
  if (self->controller_->snapshot().state == AssistantState::InCall) {
    self->controller_->request_toggle_mute();
  }
}

void UiApp::on_camera_clicked(lv_event_t* e) {
  auto* self = static_cast<UiApp*>(lv_event_get_user_data(e));
  if (self->controller_->snapshot().state == AssistantState::InCall) {
    self->controller_->request_toggle_camera();
  }
}

}  // namespace jusiai
