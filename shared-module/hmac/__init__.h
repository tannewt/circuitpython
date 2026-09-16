// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "py/misc.h"
#include "py/obj.h"

#include "psa/crypto.h"

typedef struct {
    mp_obj_base_t base;
    // The digest algorithm the HMAC is built on, e.g. PSA_ALG_SHA_256.
    psa_algorithm_t hash_alg;
    // The PSA multipart MAC operation. update() normally streams straight
    // into this (see `buffered` below for the exception); PSA has no
    // psa_mac_clone(), so unlike shared-module/hashlib's Hash this can't be
    // rewound -- digest() finishes it exactly once and caches the result.
    psa_mac_operation_t mac_op;
    // The PSA key used by mac_op. For a bytes key, imported at construction
    // time and destroyed once mac_op is finished (owns_key true). For a key
    // borrowed from a hardwarekey.HardwareKey, that object owns the key and
    // owns_key is false.
    psa_key_id_t key_id;
    bool owns_key;
    // True for a borrowed hardware key whose PSA driver doesn't implement
    // real multipart MAC support: some hardware-backed HMAC peripherals only
    // expose a one-shot compute primitive, so a PSA driver built on one may
    // independently compute the HMAC of just each update() call's bytes and
    // discard prior state -- silently giving the wrong answer if update() is
    // called more than once. When true, update() accumulates into `buf`
    // instead and digest() feeds it to mac_op in one call. Bytes keys stream
    // normally -- this only affects the (typically short) hardware-key path.
    bool buffered;
    vstr_t buf;
    // Set once digest()/hexdigest() has finished mac_op. update() raises
    // after this; further digest() calls just return the cached bytes.
    bool finished;
    uint8_t digest[PSA_HASH_MAX_SIZE];
    size_t digest_len;
} hmac_hmac_obj_t;

// Maps a CPython digest name ("sha1", "sha256") to a PSA hash algorithm.
// Returns false for an unsupported name.
bool hmac_hash_alg_from_name(const char *name, psa_algorithm_t *hash_alg);

// Constant-time equality, matching hmac.compare_digest() / CPython's _tscmp.
bool common_hal_hmac_compare_digest(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);
