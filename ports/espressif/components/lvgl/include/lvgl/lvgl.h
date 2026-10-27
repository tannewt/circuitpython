#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Minimal LVGL stub for espressif LVGL displayio backend
// Provides enough API to compile displayio/lvgl and bus drivers
// Real LVGL will replace this via IDF component manager

typedef struct _lv_display_t lv_display_t;
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_area_t {
    int32_t x1, y1, x2, y2;
} lv_area_t;

typedef enum {
    LV_COLOR_FORMAT_UNKNOWN = 0,
    LV_COLOR_FORMAT_RGB565,
    LV_COLOR_FORMAT_RGB565_SWAPPED,
    LV_COLOR_FORMAT_RGB888,
    LV_COLOR_FORMAT_ARGB8888,
    LV_COLOR_FORMAT_XRGB8888,
    LV_COLOR_FORMAT_L8,
    LV_COLOR_FORMAT_AL88,
    LV_COLOR_FORMAT_I1,
} lv_color_format_t;

typedef enum {
    LV_DISPLAY_RENDER_MODE_PARTIAL,
    LV_DISPLAY_RENDER_MODE_DIRECT,
    LV_DISPLAY_RENDER_MODE_FULL,
} lv_display_render_mode_t;

typedef struct {
    int32_t x;
    int32_t y;
} lv_point_t;

// Stub functions - implemented in lvgl_stub.c
#ifdef __cplusplus
extern "C" {
#endif
void lv_init(void);
void lv_deinit(void);
void lv_tick_inc(uint32_t tick_period);
uint32_t lv_timer_handler(void);
lv_display_t* lv_display_create(int32_t hor_res, int32_t ver_res);
void lv_display_delete(lv_display_t *disp);
void lv_display_set_user_data(lv_display_t *disp, void *user_data);
void* lv_display_get_user_data(lv_display_t *disp);
void lv_display_set_color_format(lv_display_t *disp, lv_color_format_t cf);
lv_color_format_t lv_display_get_color_format(lv_display_t *disp);
void lv_display_set_buffers(lv_display_t *disp, void *buf1, void *buf2, uint32_t buf_size, lv_display_render_mode_t mode);
void lv_display_set_flush_cb(lv_display_t *disp, void (*flush_cb)(lv_display_t*, const lv_area_t*, uint8_t*));
void lv_display_flush_ready(lv_display_t *disp);
void lv_display_set_default(lv_display_t *disp);
lv_display_t* lv_display_get_default(void);
int32_t lv_display_get_horizontal_resolution(const lv_display_t *disp);
int32_t lv_display_get_vertical_resolution(const lv_display_t *disp);
int32_t lv_display_get_physical_horizontal_resolution(const lv_display_t *disp);
void lv_refr_now(lv_display_t *disp);
lv_obj_t* lv_screen_active(void);
void lv_obj_set_style_bg_color(lv_obj_t *obj, uint32_t color, uint32_t part);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, uint8_t opa, uint32_t part);
void lv_obj_clear_flag(lv_obj_t *obj, uint32_t flag);
uint32_t lv_color_black(void);
uint8_t lv_color_format_get_size(lv_color_format_t cf);
void* lv_malloc(size_t size);
void lv_free(void *ptr);
#define LV_OPA_COVER 255
#define LV_OBJ_FLAG_SCROLLABLE 0x01
#define LV_OBJ_FLAG_HIDDEN 0x02
#ifdef __cplusplus
}
#endif
