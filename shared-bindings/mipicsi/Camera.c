// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "py/mphal.h"
#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/mipicsi/__init__.h"
#include "shared-bindings/mipicsi/Camera.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

static void check_for_deinit(mipicsi_camera_obj_t *self) {
    if (common_hal_mipicsi_camera_deinited(self)) {
        raise_deinited_error();
    }
}

//| class Camera:
//|     """Receive frames from a camera sensor on a MIPI CSI-2 interface
//|
//|     The camera sensor must be configured separately (typically over I2C) to
//|     transmit frames of the chosen resolution and format. The MIPI CSI-2
//|     receiver hardware has dedicated pins, so no pins are configured here.
//|     """
//|
//|     def __init__(
//|         self,
//|         *,
//|         h_res: int,
//|         v_res: int,
//|         lane_bit_rate_mbps: int,
//|         pixel_format: PixelFormat = PixelFormat.RGB565,
//|         data_lanes: int = 2,
//|         framebuffer_count: int = 2,
//|     ) -> None:
//|         """
//|         Configure and initialize the MIPI CSI-2 camera receiver
//|
//|         :param h_res: The horizontal resolution of the frames the sensor transmits, in pixels per line
//|         :param v_res: The vertical resolution of the frames the sensor transmits, in lines per frame
//|         :param lane_bit_rate_mbps: The MIPI lane bit rate, in megabits per second. This must match the rate the sensor transmits at and is determined by the sensor's configuration.
//|         :param pixel_format: The format of the frames the sensor transmits
//|         :param data_lanes: The number of MIPI data lanes the sensor uses, 1 or 2
//|         :param framebuffer_count: The number of framebuffers used to hold captured frames. While a taken frame is still in use, new frames are captured into a private buffer; otherwise, frames are captured into a free framebuffer.
//|         """
//|
static mp_obj_t mipicsi_camera_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_h_res, ARG_v_res, ARG_lane_bit_rate_mbps, ARG_pixel_format, ARG_data_lanes, ARG_framebuffer_count, NUM_ARGS };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_h_res, MP_ARG_INT | MP_ARG_KW_ONLY | MP_ARG_REQUIRED },
        { MP_QSTR_v_res, MP_ARG_INT | MP_ARG_KW_ONLY | MP_ARG_REQUIRED },
        { MP_QSTR_lane_bit_rate_mbps, MP_ARG_INT | MP_ARG_KW_ONLY | MP_ARG_REQUIRED },
        { MP_QSTR_pixel_format, MP_ARG_OBJ | MP_ARG_KW_ONLY, { .u_obj = MP_ROM_PTR((void *)&pixel_format_RGB565_obj) } },
        { MP_QSTR_data_lanes, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2 } },
        { MP_QSTR_framebuffer_count, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2 } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    MP_STATIC_ASSERT(MP_ARRAY_SIZE(allowed_args) == NUM_ARGS);
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const mp_int_t h_res = mp_arg_validate_int_min(args[ARG_h_res].u_int, 1, MP_QSTR_h_res);
    const mp_int_t v_res = mp_arg_validate_int_min(args[ARG_v_res].u_int, 1, MP_QSTR_v_res);
    const mp_int_t lane_bit_rate_mbps = mp_arg_validate_int_min(args[ARG_lane_bit_rate_mbps].u_int, 1, MP_QSTR_lane_bit_rate_mbps);
    mipicsi_pixel_format_t pixel_format = validate_pixel_format(args[ARG_pixel_format].u_obj, MP_QSTR_pixel_format);
    const mp_int_t data_lanes = mp_arg_validate_int_range(args[ARG_data_lanes].u_int, 1, 2, MP_QSTR_data_lanes);
    const mp_int_t framebuffer_count = mp_arg_validate_int_range(args[ARG_framebuffer_count].u_int, 1, 4, MP_QSTR_framebuffer_count);

    mipicsi_camera_obj_t *self = mp_obj_malloc_with_finaliser(mipicsi_camera_obj_t, &mipicsi_camera_type);
    common_hal_mipicsi_camera_construct(
        self,
        h_res,
        v_res,
        pixel_format,
        data_lanes,
        lane_bit_rate_mbps,
        framebuffer_count);
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Deinitialises the camera receiver and releases all memory resources for reuse."""
//|         ...
//|
static mp_obj_t mipicsi_camera_deinit(mp_obj_t self_in) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_mipicsi_camera_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipicsi_camera_deinit_obj, mipicsi_camera_deinit);

//|     def __enter__(self) -> Camera:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes the hardware when exiting a context. See
//|         :ref:`lifetime-and-contextmanagers` for more info."""
//|         ...
//|

//|     frame_available: bool
//|     """True if a captured frame is available, False otherwise"""
//|
static mp_obj_t mipicsi_camera_frame_available_get(mp_obj_t self_in) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_mipicsi_camera_available(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipicsi_camera_frame_available_get_obj, mipicsi_camera_frame_available_get);

MP_PROPERTY_GETTER(mipicsi_camera_frame_available_obj,
    (mp_obj_t)&mipicsi_camera_frame_available_get_obj);

