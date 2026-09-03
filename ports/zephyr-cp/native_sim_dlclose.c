// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Override the host's dlclose() with a NOP on native_sim.
//
// SDL_DestroyWindow() (run from the ON_EXIT native task that cleans up the SDL
// display) makes SDL unload its EGL/GL driver with dlclose(). That runs
// libEGL's ELF destructor, which calls __glDispatchFini() and tears down
// libGLdispatch's global state.
//
// At that point the Zephyr (embedded) threads are still terminating:
// posix_soc_clean_up() calls posix_arch_clean_up(), which only *releases* the
// threads so they can pthread_exit() asynchronously, and then immediately runs
// the ON_EXIT tasks. Those exiting threads run libGLdispatch's TSD destructors
// (they have per-thread GL dispatch state because the SDL display is rendered
// from Zephyr threads), so __glDispatchFini()/dlclose() races with them.
//
// The race shows up as a segfault in libGLdispatch, a hang in
// __glDispatchFini()'s pthread_mutex_lock(), or glibc heap corruption
// ("malloc_consolidate(): unaligned fastbin chunk detected") - all of which
// kill the process before the ON_EXIT_POST task that re-execs us for
// sys_reboot(). Tests that reboot (e.g. test_saved_word) then fail flakily
// because the simulator never comes back.
//
// Never unloading a host library avoids the whole class of teardown races: the
// process is about to exit or be replaced by execv(), so unmapping libraries
// buys us nothing. Zephyr does the same thing for ASan builds
// (CONFIG_ASAN_NOP_DLCLOSE), which is why the crash disappears there.
//
// A symbol defined in the executable takes precedence over the one in
// libc/libdl, so this also covers calls made from inside SDL.

int dlclose(void *handle) {
    (void)handle;
    return 0;
}
