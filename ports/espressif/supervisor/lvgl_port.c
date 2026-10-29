// LVGL port for espressif - no separate task, tick via esp_timer + background
#include "py/mpconfig.h"
#if CIRCUITPY_DISPLAYIO_LVGL
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "py/runtime.h"
#include "core/lv_global.h"
#include "shared-bindings/displayio/__init__.h"
#if CIRCUITPY_BUSDISPLAY
#include "shared-bindings/busdisplay/BusDisplay.h"
#endif
#if CIRCUITPY_FRAMEBUFFERIO
#include "shared-bindings/framebufferio/FramebufferDisplay.h"
#endif
#include "shared-module/displayio/lvgl/lvgl_internal.h"
#include "supervisor/lvgl_port.h"
#include "supervisor/port_heap.h"
#include "supervisor/shared/tick.h"

// The LVGL compositor doesn't keep the legacy port-level display registry, so the
// primary display is tracked here. It is rooted so it survives garbage collection.
MP_REGISTER_ROOT_POINTER(mp_obj_t displayio_lvgl_primary_display);

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

void espressif_lvgl_init(void) {
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

static bool is_lvgl_display(mp_obj_t obj) {
    #if CIRCUITPY_BUSDISPLAY
    if (mp_obj_is_type(obj, &busdisplay_busdisplay_type)) {
        return true;
    }
    #endif
    #if CIRCUITPY_FRAMEBUFFERIO
    if (mp_obj_is_type(obj, &framebufferio_framebufferdisplay_type)) {
        return true;
    }
    #endif
    return false;
}

mp_obj_t common_hal_displayio_get_primary_display(void) {
    mp_obj_t display = MP_STATE_VM(displayio_lvgl_primary_display);
    if (display == MP_OBJ_NULL) {
        return mp_const_none;
    }
    return display;
}

void common_hal_displayio_set_primary_display(mp_obj_t new_primary_display) {
    if (new_primary_display == mp_const_none) {
        MP_STATE_VM(displayio_lvgl_primary_display) = mp_const_none;
        return;
    }
    if (!is_lvgl_display(new_primary_display)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q or %q, not %q"),
            MP_QSTR_Display,
            MP_QSTR_Display, MP_QSTR_AnyDisplay, MP_QSTR_None);
    }
    MP_STATE_VM(displayio_lvgl_primary_display) = new_primary_display;
}

void common_hal_displayio_auto_primary_display(void) {
    // Displays are created by user code and aren't registered with the port, so there
    // is nothing to select automatically.
}

void common_hal_displayio_release_displays(void) {
    MP_STATE_VM(displayio_lvgl_primary_display) = mp_const_none;
}
#endif
