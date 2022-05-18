/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
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

#include <string.h>

#include "shared-bindings/dotenv/__init__.h"

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"
#include "py/mpstate.h"

STATIC uint8_t consume_spaces(FIL *active_file) {
    uint8_t character = ' ';
    size_t quantity_read = 1;
    while (unichar_isspace(character) && quantity_read > 0) {
        f_read(active_file, &character, 1, &quantity_read);
    }
    return character;
}

// Starting at the start of a new line, determines if the key matches the given
// key. File pointer is left after the = after the key.
STATIC bool key_matches(FIL *active_file, const char *key) {
    uint8_t character = ' ';
    size_t quantity_read = 1;
    mp_printf(&mp_plat_print, "Consuming spaces\n");
    character = consume_spaces(active_file);
    bool quoted = false;
    if (character == '\'') {
        quoted = true;
        f_read(active_file, &character, 1, &quantity_read);
    }
    size_t key_pos = 0;
    bool escaped = false;
    bool matches = true;
    size_t key_len = strlen(key);
    mp_printf(&mp_plat_print, "Finding key:");
    while (quantity_read > 0) {
        mp_printf(&mp_plat_print, "%c", character);
        if (character == '\\' && !escaped && quoted) {
            escaped = true;
        } else if (!escaped && quoted && character == '\'') {
            quoted = false;
            return matches && key_pos == key_len;
        } else if (!quoted && (unichar_isspace(character) || character == '=' || character == '\n')) {
            if (character == '=' || character == '\n') {
                // If we stop at =, then rewind one so the value can find it.
                f_lseek(active_file, f_tell(active_file) - 1);
            }
            return matches && key_pos == key_len;
        } else {
            matches = matches && key[key_pos] == character;
            mp_printf(&mp_plat_print, "+");
            escaped = false;
            key_pos++;
        }

        f_read(active_file, &character, 1, &quantity_read);
    }
    mp_printf(&mp_plat_print, "\n");
    return false;
}

STATIC bool next_line(FIL *active_file) {
    uint8_t character = ' ';
    size_t quantity_read = 1;
    bool quoted = false;
    bool escaped = false;
    FRESULT result = FR_OK;
    // Consume all characters while quoted or others up to \n.
    while (result == FR_OK && quantity_read > 0 && (quoted || character != '\n')) {
        if (!escaped) {
            if (character == '\'') {
                quoted = !quoted;
            } else if (character == '\\') {
                escaped = true;
            }
        } else {
            escaped = false;
        }
        result = f_read(active_file, &character, 1, &quantity_read);
    }
    return result == FR_OK && quantity_read > 0;
}

STATIC mp_int_t read_value(FIL *active_file, char *value, size_t value_len) {
    uint8_t character = ' ';
    size_t quantity_read = 1;
    // Consume spaces before =
    character = consume_spaces(active_file);
    if (character != '=') {
        if (character == '#' || character == '\n') {
            // Keys without an = after them are valid with the value None.
            return 0;
        }
        // All other characters are invalid.
        return -1;
    }
    character = ' ';
    // Consume space after =
    while (unichar_isspace(character) && quantity_read > 0) {
        f_read(active_file, &character, 1, &quantity_read);
    }
    bool quoted = false;
    if (character == '\'') {
        quoted = true;
        mp_printf(&mp_plat_print, "quoted\n");
        f_read(active_file, &character, 1, &quantity_read);
    }
    if (character == '"') {
        // We don't support double quoted values.
        return -1;
    }
    // Copy the value over.
    size_t value_pos = 0;
    bool escaped = false;
    while (quantity_read > 0) {
        // Consume the first \ if the value is quoted.
        if (quoted && character == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (quoted && !escaped && character == '\'') {
            // trailing ' means the value is done.
            break;
        }
        // Unquoted values are ended by a newline.
        if (!quoted && character == '\n') {
            break;
        }
        escaped = false;
        // Only copy the value over if we have space. Otherwise, we'll just
        // count the overall length.
        if (value_pos < value_len) {
            value[value_pos] = character;
        }
        value_pos++;
        f_read(active_file, &character, 1, &quantity_read);
    }

    // Done getting the value, consume anything else up to the end of the line.
    next_line(active_file);

    return value_pos;
}

mp_int_t common_hal_dotenv_get_key(const char *path, const char *key, char *value, mp_int_t value_len) {
    FIL active_file;
    FATFS *fs = &((fs_user_mount_t *)MP_STATE_VM(vfs_mount_table)->obj)->fatfs;
    FRESULT result = f_open(fs, &active_file, path, FA_READ);
    if (result != FR_OK) {
        return -1;
    }

    mp_printf(&mp_plat_print, "looking for %s\n", key);

    mp_int_t actual_value_len = -1;
    size_t line = 1;
    // TODO: Keep searching so that we return the last value.
    while (actual_value_len < 0) {
        mp_printf(&mp_plat_print, "Searching line %d @ %d\n", line, f_tell(&active_file));
        if (key_matches(&active_file, key)) {
            mp_printf(&mp_plat_print, "match!\n");
            actual_value_len = read_value(&active_file, value, value_len);
            mp_printf(&mp_plat_print, "len: %d\n", actual_value_len);
        } else if (!next_line(&active_file)) {
            break;
        }
        line++;
    }
    f_close(&active_file);
    mp_printf(&mp_plat_print, "\n");
    return actual_value_len;
}

// TODO:

bool common_hal_dotenv_set_key(const char *path, const char *key, const char *value) {
    return false;
}

bool common_hal_dotenv_unset_key(const char *path, const char *key) {
    FIL active_file;
    FATFS *fs = &((fs_user_mount_t *)MP_STATE_VM(vfs_mount_table)->obj)->fatfs;
    FRESULT result = f_open(fs, &active_file, path, FA_READ | FA_WRITE);
    if (result != FR_OK) {
        return -1;
    }

    mp_printf(&mp_plat_print, "looking for %s\n", key);

    size_t start_offset = 0;
    size_t end_offset = 0;
    size_t line = 0;
    size_t filesize = f_size(&active_file);
    while (start_offset < filesize && end_offset == 0) {
        mp_printf(&mp_plat_print, "Searching line %d @ %d\n", line, f_tell(&active_file));
        if (key_matches(&active_file, key)) {
            mp_printf(&mp_plat_print, "match!\n");
            read_value(&active_file, NULL, 0);
            end_offset = f_tell(&active_file);
            break;
        } else if (!next_line(&active_file)) {
            break;
        }
        line++;
        start_offset = f_tell(&active_file);
    }
    mp_printf(&mp_plat_print, "found between %d and %d\n", start_offset, end_offset);
    f_close(&active_file);
    mp_printf(&mp_plat_print, "\n");
    return false;
}
