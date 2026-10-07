// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/enum.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/mipicsi/__init__.h"
#include "shared-bindings/mipicsi/Camera.h"

//| """Support for MIPI CSI-2 camera interfaces
//|
//| Captures image frames from camera sensors attached to a MIPI CSI-2
//| receiver, such as the one in the ESP32-P4. The camera sensor itself is
//| configured separately (typically over I2C); this module only receives
//| the image data the sensor transmits.
//|
//| The image data arrives in the format the sensor transmits, described by
//| `PixelFormat`. For instance, a sensor transmitting RGB565 data yields
//| frames of RGB565 pixels. No format conversion or image post-processing
//| is performed.
//| """

//| class PixelFormat:
//|     """Format of the data in the captured frames. The camera sensor must be
//|     configured (typically over I2C) to transmit the corresponding MIPI CSI-2
//|     data type."""
//|
//|     RGB565: PixelFormat
//|     """A 16-bit format with 5 bits of Red, 6 bits of Green, and 5 bits of Blue (MIPI data type ``RGB565``)"""
//|
//|     RGB888: PixelFormat
//|     """A 24-bit format with 8 bits of Red, Green and Blue (MIPI data type ``RGB888``)"""
//|
//|     GRAYSCALE: PixelFormat
//|     """An 8-bit format with 8 bits of luminance (MIPI data types for 8-bit grayscale)"""
//|
//|     YUV422: PixelFormat
//|     """A 16-bit YUV 4:2:2 format, in the byte order the sensor transmits (MIPI data type ``YUV422 8-bit``)"""
//|
//|     RAW8: PixelFormat
//|     """8 bits per pixel of raw Bayer data (MIPI data type ``RAW8``)"""
//|
//|     RAW10: PixelFormat
//|     """10 bits per pixel of raw Bayer data (MIPI data type ``RAW10``)"""
//|
//|     RAW12: PixelFormat
//|     """12 bits per pixel of raw Bayer data (MIPI data type ``RAW12``)"""
//|

MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, RGB565, MIPICSI_FORMAT_RGB565);
MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, RGB888, MIPICSI_FORMAT_RGB888);
MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, GRAYSCALE, MIPICSI_FORMAT_GRAYSCALE);
MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, YUV422, MIPICSI_FORMAT_YUV422);
MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, RAW8, MIPICSI_FORMAT_RAW8);
MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, RAW10, MIPICSI_FORMAT_RAW10);
MAKE_ENUM_VALUE(mipicsi_pixel_format_type, pixel_format, RAW12, MIPICSI_FORMAT_RAW12);

MAKE_ENUM_MAP(mipicsi_pixel_format) {
    MAKE_ENUM_MAP_ENTRY(pixel_format, RGB565),
    MAKE_ENUM_MAP_ENTRY(pixel_format, RGB888),
    MAKE_ENUM_MAP_ENTRY(pixel_format, GRAYSCALE),
    MAKE_ENUM_MAP_ENTRY(pixel_format, YUV422),
    MAKE_ENUM_MAP_ENTRY(pixel_format, RAW8),
    MAKE_ENUM_MAP_ENTRY(pixel_format, RAW10),
    MAKE_ENUM_MAP_ENTRY(pixel_format, RAW12),
};

static MP_DEFINE_CONST_DICT(mipicsi_pixel_format_locals_dict, mipicsi_pixel_format_locals_table);
MAKE_PRINTER(mipicsi, mipicsi_pixel_format);
MAKE_ENUM_TYPE(mipicsi, PixelFormat, mipicsi_pixel_format);

mipicsi_pixel_format_t validate_pixel_format(mp_obj_t obj, qstr arg_name) {
    return cp_enum_value(&mipicsi_pixel_format_type, obj, arg_name);
}

static const mp_rom_map_elem_t mipicsi_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mipicsi) },
    { MP_ROM_QSTR(MP_QSTR_PixelFormat), MP_ROM_PTR(&mipicsi_pixel_format_type) },
    { MP_ROM_QSTR(MP_QSTR_Camera), MP_ROM_PTR(&mipicsi_camera_type) },
};

static MP_DEFINE_CONST_DICT(mipicsi_module_globals, mipicsi_module_globals_table);

const mp_obj_module_t mipicsi_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mipicsi_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mipicsi, mipicsi_module);
