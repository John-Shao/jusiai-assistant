/*
 * lv_conf.h — LVGL v8.4 configuration for JuSi AI Assistant.
 *
 * Only the options that differ from LVGL's built-in defaults are listed here;
 * everything else is filled in by lvgl/src/lv_conf_internal.h. Resolved via
 * LV_CONF_INCLUDE_SIMPLE (see cmake/lvgl.cmake).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
 *   COLOR SETTINGS
 *====================*/

/* 32-bit colour. In memory each pixel is B,G,R,A — matches SDL_PIXELFORMAT_ARGB8888
 * on little-endian hosts, which the SDL3 display driver relies on. */
#define LV_COLOR_DEPTH 32

/*=========================
 *   MEMORY SETTINGS
 *=========================*/

/* Widget objects and styles only — the camera preview uses an externally
 * allocated canvas buffer, so this can stay small. */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (192U * 1024U)

/*====================
 *   HAL SETTINGS
 *====================*/

/* Default display refresh / input read periods (ms). */
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_INDEV_DEF_READ_PERIOD 16

/* The app advances LVGL's tick via lv_tick_inc() from the render loop. */
#define LV_TICK_CUSTOM 0

/*=======================
 *   FEATURE CONFIG
 *=======================*/

/* Widgets used by the UI. */
#define LV_USE_CANVAS 1   /* live camera preview */
#define LV_USE_IMG    1
#define LV_USE_LABEL  1
#define LV_USE_BTN    1
#define LV_USE_BAR    1
#define LV_USE_SPINNER 1  /* connecting indicator */
#define LV_USE_MSGBOX 1   /* error dialog */

/* Logging — routed to stderr by LVGL, kept at warnings and above. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Default theme. */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/*==================
 *   FONT USAGE
 *==================*/

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_28 1

#define LV_FONT_DEFAULT &lv_font_montserrat_16

#endif /* LV_CONF_H */
