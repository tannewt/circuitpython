/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 hathach for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "shared-bindings/usb_midi/__init__.h"

#include "genhdr/autogen_usb_descriptor.h"
#include "py/obj.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/objtuple.h"
#include "shared-bindings/usb_midi/PortIn.h"
#include "shared-bindings/usb_midi/PortOut.h"
#include "supervisor/memory.h"
#include "tusb.h"

supervisor_allocation* usb_midi_allocation;

void usb_midi_init(void) {
    // TODO(tannewt): Make this dynamic.
    uint16_t tuple_size = align32_size(sizeof(mp_obj_tuple_t) + sizeof(mp_obj_t*) * 2);
    uint16_t portin_size = align32_size(sizeof(usb_midi_portin_obj_t));
    uint16_t portout_size = align32_size(sizeof(usb_midi_portout_obj_t));

    // For each embedded MIDI Jack in the descriptor we create a Port
    usb_midi_allocation = allocate_memory(tuple_size + portin_size + portout_size, false);

    mp_obj_tuple_t *ports = (mp_obj_tuple_t *) usb_midi_allocation->ptr;
    ports->base.type = &mp_type_tuple;
    ports->len = 2;

    usb_midi_portin_obj_t* in = (usb_midi_portin_obj_t *) (usb_midi_allocation->ptr + tuple_size / 4);
    in->base.type = &usb_midi_portin_type;
    ports->items[0] = MP_OBJ_FROM_PTR(in);

    usb_midi_portout_obj_t* out = (usb_midi_portout_obj_t *) (usb_midi_allocation->ptr + tuple_size / 4 + portin_size / 4);
    out->base.type = &usb_midi_portout_type;
    ports->items[1] = MP_OBJ_FROM_PTR(out);

    mp_map_lookup(&usb_midi_module_globals.map, MP_ROM_QSTR(MP_QSTR_ports), MP_MAP_LOOKUP)->value = MP_OBJ_FROM_PTR(ports);
}
