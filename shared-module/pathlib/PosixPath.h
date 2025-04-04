#pragma once

#include "py/obj.h"
#include "py/objstr.h"

typedef struct {
    mp_obj_base_t base;
    mp_obj_t path_str;
} pathlib_posixpath_obj_t;

// Forward declaration
extern const mp_obj_type_t pathlib_posixpath_type;

// Helper functions
mp_obj_t pathlib_posixpath_from_str(mp_obj_t path_str);
mp_obj_t mp_obj_new_path_dir_iter(mp_obj_t path, mp_obj_t iter);

// Common HAL functions
mp_obj_t common_hal_pathlib_posixpath_new(mp_obj_t path);
mp_obj_t common_hal_pathlib_posixpath_joinpath(pathlib_posixpath_obj_t *self, mp_obj_t arg);
mp_obj_t common_hal_pathlib_posixpath_parent(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_name(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_stem(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_suffix(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_exists(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_is_dir(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_is_file(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_absolute(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_resolve(pathlib_posixpath_obj_t *self);
mp_obj_t common_hal_pathlib_posixpath_iterdir(pathlib_posixpath_obj_t *self);