//|     def take(self, timeout: Optional[float] = 0.25) -> Optional[displayio.Bitmap | ReadableBuffer]:
//|         """Return the newest captured frame, waiting up to ``timeout`` seconds for one to arrive.
//|
//|         In the case of timeout, `None` is returned.
//|
//|         If `pixel_format` is `PixelFormat.GRAYSCALE` or `PixelFormat.RGB565`, the returned value is a
//|         read-only `displayio.Bitmap`. Otherwise, the returned value is a read-only `memoryview` of
//|         the raw frame data.
//|
//|         The returned frame remains valid until the next call to ``take``. While the returned frame
//|         is in use, newly captured frames go into a private buffer and are discarded."""
//|
static mp_obj_t mipicsi_camera_take(size_t n_args, const mp_obj_t *args) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_float_t timeout = n_args < 2 ? MICROPY_FLOAT_CONST(0.25) : mp_obj_get_float(args[1]);
    check_for_deinit(self);
    const void *result = common_hal_mipicsi_camera_take(self, (int)MICROPY_FLOAT_C_FUN(round)(timeout * 1000));
    if (!result) {
        return mp_const_none;
    }
    mipicsi_pixel_format_t format = common_hal_mipicsi_camera_get_pixel_format(self);
    if (format == MIPICSI_FORMAT_RGB565 || format == MIPICSI_FORMAT_GRAYSCALE) {
        int width = common_hal_mipicsi_camera_get_width(self);
        int height = common_hal_mipicsi_camera_get_height(self);
        displayio_bitmap_t *bitmap = m_new_obj(displayio_bitmap_t);
        bitmap->base.type = &displayio_bitmap_type;
        common_hal_displayio_bitmap_construct_from_buffer(bitmap, width, height, (format == MIPICSI_FORMAT_RGB565) ? 16 : 8, (uint32_t *)(void *)result, true);
        return bitmap;
    }
    mp_int_t bits_per_pixel = common_hal_mipicsi_camera_get_bits_per_pixel(self);
    mp_int_t width = common_hal_mipicsi_camera_get_width(self);
    mp_int_t height = common_hal_mipicsi_camera_get_height(self);
    size_t len = (size_t)(width * height * bits_per_pixel + 7) / 8;
    return mp_obj_new_memoryview('B', len, (void *)result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mipicsi_camera_take_obj, 1, 2, mipicsi_camera_take);

//|     pixel_format: PixelFormat
//|     """The pixel format of captured frames"""
//|
static mp_obj_t mipicsi_camera_pixel_format_get(mp_obj_t self_in) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return cp_enum_find(&mipicsi_pixel_format_type, common_hal_mipicsi_camera_get_pixel_format(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipicsi_camera_pixel_format_get_obj, mipicsi_camera_pixel_format_get);

MP_PROPERTY_GETTER(mipicsi_camera_pixel_format_obj,
    (mp_obj_t)&mipicsi_camera_pixel_format_get_obj);

//|     width: int
//|     """The width of the image being captured"""
//|
static mp_obj_t mipicsi_camera_width_get(mp_obj_t self_in) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(common_hal_mipicsi_camera_get_width(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipicsi_camera_width_get_obj, mipicsi_camera_width_get);

MP_PROPERTY_GETTER(mipicsi_camera_width_obj,
    (mp_obj_t)&mipicsi_camera_width_get_obj);

//|     height: int
//|     """The height of the image being captured"""
//|
static mp_obj_t mipicsi_camera_height_get(mp_obj_t self_in) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(common_hal_mipicsi_camera_get_height(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipicsi_camera_height_get_obj, mipicsi_camera_height_get);

MP_PROPERTY_GETTER(mipicsi_camera_height_obj,
    (mp_obj_t)&mipicsi_camera_height_get_obj);

//|     framebuffer_count: int
//|     """The number of framebuffers used to hold captured frames"""
//|
static mp_obj_t mipicsi_camera_framebuffer_count_get(mp_obj_t self_in) {
    mipicsi_camera_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(common_hal_mipicsi_camera_get_framebuffer_count(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipicsi_camera_framebuffer_count_get_obj, mipicsi_camera_framebuffer_count_get);

MP_PROPERTY_GETTER(mipicsi_camera_framebuffer_count_obj,
    (mp_obj_t)&mipicsi_camera_framebuffer_count_get_obj);

static const mp_rom_map_elem_t mipicsi_camera_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mipicsi_camera_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&mipicsi_camera_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame_available), MP_ROM_PTR(&mipicsi_camera_frame_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_framebuffer_count), MP_ROM_PTR(&mipicsi_camera_framebuffer_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&mipicsi_camera_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel_format), MP_ROM_PTR(&mipicsi_camera_pixel_format_obj) },
    { MP_ROM_QSTR(MP_QSTR_take), MP_ROM_PTR(&mipicsi_camera_take_obj) },
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&mipicsi_camera_width_obj) },
};

static MP_DEFINE_CONST_DICT(mipicsi_camera_locals_dict, mipicsi_camera_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mipicsi_camera_type,
    MP_QSTR_Camera,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, mipicsi_camera_make_new,
    locals_dict, &mipicsi_camera_locals_dict
    );
