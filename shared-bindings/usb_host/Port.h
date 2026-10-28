// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/objarray.h"

#include "shared-bindings/microcontroller/Pin.h"

#include "common-hal/usb_host/Port.h"

extern const mp_obj_type_t usb_host_port_type;

// This is unique to common_hal constructs because it returns a globally stored
// object instead of taking on in that may be on the heap. This allows the
// method to check the internals of the global object against the given arguments
// to determine whether to return the singleton or raise an exception.
usb_host_port_obj_t *common_hal_usb_host_port_construct(const mcu_pin_obj_t *dp, const mcu_pin_obj_t *dm);

// Gives the host controller a buffer of at least size bytes for isochronous IN
// packets, which it then keeps between transfers and hands over as
// length-prefixed records, as usb_host_audio needs. Until it is released it
// replaces the buffer set in settings.toml. False when another isochronous
// stream holds the buffer; raises when out of memory.
bool common_hal_usb_host_port_claim_iso_buffer(size_t size);
// Frees the claimed buffer and puts back the one from settings.toml. Call after
// the stream ended. Never raises.
void common_hal_usb_host_port_release_iso_buffer(void);
