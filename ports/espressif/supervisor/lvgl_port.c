// LVGL port for espressif - no separate task, tick via esp_timer + background
#include "py/mpconfig.h"
#if CIRCUITPY_DISPLAYIO_LVGL
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "py/runtime.h"
#include "core/lv_global.h"
#include "shared-module/displayio/lvgl/lvgl_internal.h"
#include "supervisor/lvgl_port.h"
#include "supervisor/port_heap.h"

static const char *lvgl_tag = "LVGL";

static esp_timer_handle_t lvgl_tick_timer = NULL;

// LVGL's allocator hooks (LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM). Displays and
// their buffers outlive the VM, so they come from the port heap.
void lv_mem_init(void) {
}

void lv_mem_deinit(void) {
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    // Pools aren't supported with a custom allocator.
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size) {
    return port_malloc(size, false);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return port_realloc(p, new_size, false);
}

void lv_free_core(void *p) {
    port_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    // Not supported for custom allocators.
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

static void lvgl_tick_cb(void *arg) {
    lv_tick_inc(5);
}

void displayio_lvgl_port_lock(void) {
}
void displayio_lvgl_port_unlock(void) {
}

void displayio_lvgl_port_init(void) {
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;
    lv_init();
    // Show which rendering backends LVGL registered. With PPA enabled this includes
    // "ESP_PPA" and confirms the P4/S31 accelerator is available.
    lv_draw_unit_t *unit = LV_GLOBAL_DEFAULT()->draw_info.unit_head;
    while (unit != NULL) {
        ESP_LOGI(lvgl_tag, "draw unit: %s", unit->name);
        unit = unit->next;
    }
    const esp_timer_create_args_t args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_create(&args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, 5000); // 5ms
}

void espressif_lvgl_background(void) {
    // Called from port_background_tick when auto_refresh
    static uint32_t background_count;
    if ((background_count++ & 0x7FF) == 0) {
        ESP_LOGI(lvgl_tag, "background %lu default=%p", (unsigned long)background_count, lv_display_get_default());
    }
    lv_timer_handler();
}
#endif
