# SPDX-FileCopyrightText: 2026 Mikey Sklar
# SPDX-License-Identifier: MIT

# Compare jpegio's hardware decode with TJpgDec and time both.
# Copy baseline JPEGs into /jpegtest/ first, for example 720p at 4:2:0, 4:2:2 and 4:4:4,
# a size that is not a multiple of 16, a grayscale and a progressive file.
# Opening a filename always uses TJpgDec; opening bytes uses the hardware when it can.
# decoder.used_hardware says which one the last decode() used.
# Expect small differences, since the two IDCTs round differently.

import os
import time

import displayio
import jpegio

DIR = "/jpegtest/"
decoder = jpegio.JpegDecoder()


def max_step(a, b):
    worst = 0
    differ = 0
    for i in range(0, len(a), 2):
        va = (a[i] << 8) | a[i + 1]
        vb = (b[i] << 8) | b[i + 1]
        if va != vb:
            differ += 1
            worst = max(
                worst,
                abs((va >> 11) - (vb >> 11)) * 2,
                abs(((va >> 5) & 63) - ((vb >> 5) & 63)),
                abs((va & 31) - (vb & 31)) * 2,
            )
    return differ, worst


def median_ms(source, bitmap, n=10):
    times = []
    for _ in range(n):
        t0 = time.monotonic_ns()
        decoder.open(source)
        decoder.decode(bitmap)
        times.append(time.monotonic_ns() - t0)
    times.sort()
    return times[n // 2] / 1e6


for name in sorted(os.listdir(DIR)):
    path = DIR + name
    data = open(path, "rb").read()
    try:
        width, height = decoder.open(data)
        hw = displayio.Bitmap(width, height, 65535)
        decoder.decode(hw)
        used_hardware = decoder.used_hardware
    except Exception as e:
        print(name, "raises", repr(e))
        continue
    sw = displayio.Bitmap(width, height, 65535)
    decoder.open(path)
    decoder.decode(sw)
    assert not decoder.used_hardware
    differ, worst = max_step(bytes(memoryview(sw)), bytes(memoryview(hw)))
    print(
        "%s %dx%d hardware=%s: %d of %d pixels differ, max %d steps; bytes %.1f ms, file %.1f ms"
        % (
            name,
            width,
            height,
            used_hardware,
            differ,
            width * height,
            worst,
            median_ms(data, hw),
            median_ms(path, sw, 3),
        )
    )
