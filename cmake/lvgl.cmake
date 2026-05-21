# Fetch and build LVGL v8.4. The configuration header (lv_conf.h) lives at the
# project root; LV_CONF_INCLUDE_SIMPLE makes LVGL resolve it via the include
# path rather than a relative "../../lv_conf.h" lookup.
include(FetchContent)

if(NOT TARGET lvgl)
  FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v8.4.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(lvgl)

  # Point LVGL at our lv_conf.h (project root).
  target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
  target_include_directories(lvgl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
