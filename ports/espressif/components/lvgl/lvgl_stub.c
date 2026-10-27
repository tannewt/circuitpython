#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

struct _lv_display_t { int hor, ver; void *user_data; lv_color_format_t cf; };
static lv_display_t stub_disp;

void lv_init(void) {}
void lv_deinit(void) {}
void lv_tick_inc(uint32_t t) {(void)t;}
uint32_t lv_timer_handler(void) { return 10; }
lv_display_t* lv_display_create(int32_t hor, int32_t ver) { stub_disp.hor=hor; stub_disp.ver=ver; return &stub_disp; }
void lv_display_delete(lv_display_t *d) {(void)d;}
void lv_display_set_user_data(lv_display_t *d, void *u) { if(d) d->user_data=u; }
void* lv_display_get_user_data(lv_display_t *d) { return d ? d->user_data : NULL; }
void lv_display_set_color_format(lv_display_t *d, lv_color_format_t cf) { if(d) d->cf=cf; }
lv_color_format_t lv_display_get_color_format(lv_display_t *d) { return d ? d->cf : LV_COLOR_FORMAT_RGB565; }
void lv_display_set_buffers(lv_display_t *d, void *b1, void *b2, uint32_t s, lv_display_render_mode_t m) {(void)d;(void)b1;(void)b2;(void)s;(void)m;}
void lv_display_set_flush_cb(lv_display_t *d, void (*cb)(lv_display_t*, const lv_area_t*, uint8_t*)) {(void)d;(void)cb;}
void lv_display_flush_ready(lv_display_t *d) {(void)d;}
void lv_display_set_default(lv_display_t *d) {(void)d;}
lv_display_t* lv_display_get_default(void) { return &stub_disp; }
int32_t lv_display_get_horizontal_resolution(const lv_display_t *d) { return d ? d->hor : 320; }
int32_t lv_display_get_vertical_resolution(const lv_display_t *d) { return d ? d->ver : 240; }
int32_t lv_display_get_physical_horizontal_resolution(const lv_display_t *d) { return d ? d->hor : 320; }
void lv_refr_now(lv_display_t *d) {(void)d;}
lv_obj_t* lv_screen_active(void) { static lv_obj_t o; return &o; }
void lv_obj_set_style_bg_color(lv_obj_t *o, uint32_t c, uint32_t p) {(void)o;(void)c;(void)p;}
void lv_obj_set_style_bg_opa(lv_obj_t *o, uint8_t a, uint32_t p) {(void)o;(void)a;(void)p;}
void lv_obj_clear_flag(lv_obj_t *o, uint32_t f) {(void)o;(void)f;}
uint32_t lv_color_black(void) { return 0; }
uint8_t lv_color_format_get_size(lv_color_format_t cf) { switch(cf){case LV_COLOR_FORMAT_RGB565: return 2; case LV_COLOR_FORMAT_RGB888: return 3; case LV_COLOR_FORMAT_ARGB8888: return 4; default: return 2;} }
void* lv_malloc(size_t s) { return malloc(s); }
void lv_free(void *p) { free(p); }
