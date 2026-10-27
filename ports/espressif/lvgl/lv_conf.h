/**
 * LVGL config for CircuitPython on ESP32 P4/S31
 * Based on lv_conf_template.h with P4 optimizations
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_OS 0

/* Color depth: 16 for RGB565, 32 for ARGB8888 - set via display, but default 16 */
#define LV_COLOR_DEPTH 16

/* Memory: use CircuitPython heap via port_malloc */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "supervisor/memory.h"
#define LV_MEM_CUSTOM_ALLOC   port_malloc
#define LV_MEM_CUSTOM_FREE    port_free
#define LV_MEM_CUSTOM_REALLOC port_realloc

/* Tick: driven by esp_timer via lv_tick_inc */
#define LV_TICK_CUSTOM 0

/* Display and rendering */
#define LV_DISPLAY_RENDER_MODE_PARTIAL 1
#define LV_DISPLAY_RENDER_MODE_DIRECT 1
#define LV_DISPLAY_RENDER_MODE_FULL 1

/* Features needed for displayio */
#define LV_USE_DRAW_SW 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_OBJ 1

/* Font: use built-in */
#define LV_FONT_MONTSERRAT_14 1

/* Logging */
#define LV_USE_LOG 0

/* Enable required widgets for displayio */
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BUTTON 0
#define LV_USE_CANVAS 0

#endif
