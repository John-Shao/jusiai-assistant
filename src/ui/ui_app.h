// LVGL screen for the AI assistant — an immersive video-call layout: the
// camera fills the whole window, with a floating status pill and circular
// controls (microphone, call / hang-up, camera) overlaid on top.
#pragma once

#include <cstdint>
#include <vector>

#include "app_config.h"
#include "core/assistant_controller.h"
#include "lvgl.h"
#include "ui/lv_display.h"

namespace jusiai {

class UiApp {
 public:
  UiApp(AssistantController* controller, const AppConfig& config);

  // Create the window and build the screen. Returns false on failure.
  bool init();

  // Run the render loop until the window is closed.
  void run();

 private:
  void build_ui();
  void build_scale_luts();           // build the cover-scale lookup tables
  void refresh_status();             // sync widgets with the controller snapshot
  void refresh_preview();            // scale the latest camera frame to the canvas
  void render_mic_icon(bool muted);  // draw the microphone glyph
  void render_cam_icon(bool off);    // draw the camera glyph

  static void on_primary_clicked(lv_event_t* e);  // call / hang-up
  static void on_mute_clicked(lv_event_t* e);     // microphone
  static void on_camera_clicked(lv_event_t* e);   // camera on/off

  AssistantController* controller_;
  AppConfig config_;
  LvDisplay display_;

  // Widgets.
  lv_obj_t* canvas_ = nullptr;       // full-window camera preview
  lv_obj_t* title_label_ = nullptr;
  lv_obj_t* status_pill_ = nullptr;  // floating status / hint pill
  lv_obj_t* call_btn_ = nullptr;     // centre: call / hang-up
  lv_obj_t* call_label_ = nullptr;
  lv_obj_t* mic_btn_ = nullptr;      // left: microphone mute
  lv_obj_t* mic_icon_ = nullptr;
  lv_obj_t* cam_btn_ = nullptr;      // right: camera on/off
  lv_obj_t* cam_icon_ = nullptr;

  std::vector<std::uint8_t> mic_icon_buf_;  // microphone glyph canvas buffer
  std::vector<std::uint8_t> cam_icon_buf_;  // camera glyph canvas buffer
  std::vector<lv_color_t> canvas_buf_;      // full-window preview buffer
  std::vector<int> sx_lut_;                 // cover-scale column map
  std::vector<int> sy_lut_;                 // cover-scale row map

  int cam_w_ = 0;   // camera frame size
  int cam_h_ = 0;
  int view_w_ = 0;  // canvas / window size
  int view_h_ = 0;
  bool cam_dark_ = false;  // canvas currently shows the "camera off" fill

  // Change tracking so widgets are only touched when something changed.
  std::uint64_t preview_version_ = 0;
  bool snapshot_valid_ = false;
  UiSnapshot last_;
};

}  // namespace jusiai
