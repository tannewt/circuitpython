// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "shared-bindings/usb_host_bulk/InStream.h"

#include "hardware/sync.h"
#include "py/runtime.h"
#include "shared-bindings/usb/core/__init__.h"
#include "shared-bindings/usb/core/Device.h"
#include "supervisor/port_heap.h"
#include "tusb.h"

#include "pio_usb_bulk_stream.h"

#define STOP_TIMEOUT_US (100000)

// The stream the host is polling, if any. A weak link: the Python object owns
// the ring, and its finaliser clears this.
static usb_host_bulk_instream_obj_t *_active;

// A ring whose stop timed out in a finaliser. The host core may still write
// to it, so it is kept until a later stop succeeds.
static pio_usb_bulk_ring_t *_orphan_ring;
static uint8_t *_orphan_storage;

static bool _stop_orphan(void) {
    if (_orphan_ring == NULL) {
        return true;
    }
    if (!pio_usb_host_bulk_stream_stop(_orphan_ring, STOP_TIMEOUT_US)) {
        return false;
    }
    port_free(_orphan_storage);
    port_free(_orphan_ring);
    _orphan_storage = NULL;
    _orphan_ring = NULL;
    return true;
}

// Detach the ring from the host but keep it, so that bytes captured before
// the stop can still be read. No Python allocation: also used on unplug.
static bool _stop(usb_host_bulk_instream_obj_t *self) {
    if (!pio_usb_host_bulk_stream_stop(self->ring, STOP_TIMEOUT_US)) {
        return false;
    }
    if (_active == self) {
        _active = NULL;
    }
    return true;
}

void common_hal_usb_host_bulk_instream_construct(usb_host_bulk_instream_obj_t *self,
    usb_core_device_obj_t *device, uint8_t endpoint, uint32_t buffer_size) {
    if (!_stop_orphan()) {
        mp_raise_usb_core_USBTimeoutError();
    }
    if (_active != NULL) {
        if (_active->ring->active) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("Already running"));
        }
        // It ended on its own and keeps its ring until it is deinited.
        _active = NULL;
    }
    tuh_bus_info_t bus_info;
    if (!tuh_bus_info_get(device->device_address, &bus_info) || bus_info.rhport < 1 ||
        bus_info.speed != TUSB_SPEED_FULL) {
        mp_raise_usb_core_USBError(MP_ERROR_TEXT("Operation or feature not supported"));
    }
    if (!common_hal_usb_core_device_open_endpoint(device, endpoint)) {
        mp_raise_usb_core_USBError(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_endpoint);
    }
    // The host core writes both from the frame handler, so they must be in SRAM.
    pio_usb_bulk_ring_t *ring = port_malloc(sizeof(*ring), true);
    if (ring == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Could not allocate DMA capable buffer"));
    }
    memset(ring, 0, sizeof(*ring));
    uint8_t *storage = port_malloc(buffer_size, true);
    if (storage == NULL) {
        port_free(ring);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Could not allocate DMA capable buffer"));
    }
    // TinyUSB numbers the PIO root ports from 1.
    if (!pio_usb_host_bulk_stream_start(bus_info.rhport - 1, device->device_address, endpoint,
        ring, storage, buffer_size)) {
        port_free(storage);
        port_free(ring);
        mp_raise_usb_core_USBError(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_endpoint);
    }
    self->device = MP_OBJ_FROM_PTR(device);
    self->ring = ring;
    self->storage = storage;
    self->lost_packets = 0;
    self->device_address = device->device_address;
    self->endpoint = endpoint;
    _active = self;
}

bool common_hal_usb_host_bulk_instream_deinited(usb_host_bulk_instream_obj_t *self) {
    return self->device == MP_OBJ_NULL;
}

// Uses only this object's own fields: in a finaliser, the Device may already
// have been swept.
void common_hal_usb_host_bulk_instream_deinit(usb_host_bulk_instream_obj_t *self, bool abandon) {
    if (common_hal_usb_host_bulk_instream_deinited(self)) {
        return;
    }
    bool stopped = _stop(self);
    if (!stopped && !abandon) {
        mp_raise_usb_core_USBTimeoutError();
    }
    self->lost_packets = self->ring->stats.overrun_packets;
    if (stopped) {
        port_free(self->storage);
        port_free(self->ring);
    } else if (_stop_orphan()) {
        _orphan_ring = self->ring;
        _orphan_storage = self->storage;
    }
    // Otherwise two rings are stuck. Leak this one rather than free memory
    // the host core may still write to.
    if (_active == self) {
        _active = NULL;
    }
    self->ring = NULL;
    self->storage = NULL;
    self->device = MP_OBJ_NULL;
}

uint32_t common_hal_usb_host_bulk_instream_read(usb_host_bulk_instream_obj_t *self, uint8_t *data, uint32_t len) {
    return pio_usb_host_bulk_stream_read(self->ring, data, len);
}

uint32_t common_hal_usb_host_bulk_instream_get_in_waiting(usb_host_bulk_instream_obj_t *self) {
    pio_usb_bulk_ring_t *ring = self->ring;
    uint32_t const read = ring->read_pos;
    uint32_t const written = ring->write_pos;
    __dmb();
    uint32_t const available = written - read;
    return available > ring->capacity ? 0 : available;
}

uint32_t common_hal_usb_host_bulk_instream_get_lost_packets(usb_host_bulk_instream_obj_t *self) {
    if (self->ring != NULL) {
        return self->ring->stats.overrun_packets;
    }
    return self->lost_packets;
}

bool common_hal_usb_host_bulk_instream_get_ended(usb_host_bulk_instream_obj_t *self) {
    bool ended = !self->ring->active;
    // Clearing active is the host's last access, so later reads of the ring
    // see every byte it wrote.
    __dmb();
    return ended;
}

void common_hal_usb_host_bulk_instream_reset_input_buffer(usb_host_bulk_instream_obj_t *self) {
    // read_pos belongs to the consumer, so moving it is safe while capturing.
    self->ring->read_pos = self->ring->write_pos;
}

void usb_host_bulk_stop_device(uint8_t device_address) {
    if (_active != NULL && _active->device_address == device_address && !_stop(_active)) {
        mp_raise_usb_core_USBTimeoutError();
    }
}

bool usb_host_bulk_endpoint_busy(uint8_t device_address, uint8_t endpoint) {
    return _active != NULL && _active->device_address == device_address &&
           _active->endpoint == endpoint && _active->ring->active;
}

void usb_host_bulk_device_deinit(usb_core_device_obj_t *device) {
    if (_active != NULL && _active->device == MP_OBJ_FROM_PTR(device)) {
        _stop(_active);
    }
}

void usb_host_bulk_device_gone(uint8_t device_address) {
    // The host core also detaches the ring by itself once it sees the unplug.
    if (_active != NULL && _active->device_address == device_address) {
        _stop(_active);
    }
}
