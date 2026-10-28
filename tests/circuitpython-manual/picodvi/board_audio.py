# SPDX-FileCopyrightText: 2026 Adafruit Industries
#
# SPDX-License-Identifier: MIT
# A board with a default DVI display, a display attached and no display
# settings in settings.toml.
# Plays a tone on supervisor.runtime.audio, then checks it becomes
# None when the display is released.
#
# By hand: CIRCUITPY_PICODVI_AUDIO = false in settings.toml, then reset:
# supervisor.runtime.audio is None and the display is 8-bit.
import array
import math
import time

import audiocore
import displayio
import picodvi
import supervisor

audio = supervisor.runtime.audio
print("supervisor.runtime.audio", audio)
assert isinstance(audio, picodvi.AudioOut)
fb = supervisor.runtime.display.framebuffer
print("boot display", fb.width, fb.height, fb.color_depth)

tone = audiocore.RawSample(
    array.array("h", [int(math.sin(math.pi * 2 * i / 48) * 8000) for i in range(48)]),
    sample_rate=48000,
)
audio.play(tone, loop=True)
time.sleep(1)
print("playing", audio.playing)
audio.stop()
print("stopped", not audio.playing)

try:
    picodvi.AudioOut(fb)
    print("FAIL: second AudioOut on the boot display")
except RuntimeError as e:
    print("second AudioOut refused:", e)

displayio.release_displays()
print("after release", supervisor.runtime.audio)
assert supervisor.runtime.audio is None
print("done")
