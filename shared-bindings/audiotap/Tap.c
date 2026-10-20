// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audiotap/Tap.h"
#include "shared-bindings/audiocore/__init__.h"

#include "shared/runtime/context_manager_helpers.h"
#include "py/binary.h"
#include "py/objproperty.h"
#include "py/runtime.h"
#include "shared-bindings/util.h"

//| class Tap:
//|     """Passes audio through unchanged and keeps a copy that the program can read"""
//|
//|     def __init__(
//|         self,
//|         capacity: int = 4096,
//|         buffer_size: int = 512,
//|         sample_rate: int = 8000,
//|         bits_per_sample: int = 16,
//|         samples_signed: bool = True,
//|         channel_count: int = 1,
//|     ) -> None:
//|         """Create a Tap. It plays a sample like the effects in `audiofilters`
//|         and `audiodelays` do, so it can go anywhere in an audio chain. Whatever
//|         plays the tap gets the sample's audio unchanged.
//|
//|         As the audio passes through, the tap keeps it in a buffer of
//|         ``capacity`` samples, which `readinto` copies out of without waiting,
//|         and measures its levels for `levels`. The program can look at the
//|         audio this way, at the point in the chain where the tap is, while it
//|         plays.
//|
//|         The tap only sees audio that is played through it: something, such
//|         as an `audiomixer.Mixer` voice playing to an audio output, must play
//|         it or a chain that it is part of. To look at audio without hearing
//|         it, play it at a ``level`` of 0. Until its sample is played, and
//|         after it ends, the tap plays silence and keeps nothing.
//|
//|         When the program reads too slowly, the buffer fills up, the tap keeps
//|         the audio it has and drops the newer audio until there is room
//|         again, and `overflow` turns True. The audio that passes through is
//|         never affected.
//|
//|         :param int capacity: The number of samples kept for `readinto`, rounded
//|           down to whole frames. A stereo frame is two samples. 0 keeps none,
//|           for a tap that only measures `levels`.
//|         :param int buffer_size: The total size in bytes of each of the two playback buffers to use
//|         :param int sample_rate: The sample rate to be used
//|         :param int bits_per_sample: The bits per sample of the audio: 8 or 16
//|         :param bool samples_signed: Effect is signed (True) or unsigned (False)
//|         :param int channel_count: The number of channels the source samples contain. 1 = mono; 2 = stereo.
//|
//|         Showing the level of a USB microphone while it is heard through the
//|         DVI output, and keeping the audio for a spectrum display. The
//|         ``adafruit_usb_host_descriptors`` library finds the microphone and
//|         its format, and `usb.core` starts its stream; see
//|         `usb_host_audio.USBIn`. DVI audio plays at 32, 44.1 or 48 kHz, so an
//|         `audiospeed.Resampler` converts other sample rates::
//|
//|           import array
//|           import time
//|
//|           import adafruit_usb_host_descriptors
//|           import audiomixer
//|           import audiospeed
//|           import audiotap
//|           import displayio
//|           import supervisor
//|           import usb_host_audio
//|           import vectorio
//|
//|           device = adafruit_usb_host_descriptors.find_microphone()
//|           if device is None:
//|               raise RuntimeError("Plug a USB microphone or webcam into the USB host port")
//|           formats = adafruit_usb_host_descriptors.find_microphone_formats(device)
//|           mic_format = adafruit_usb_host_descriptors.choose_microphone_format(formats, sample_rate=48000)
//|           sample_rate = mic_format.nearest_rate(48000)
//|
//|           device.set_configuration()
//|           # SET_INTERFACE to the format's alternate setting starts the stream, then
//|           # SET_CUR of the endpoint's sampling frequency picks the rate.
//|           device.ctrl_transfer(0x01, 11, mic_format.alternate, mic_format.interface, None)
//|           if mic_format.rate_control:
//|               device.ctrl_transfer(
//|                   0x22, 0x01, 0x0100, mic_format.endpoint, sample_rate.to_bytes(3, "little")
//|               )
//|
//|           mic = usb_host_audio.USBIn(
//|               device,
//|               mic_format.endpoint,
//|               sample_rate=sample_rate,
//|               channel_count=mic_format.channel_count,
//|               bit_depth=8 * mic_format.bytes_per_sample,
//|               samples_signed=mic_format.samples_signed,
//|               max_packet_size=mic_format.max_packet_size,
//|           )
//|           # Keep a quarter second, more than arrives between reads of the loop below.
//|           tap = audiotap.Tap(
//|               capacity=sample_rate * mic.channel_count // 4,
//|               sample_rate=sample_rate,
//|               channel_count=mic.channel_count,
//|           )
//|           mixer = audiomixer.Mixer(voice_count=1, sample_rate=48000, channel_count=mic.channel_count)
//|           supervisor.runtime.audio.play(mixer)
//|           mixer.voice[0].play(audiospeed.Resampler(tap.play(mic)))
//|
//|           # Bars for the peak (top) and RMS (bottom) levels. Showing them hides the
//|           # console, whose scrolling stalls the microphone's stream. print() still
//|           # goes to the serial console.
//|           display = supervisor.runtime.display
//|           palette = displayio.Palette(1)
//|           palette[0] = 0x00FF00
//|           peak_bar = vectorio.Rectangle(pixel_shader=palette, width=1, height=40, x=0, y=60)
//|           rms_bar = vectorio.Rectangle(pixel_shader=palette, width=1, height=40, x=0, y=140)
//|           meter = displayio.Group()
//|           meter.append(peak_bar)
//|           meter.append(rms_bar)
//|           display.root_group = meter
//|
//|           samples = array.array("h", [0] * 1024)
//|           while True:
//|               # Read the audio that is waiting. A spectrum display would use these samples.
//|               while tap.available >= len(samples):
//|                   tap.readinto(samples)
//|               peak, rms = tap.levels()
//|               peak_bar.width = max(1, int(peak * display.width))
//|               rms_bar.width = max(1, int(rms * display.width))
//|               print(f"peak {peak:.3f} rms {rms:.3f}")
//|               time.sleep(0.1)"""
//|         ...
//|
static mp_obj_t audiotap_tap_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_capacity, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_capacity, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 4096} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 512} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1 } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t capacity = mp_arg_validate_int_range(args[ARG_capacity].u_int, 0, 1 << 24, MP_QSTR_capacity);
    mp_int_t buffer_size = mp_arg_validate_int_range(args[ARG_buffer_size].u_int, 4, 1 << 16, MP_QSTR_buffer_size);
    mp_int_t channel_count = mp_arg_validate_int_range(args[ARG_channel_count].u_int, 1, 2, MP_QSTR_channel_count);
    mp_int_t sample_rate = mp_arg_validate_int_min(args[ARG_sample_rate].u_int, 1, MP_QSTR_sample_rate);
    mp_int_t bits_per_sample = args[ARG_bits_per_sample].u_int;
    if (bits_per_sample != 8 && bits_per_sample != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits_per_sample must be 8 or 16"));
    }

    audiotap_tap_obj_t *self = mp_obj_malloc_with_finaliser(audiotap_tap_obj_t, &audiotap_tap_type);
    common_hal_audiotap_tap_construct(self, capacity, buffer_size, bits_per_sample,
        args[ARG_samples_signed].u_bool, channel_count, sample_rate);

    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Deinitialises the Tap."""
//|         ...
//|
static mp_obj_t audiotap_tap_deinit(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiotap_tap_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_deinit_obj, audiotap_tap_deinit);

static void check_for_deinit(audiotap_tap_obj_t *self) {
    audiosample_check_for_deinit(&self->base);
}

//|     def __enter__(self) -> Tap:
//|         """No-op used by Context Managers."""
//|         ...
//|
//  Provided by context manager helper.

//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context. See
//|         :ref:`lifetime-and-contextmanagers` for more info."""
//|         ...
//|
//  Provided by context manager helper.

