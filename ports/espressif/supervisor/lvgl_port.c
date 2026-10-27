// LVGL port for espressif - no separate task, tick via esp_timer + background
#include "py/mpconfig.h"
#if CIRCUITPY_DISPLAYIO_LVGL
#include "lvgl.h"
#include "esp_timer.h"
#include "supervisor/shared/tick.h"

static esp_timer_handle_t lvgl_tick_timer = NULL;

static void lvgl_tick_cb(void *arg) {
    lv_tick_inc(5);
}

void displayio_lvgl_port_lock(void) {}
void displayio_lvgl_port_unlock(void) {}

void espressif_lvgl_init(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;
    lv_init();
    const esp_timer_create_args_t args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_create(&args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, 5000); // 5ms
}

void espressif_lvgl_background(void) {
    // Called from port_background_tick when auto_refresh
    lv_timer_handler();
}
#endif
