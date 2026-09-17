// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Romain Boutrois
//
// SPDX-License-Identifier: MIT

#include "supervisor/board.h"
#include "mpconfigboard.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "lib/oofatfs/ff.h"
#include "extmod/vfs_fat.h"
#include "py/mpstate.h"
#include "supervisor/filesystem.h"

void board_init(void) {
    mp_import_stat_t stat_b = mp_import_stat("boot.py");
    if (stat_b != MP_IMPORT_STAT_FILE) {
        fs_user_mount_t *fs_mount = filesystem_circuitpy();
        FATFS *fatfs = &fs_mount->fatfs;
        FIL fs;
        UINT char_written = 0;
        // Default boot.py: CIRCUITPY is read-only over USB so children can't
        // break the robot's files from the drive, while code sent over the
        // serial REPL or BLE (Eliobot's web editor) can still write them.
        const byte buffer[] =
            "import storage\n"
            "\n"
            "# Eliobot default: the USB drive is read-only, and code sent by\n"
            "# the Elioblocs editor can write files. To edit files from the drive\n"
            "# instead, comment out the next line.\n"
            "storage.remount(\"/\", False)\n";
        f_open(fatfs, &fs, "/boot.py", FA_WRITE | FA_CREATE_ALWAYS);
        f_write(&fs, buffer, sizeof(buffer) - 1, &char_written);
        f_close(&fs);
    }
}

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.