//|     playing: bool
//|     """True when the tap is playing a sample. (read-only)"""
//|
static mp_obj_t audiotap_tap_obj_get_playing(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiotap_tap_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_get_playing_obj, audiotap_tap_obj_get_playing);

MP_PROPERTY_GETTER(audiotap_tap_playing_obj,
    (mp_obj_t)&audiotap_tap_get_playing_obj);

//|     capacity: int
//|     """The number of samples that the tap keeps for `readinto`. (read-only)"""
//|
static mp_obj_t audiotap_tap_obj_get_capacity(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(common_hal_audiotap_tap_get_capacity(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_get_capacity_obj, audiotap_tap_obj_get_capacity);

MP_PROPERTY_GETTER(audiotap_tap_capacity_obj,
    (mp_obj_t)&audiotap_tap_get_capacity_obj);

//|     available: int
//|     """The number of samples waiting to be read with `readinto`. (read-only)"""
//|
static mp_obj_t audiotap_tap_obj_get_available(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(common_hal_audiotap_tap_get_available(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_get_available_obj, audiotap_tap_obj_get_available);

MP_PROPERTY_GETTER(audiotap_tap_available_obj,
    (mp_obj_t)&audiotap_tap_get_available_obj);

//|     overflow: bool
//|     """True if audio was dropped since the last time this was checked,
//|     because `readinto` did not keep up. Reading this clears it. (read-only)"""
//|
static mp_obj_t audiotap_tap_obj_get_overflow(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiotap_tap_get_overflow(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_get_overflow_obj, audiotap_tap_obj_get_overflow);

MP_PROPERTY_GETTER(audiotap_tap_overflow_obj,
    (mp_obj_t)&audiotap_tap_get_overflow_obj);

//|     def readinto(self, buffer: WriteableBuffer) -> int:
//|         """Copies the oldest audio waiting into ``buffer`` and returns the number
//|         of samples copied, which is 0 when none is waiting. Does not wait for
//|         audio. Copies whole frames only: a stereo frame is two samples, left
//|         then right.
//|
//|         The samples are copied as they are played, so ``buffer`` is
//|         usually an ``array.array`` of typecode ``'h'`` for 16 bit signed
//|         samples. A ``bytearray`` works for any format and holds half as
//|         many 16 bit samples as its length. To read into part of a buffer,
//|         pass a slice of a ``memoryview`` of it."""
//|         ...
//|
static mp_obj_t audiotap_tap_obj_readinto(mp_obj_t self_in, mp_obj_t buffer_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_in, &bufinfo, MP_BUFFER_WRITE);
    uint32_t length = bufinfo.len / (self->base.bits_per_sample / 8);
    return mp_obj_new_int_from_uint(common_hal_audiotap_tap_readinto(self, bufinfo.buf, length));
}
MP_DEFINE_CONST_FUN_OBJ_2(audiotap_tap_readinto_obj, audiotap_tap_obj_readinto);

//|     def clear(self) -> None:
//|         """Discards the audio waiting, so that `readinto` returns only audio
//|         that passes through after this. Use it to skip ahead to the live
//|         audio, for example after an `overflow`."""
//|         ...
//|
static mp_obj_t audiotap_tap_obj_clear(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_audiotap_tap_clear(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_clear_obj, audiotap_tap_obj_clear);

//|     def levels(self) -> Tuple[float, float]:
//|         """Returns the peak and RMS level of the audio that passed through
//|         since the last call, from 0.0 to 1.0 of full scale, and starts a new
//|         measurement. Both are 0.0 when no audio passed through. The channels
//|         of stereo audio are measured together. This works with a ``capacity``
//|         of 0 too."""
//|         ...
//|
static mp_obj_t audiotap_tap_obj_levels(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_float_t peak, rms;
    common_hal_audiotap_tap_get_levels(self, &peak, &rms);
    mp_obj_t levels[2] = { mp_obj_new_float(peak), mp_obj_new_float(rms) };
    return mp_obj_new_tuple(2, levels);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_levels_obj, audiotap_tap_obj_levels);

//|     def play(self, sample: circuitpython_typing.AudioSample, *, loop: bool = False) -> Tap:
//|         """Plays the sample once when loop=False and continuously when loop=True.
//|         Does not block. Use `playing` to block.
//|
//|         The sample must match the encoding settings given in the constructor.
//|
//|         :return: The tap itself. Can be used for chaining, ie:
//|           ``audio.play(tap.play(sample))``.
//|         :rtype: Tap"""
//|         ...
//|
static mp_obj_t audiotap_tap_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    common_hal_audiotap_tap_play(self, args[ARG_sample].u_obj, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiotap_tap_play_obj, 1, audiotap_tap_obj_play);

//|     def stop(self) -> None:
//|         """Stops playback of the sample. The audio already kept can still be read."""
//|         ...
//|
//|
static mp_obj_t audiotap_tap_obj_stop(mp_obj_t self_in) {
    audiotap_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_audiotap_tap_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiotap_tap_stop_obj, audiotap_tap_obj_stop);

static const mp_rom_map_elem_t audiotap_tap_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiotap_tap_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiotap_tap_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiotap_tap_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiotap_tap_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&audiotap_tap_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audiotap_tap_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_levels), MP_ROM_PTR(&audiotap_tap_levels_obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiotap_tap_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_capacity), MP_ROM_PTR(&audiotap_tap_capacity_obj) },
    { MP_ROM_QSTR(MP_QSTR_available), MP_ROM_PTR(&audiotap_tap_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_overflow), MP_ROM_PTR(&audiotap_tap_overflow_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiotap_tap_locals_dict, audiotap_tap_locals_dict_table);

static const audiosample_p_t audiotap_tap_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiotap_tap_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiotap_tap_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiotap_tap_type,
    MP_QSTR_Tap,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiotap_tap_make_new,
    locals_dict, &audiotap_tap_locals_dict,
    protocol, &audiotap_tap_proto
    );
